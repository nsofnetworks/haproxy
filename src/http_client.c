/*
 * HTTP Client
 *
 * Copyright (C) 2021 HAProxy Technologies, William Lallemand <wlallemand@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This file implements an HTTP Client API.
 *
 */

#include <haproxy/api.h>
#include <haproxy/applet.h>
#include <haproxy/cli.h>
#include <haproxy/ssl_ckch.h>
#include <haproxy/dynbuf.h>
#include <haproxy/cfgparse.h>
#include <haproxy/global.h>
#include <haproxy/istbuf.h>
#include <haproxy/h1_htx.h>
#include <haproxy/http.h>
#include <haproxy/http_ana-t.h>
#include <haproxy/http_client.h>
#include <haproxy/http_htx.h>
#include <haproxy/http_rules.h>
#include <haproxy/htx.h>
#include <haproxy/log.h>
#include <haproxy/proxy.h>
#include <haproxy/resolvers.h>
#include <haproxy/sc_strm.h>
#include <haproxy/server.h>
#include <haproxy/ssl_sock-t.h>
#include <haproxy/sock_inet.h>
#include <haproxy/stconn.h>
#include <haproxy/tools.h>

#include <string.h>


static struct proxy *httpclient_proxy;
static struct server *httpclient_srv_raw;

#ifdef USE_OPENSSL
/* if the httpclient is not configured, error are ignored and features are limited */
static int hard_error_ssl = 0;
static struct server *httpclient_srv_ssl;
static int httpclient_ssl_verify = SSL_SOCK_VERIFY_REQUIRED;
static char *httpclient_ssl_ca_file = NULL;
#endif
static struct applet httpclient_applet;

/* if the httpclient is not configured, error are ignored and features are limited */
static int hard_error_resolvers = 0;
static char *resolvers_id = NULL;
static char *resolvers_prefer = NULL;

/* --- This part of the file implement an HTTP client over the CLI ---
 * The functions will be  starting by "hc_cli" for "httpclient cli"
 */

/* What kind of data we need to read */
#define HC_CLI_F_RES_STLINE     0x01
#define HC_CLI_F_RES_HDR        0x02
#define	HC_CLI_F_RES_BODY       0x04
#define HC_CLI_F_RES_END        0x08

/* the CLI context for the httpclient command */
struct hcli_svc_ctx {
	struct httpclient *hc;  /* the httpclient instance */
	uint flags;             /* flags from HC_CLI_F_* above */
};

/* These are the callback used by the HTTP Client when it needs to notify new
 * data, we only sets a flag in the IO handler via the svcctx.
 */
void hc_cli_res_stline_cb(struct httpclient *hc)
{
	struct appctx *appctx = hc->caller;
	struct hcli_svc_ctx *ctx;

	if (!appctx)
		return;

	ctx = appctx->svcctx;
	ctx->flags |= HC_CLI_F_RES_STLINE;
	appctx_wakeup(appctx);
}

void hc_cli_res_headers_cb(struct httpclient *hc)
{
	struct appctx *appctx = hc->caller;
	struct hcli_svc_ctx *ctx;

	if (!appctx)
		return;

	ctx = appctx->svcctx;
	ctx->flags |= HC_CLI_F_RES_HDR;
	appctx_wakeup(appctx);
}

void hc_cli_res_body_cb(struct httpclient *hc)
{
	struct appctx *appctx = hc->caller;
	struct hcli_svc_ctx *ctx;

	if (!appctx)
		return;

	ctx = appctx->svcctx;
	ctx->flags |= HC_CLI_F_RES_BODY;
	appctx_wakeup(appctx);
}

void hc_cli_res_end_cb(struct httpclient *hc)
{
	struct appctx *appctx = hc->caller;
	struct hcli_svc_ctx *ctx;

	if (!appctx)
		return;

	ctx = appctx->svcctx;
	ctx->flags |= HC_CLI_F_RES_END;
	appctx_wakeup(appctx);
}

/*
 * Parse an httpclient keyword on the cli:
 * httpclient <ID> <method> <URI>
 */
static int hc_cli_parse(char **args, char *payload, struct appctx *appctx, void *private)
{
	struct hcli_svc_ctx *ctx = applet_reserve_svcctx(appctx, sizeof(*ctx));
	struct httpclient *hc;
	char *err = NULL;
	enum http_meth_t meth;
	char *meth_str;
	struct ist uri;
	struct ist body = IST_NULL;

	if (!cli_has_level(appctx, ACCESS_LVL_ADMIN))
		return 1;

	if (!*args[1] || !*args[2]) {
		memprintf(&err, ": not enough parameters");
		goto err;
	}

	meth_str = args[1];
	uri = ist(args[2]);

	if (payload)
		body = ist(payload);

	meth = find_http_meth(meth_str, strlen(meth_str));

	hc = httpclient_new(appctx, meth, uri);
	if (!hc) {
		goto err;
	}

	/* update the httpclient callbacks */
	hc->ops.res_stline = hc_cli_res_stline_cb;
	hc->ops.res_headers = hc_cli_res_headers_cb;
	hc->ops.res_payload = hc_cli_res_body_cb;
	hc->ops.res_end = hc_cli_res_end_cb;

	ctx->hc = hc; /* store the httpclient ptr in the applet */
	ctx->flags = 0;

	if (httpclient_req_gen(hc, hc->req.url, hc->req.meth, NULL, body) != ERR_NONE)
		goto err;


	if (!httpclient_start(hc))
		goto err;

	return 0;

err:
	memprintf(&err, "Can't start the HTTP client%s.\n", err ? err : "");
	return cli_err(appctx, err);
}

/* This function dumps the content of the httpclient receive buffer
 * on the CLI output
 *
 * Return 1 when the processing is finished
 * return 0 if it needs to be called again
 */
static int hc_cli_io_handler(struct appctx *appctx)
{
	struct hcli_svc_ctx *ctx = appctx->svcctx;
	struct stconn *sc = appctx_sc(appctx);
	struct httpclient *hc = ctx->hc;
	struct http_hdr *hdrs, *hdr;

	if (ctx->flags & HC_CLI_F_RES_STLINE) {
		chunk_printf(&trash, "%.*s %d %.*s\n", (unsigned int)istlen(hc->res.vsn), istptr(hc->res.vsn),
			     hc->res.status, (unsigned int)istlen(hc->res.reason), istptr(hc->res.reason));
		if (applet_putchk(appctx, &trash) == -1)
			goto more;
		ctx->flags &= ~HC_CLI_F_RES_STLINE;
	}

	if (ctx->flags & HC_CLI_F_RES_HDR) {
		chunk_reset(&trash);
		hdrs = hc->res.hdrs;
		for (hdr = hdrs; isttest(hdr->v); hdr++) {
			if (!h1_format_htx_hdr(hdr->n, hdr->v, &trash))
				goto too_many_hdrs;
		}
		if (!chunk_memcat(&trash, "\r\n", 2))
			goto too_many_hdrs;
		if (applet_putchk(appctx, &trash) == -1)
			goto more;
		ctx->flags &= ~HC_CLI_F_RES_HDR;
	}

	if (ctx->flags & HC_CLI_F_RES_BODY) {
		int ret;

		ret = httpclient_res_xfer(hc, sc_ib(sc));
		channel_add_input(sc_ic(sc), ret); /* forward what we put in the buffer channel */

		/* remove the flag if the buffer was emptied */
		if (httpclient_data(hc))
			goto more;
		ctx->flags &= ~HC_CLI_F_RES_BODY;
	}

	/* we must close only if F_END is the last flag */
	if (ctx->flags ==  HC_CLI_F_RES_END) {
		ctx->flags &= ~HC_CLI_F_RES_END;
		goto end;
	}

more:
	if (!ctx->flags)
		applet_have_no_more_data(appctx);
	return 0;
end:
	return 1;

too_many_hdrs:
	return cli_err(appctx, "Too many headers.\n");
}

static void hc_cli_release(struct appctx *appctx)
{
	struct hcli_svc_ctx *ctx = appctx->svcctx;
	struct httpclient *hc = ctx->hc;

	/* Everything possible was printed on the CLI, we can destroy the client */
	httpclient_stop_and_destroy(hc);

	return;
}

/* register cli keywords */
static struct cli_kw_list cli_kws = {{ },{
	{ { "httpclient", NULL }, "httpclient <method> <URI>               : launch an HTTP request", hc_cli_parse, hc_cli_io_handler, hc_cli_release,  NULL, ACCESS_EXPERT},
	{ { NULL }, NULL, NULL, NULL }
}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);


/* --- This part of the file implements the actual HTTP client API --- */

/*
 * Generate a simple request and fill the httpclient request buffer with it.
 * The request contains a request line generated from the absolute <url> and
 * <meth> as well as list of headers <hdrs>.
 *
 * If the buffer was filled correctly the function returns 0, if not it returns
 * an error_code but there is no guarantee that the buffer wasn't modified.
 */
int httpclient_req_gen(struct httpclient *hc, const struct ist url, enum http_meth_t meth, const struct http_hdr *hdrs, const struct ist payload)
{
	struct htx_sl *sl;
	struct htx *htx;
	int err_code = 0;
	struct ist meth_ist, vsn;
	unsigned int flags = HTX_SL_F_VER_11 | HTX_SL_F_NORMALIZED_URI | HTX_SL_F_HAS_SCHM;
	int i;
	int foundhost = 0, foundaccept = 0, foundua = 0;

	if (!b_alloc(&hc->req.buf))
		goto error;

	if (meth >= HTTP_METH_OTHER)
		goto error;

	meth_ist = http_known_methods[meth];

	vsn = ist("HTTP/1.1");

	htx = htx_from_buf(&hc->req.buf);
	if (!htx)
		goto error;

	if (!hc->ops.req_payload && !isttest(payload))
		flags |= HTX_SL_F_BODYLESS;

	sl = htx_add_stline(htx, HTX_BLK_REQ_SL, flags, meth_ist, url, vsn);
	if (!sl) {
		goto error;
	}
	sl->info.req.meth = meth;

	for (i = 0; hdrs && hdrs[i].n.len; i++) {
		/* Don't check the value length because a header value may be empty */
		if (isttest(hdrs[i].v) == 0)
			continue;

		if (isteqi(hdrs[i].n, ist("host")))
			foundhost = 1;
		else if (isteqi(hdrs[i].n, ist("accept")))
			foundaccept = 1;
		else if (isteqi(hdrs[i].n, ist("user-agent")))
			foundua = 1;

		if (!htx_add_header(htx, hdrs[i].n, hdrs[i].v))
			goto error;
	}

	if (!foundhost) {
		/* Add Host Header from URL */
		if (!htx_add_header(htx, ist("Host"), ist("h")))
			goto error;
		if (!http_update_host(htx, sl, url))
			goto error;
	}

	if (!foundaccept) {
		if (!htx_add_header(htx, ist("Accept"), ist("*/*")))
			goto error;
	}

	if (!foundua) {
		if (!htx_add_header(htx, ist("User-Agent"), ist(HTTPCLIENT_USERAGENT)))
			goto error;
	}


	if (!htx_add_endof(htx, HTX_BLK_EOH))
		goto error;

	if (isttest(payload)) {
		/* add the payload if it can feat in the buffer, no need to set
		 * the Content-Length, the data will be sent chunked */
		if (!htx_add_data_atonce(htx, payload))
			goto error;
	}

	/* If req.payload was set, does not set the end of stream which *MUST*
	 * be set in the callback */
	if (!hc->ops.req_payload)
		htx->flags |= HTX_FL_EOM;

	htx_to_buf(htx, &hc->req.buf);

	return 0;
error:
	err_code |= ERR_ALERT | ERR_ABORT;
	return err_code;
}

/*
 * transfer the response to the destination buffer and wakeup the HTTP client
 * applet so it could fill again its buffer.
 *
 * Return the number of bytes transferred.
 */
int httpclient_res_xfer(struct httpclient *hc, struct buffer *dst)
{
	size_t room = b_room(dst);
	int ret;

	ret = b_force_xfer(dst, &hc->res.buf, MIN(room, b_data(&hc->res.buf)));
	/* call the client once we consumed all data */
	if (!b_data(&hc->res.buf)) {
		b_free(&hc->res.buf);
		if (hc->appctx)
			appctx_wakeup(hc->appctx);
	}
	return ret;
}

/*
 * Transfer raw HTTP payload from src, and insert it into HTX format in the
 * httpclient.
 *
 * Must be used to transfer the request body.
 * Then wakeup the httpclient so it can transfer it.
 *
 * <end> tries to add the ending data flag if it succeed to copy all data.
 *
 * Return the number of bytes copied from src.
 */
int httpclient_req_xfer(struct httpclient *hc, struct ist src, int end)
{
	int ret = 0;
	struct htx *htx;

	if (!b_alloc(&hc->req.buf))
		goto error;

	htx = htx_from_buf(&hc->req.buf);
	if (!htx)
		goto error;

	if (hc->appctx)
		appctx_wakeup(hc->appctx);

	ret += htx_add_data(htx, src);


	/* if we copied all the data and the end flag is set */
	if ((istlen(src) == ret) && end) {
		/* no more data are expected. If the HTX buffer is empty, be
		 * sure to add something (EOT block in this case) to have
		 * something to send. It is important to be sure the EOM flags
		 * will be handled by the endpoint. Because the message is
		 * empty, this should not fail. Otherwise it is an error
		 */
		if (htx_is_empty(htx)) {
			if (!htx_add_endof(htx, HTX_BLK_EOT))
				goto error;
		}
		htx->flags |= HTX_FL_EOM;
	}
	htx_to_buf(htx, &hc->req.buf);

error:

	return ret;
}

/* Set the 'timeout server' in ms for the next httpclient request */
void httpclient_set_timeout(struct httpclient *hc, int timeout)
{
	hc->timeout_server = timeout;
}

/*
 * Sets a destination for the httpclient from an HAProxy addr format
 * This will prevent to determine the destination from the URL
 * Return 0 in case of success or -1 otherwise.
 */
int httpclient_set_dst(struct httpclient *hc, const char *dst)
{
	struct sockaddr_storage *sk;
	char *errmsg = NULL;

	sockaddr_free(&hc->dst);
	/* 'sk' is statically allocated (no need to be freed). */
	sk = str2sa_range(dst, NULL, NULL, NULL, NULL, NULL,
	                  &errmsg, NULL, NULL,
	                  PA_O_PORT_OK | PA_O_STREAM | PA_O_XPRT | PA_O_CONNECT);
	if (!sk) {
		ha_alert("httpclient: Failed to parse destination address in %s\n", errmsg);
		free(errmsg);
		return -1;
	}

	if (!sockaddr_alloc(&hc->dst, sk, sizeof(*sk))) {
		ha_alert("httpclient: Failed to allocate sockaddr in %s:%d.\n", __FUNCTION__, __LINE__);
		return -1;
	}

	return 0;
}

/*
 * Return a split URL in <scheme>, <host>, <port>
 */
static void httpclient_spliturl(struct ist url, enum http_scheme *scheme,
                                struct ist *host, int *port)
{
	enum http_scheme scheme_tmp = SCH_HTTP;
	int port_tmp = 0;
	struct ist scheme_ist, authority_ist, host_ist, port_ist;
	char *p, *end;
	struct http_uri_parser parser;

	parser = http_uri_parser_init(url);
	scheme_ist = http_parse_scheme(&parser);

	if (isteqi(scheme_ist, ist("http://"))){
		scheme_tmp = SCH_HTTP;
		port_tmp = 80;
	} else if (isteqi(scheme_ist, ist("https://"))) {
		scheme_tmp = SCH_HTTPS;
		port_tmp = 443;
	}

	authority_ist = http_parse_authority(&parser, 1);
	p = end = istend(authority_ist);

	/* look for a port at the end of the authority */
	while (p > istptr(authority_ist) && isdigit((unsigned char)*--p))
		;

	if (*p == ':') {
		host_ist = ist2(istptr(authority_ist), p - istptr(authority_ist));
		port_ist = istnext(ist2(p, end - p));
		ist2str(trash.area, port_ist);
		port_tmp = atoi(trash.area);
	} else {
		host_ist = authority_ist;
	}

	if (scheme)
		*scheme = scheme_tmp;
	if (host)
		*host = host_ist;
	if (port)
		*port = port_tmp;

}

/*
 * Start the HTTP client
 * Create the appctx, session, stream and wakeup the applet
 *
 * Return the <appctx> or NULL if it failed
 */
struct appctx *httpclient_start(struct httpclient *hc)
{
	struct applet *applet = &httpclient_applet;
	struct appctx *appctx;

	/* if the client was started and not ended, an applet is already
	 * running, we shouldn't try anything */
	if (httpclient_started(hc) && !httpclient_ended(hc))
		return NULL;

	/* The HTTP client will be created in the same thread as the caller,
	 * avoiding threading issues */
	appctx = appctx_new_here(applet, NULL);
	if (!appctx)
		goto out;
	appctx->svcctx = hc;
	hc->flags = 0;

	if (appctx_init(appctx) == -1) {
		ha_alert("httpclient: Failed to initialize appctx %s:%d.\n", __FUNCTION__, __LINE__);
		goto out_free_appctx;
	}

	return appctx;

out_free_appctx:
	appctx_free_on_early_error(appctx);
out:

	return NULL;
}

/*
 * This function tries to destroy the httpclient if it wasn't running.
 * If it was running, stop the client and ask it to autodestroy itself.
 *
 * Once this function is used, all pointer sto the client must be removed
 *
 */
void httpclient_stop_and_destroy(struct httpclient *hc)
{

	/* The httpclient was already stopped or never started, we can safely destroy it */
	if (hc->flags & HTTPCLIENT_FS_ENDED || !(hc->flags & HTTPCLIENT_FS_STARTED)) {
		httpclient_destroy(hc);
	} else {
		/* if the client wasn't stopped, ask for a stop and destroy */
		hc->flags |= (HTTPCLIENT_FA_AUTOKILL | HTTPCLIENT_FA_STOP);
		/* the calling applet doesn't exist anymore */
		hc->caller = NULL;
		if (hc->appctx)
			appctx_wakeup(hc->appctx);
	}
}

/* Free the httpclient */
void httpclient_destroy(struct httpclient *hc)
{
	struct http_hdr *hdrs;


	if (!hc)
		return;

	/* we should never destroy a client which was started but not stopped  */
	BUG_ON(httpclient_started(hc) && !httpclient_ended(hc));

	/* request */
	istfree(&hc->req.url);
	b_free(&hc->req.buf);
	/* response */
	istfree(&hc->res.vsn);
	istfree(&hc->res.reason);
	hdrs = hc->res.hdrs;
	while (hdrs && isttest(hdrs->n)) {
		istfree(&hdrs->n);
		istfree(&hdrs->v);
		hdrs++;
	}
	ha_free(&hc->res.hdrs);
	b_free(&hc->res.buf);
	sockaddr_free(&hc->dst);

	free(hc);

	return;
}

/* Allocate an httpclient and its buffers
 * Return NULL on failure */
struct httpclient *httpclient_new(void *caller, enum http_meth_t meth, struct ist url)
{
	struct httpclient *hc;

	hc = calloc(1, sizeof(*hc));
	if (!hc)
		goto err;

	hc->req.buf = BUF_NULL;
	hc->res.buf = BUF_NULL;
	hc->caller = caller;
	hc->req.url = istdup(url);
	hc->req.meth = meth;

	return hc;

err:
	httpclient_destroy(hc);
	return NULL;
}

static void httpclient_applet_io_handler(struct appctx *appctx)
{
	struct httpclient *hc = appctx->svcctx;
	struct stconn *sc = appctx_sc(appctx);
	struct stream *s = __sc_strm(sc);
	struct channel *req = &s->req;
	struct channel *res = &s->res;
	struct htx_blk *blk = NULL;
	struct htx *htx;
	struct htx_sl *sl = NULL;
	uint32_t hdr_num;
	uint32_t sz;
	int ret;

	while (1) {

		/* required to stop */
		if (hc->flags & HTTPCLIENT_FA_STOP)
			goto end;

		switch(appctx->st0) {

			case HTTPCLIENT_S_REQ:
				/* we know that the buffer is empty here, since
				 * it's the first call, we can freely copy the
				 * request from the httpclient buffer */
				ret = b_xfer(&req->buf, &hc->req.buf, b_data(&hc->req.buf));
				if (!ret)
					goto more;

				if (!b_data(&hc->req.buf))
					b_free(&hc->req.buf);

				htx = htx_from_buf(&req->buf);
				if (!htx)
					goto more;

				channel_add_input(req, htx->data);

				if (htx->flags & HTX_FL_EOM) /* check if a body need to be added */
					appctx->st0 = HTTPCLIENT_S_RES_STLINE;
				else
					appctx->st0 = HTTPCLIENT_S_REQ_BODY;

				goto more; /* we need to leave the IO handler once we wrote the request */
			break;
			case HTTPCLIENT_S_REQ_BODY:
				/* call the payload callback */
				{
					if (hc->ops.req_payload) {
						struct htx *hc_htx;

						/* call the request callback */
						hc->ops.req_payload(hc);

						hc_htx = htx_from_buf(&hc->req.buf);
						htx = htx_from_buf(&req->buf);

						if (htx_is_empty(hc_htx))
							goto more;

						if (htx_is_empty(htx)) {
							size_t data = hc_htx->data;

							/* Here htx_to_buf() will set buffer data to 0 because
							 * the HTX is empty, and allow us to do an xfer.
							 */
							htx_to_buf(hc_htx, &hc->req.buf);
							htx_to_buf(htx, &req->buf);
							b_xfer(&req->buf, &hc->req.buf, b_data(&hc->req.buf));
							channel_add_input(req, data);
						} else {
							struct htx_ret ret;

							ret = htx_xfer_blks(htx, hc_htx, htx_used_space(hc_htx), HTX_BLK_UNUSED);
							channel_add_input(req, ret.ret);

							/* we must copy the EOM if we empty the buffer */
							if (htx_is_empty(hc_htx)) {
								htx->flags |= (hc_htx->flags & HTX_FL_EOM);
							}
							htx_to_buf(htx, &req->buf);
							htx_to_buf(hc_htx, &hc->req.buf);
						}


						if (!b_data(&hc->req.buf))
							b_free(&hc->req.buf);
					}

					htx = htx_from_buf(&req->buf);
					if (!htx)
						goto more;

					/* if the request contains the HTX_FL_EOM, we finished the request part. */
					if (htx->flags & HTX_FL_EOM) {
						req->flags |= CF_EOI;
						se_fl_set(appctx->sedesc, SE_FL_EOI);
						appctx->st0 = HTTPCLIENT_S_RES_STLINE;
					}

					goto process_data; /* we need to leave the IO handler once we wrote the request */
				}
			break;

			case HTTPCLIENT_S_RES_STLINE:
				/* copy the start line in the hc structure,then remove the htx block */
				if (!co_data(res))
					goto more;
				htx = htxbuf(&res->buf);
				if (!htx)
					goto more;
				blk = htx_get_head_blk(htx);
				if (blk && (htx_get_blk_type(blk) == HTX_BLK_RES_SL))
					sl = htx_get_blk_ptr(htx, blk);
				if (!sl || (!(sl->flags & HTX_SL_F_IS_RESP)))
					goto more;

				/* copy the status line in the httpclient */
				hc->res.status = sl->info.res.status;
				hc->res.vsn = istdup(htx_sl_res_vsn(sl));
				hc->res.reason = istdup(htx_sl_res_reason(sl));
				sz = htx_get_blksz(blk);
				c_rew(res, sz);
				htx_remove_blk(htx, blk);
				/* caller callback */
				if (hc->ops.res_stline)
					hc->ops.res_stline(hc);

				/* if there is no HTX data anymore and the EOM flag is
				 * set, leave (no body) */
				if (htx_is_empty(htx) && htx->flags & HTX_FL_EOM)
					appctx->st0 = HTTPCLIENT_S_RES_END;
				else
					appctx->st0 = HTTPCLIENT_S_RES_HDR;
				break;

			case HTTPCLIENT_S_RES_HDR:
				/* first copy the headers in a local hdrs
				 * structure, once we the total numbers of the
				 * header we allocate the right size and copy
				 * them. The htx block of the headers are
				 * removed each time one is read  */
				{
					struct http_hdr hdrs[global.tune.max_http_hdr];

					if (!co_data(res))
						goto more;
					htx = htxbuf(&res->buf);
					if (!htx)
						goto more;

					hdr_num = 0;
					blk = htx_get_head_blk(htx);
					while (blk) {
						enum htx_blk_type type = htx_get_blk_type(blk);
						uint32_t sz = htx_get_blksz(blk);

						c_rew(res, sz);

						if (type == HTX_BLK_HDR) {
							hdrs[hdr_num].n = istdup(htx_get_blk_name(htx, blk));
							hdrs[hdr_num].v = istdup(htx_get_blk_value(htx, blk));
							hdr_num++;
						}
						else if (type == HTX_BLK_EOH) {
							/* create a NULL end of array and leave the loop */
							hdrs[hdr_num].n = IST_NULL;
							hdrs[hdr_num].v = IST_NULL;
							htx_remove_blk(htx, blk);
							break;
						}
						blk = htx_remove_blk(htx, blk);
					}

					if (hdr_num) {
						/* alloc and copy the headers in the httpclient struct */
						hc->res.hdrs = calloc((hdr_num + 1), sizeof(*hc->res.hdrs));
						if (!hc->res.hdrs)
							goto end;
						memcpy(hc->res.hdrs, hdrs, sizeof(struct http_hdr) * (hdr_num + 1));

						/* caller callback */
						if (hc->ops.res_headers)
							hc->ops.res_headers(hc);
					}

					/* if there is no HTX data anymore and the EOM flag is
					 * set, leave (no body) */
					if (htx_is_empty(htx) && htx->flags & HTX_FL_EOM) {
						appctx->st0 = HTTPCLIENT_S_RES_END;
					} else {
						appctx->st0 = HTTPCLIENT_S_RES_BODY;
					}
				}
			break;

			case HTTPCLIENT_S_RES_BODY:
				/*
				 * The IO handler removes the htx blocks in the response buffer and
				 * push them in the hc->res.buf buffer in a raw format.
				 */
				if (!co_data(res))
					goto more;

				htx = htxbuf(&res->buf);
				if (!htx || htx_is_empty(htx))
					goto more;

				if (!b_alloc(&hc->res.buf))
					goto more;

				if (b_full(&hc->res.buf))
					goto process_data;

				/* decapsule the htx data to raw data */
				blk = htx_get_head_blk(htx);
				while (blk) {
					enum htx_blk_type type = htx_get_blk_type(blk);
					size_t count = co_data(res);
					uint32_t blksz = htx_get_blksz(blk);
					uint32_t room = b_room(&hc->res.buf);
					uint32_t vlen;

					/* we should try to copy the maximum output data in a block, which fit
					 * the destination buffer */
					vlen = MIN(count, blksz);
					vlen = MIN(vlen, room);

					if (vlen == 0)
						goto process_data;

					if (type == HTX_BLK_DATA) {
						struct ist v = htx_get_blk_value(htx, blk);

						__b_putblk(&hc->res.buf, v.ptr, vlen);
						c_rew(res, vlen);

						if (vlen == blksz)
							blk = htx_remove_blk(htx, blk);
						else
							htx_cut_data_blk(htx, blk, vlen);

						/* the data must be processed by the caller in the receive phase */
						if (hc->ops.res_payload)
							hc->ops.res_payload(hc);

						/* cannot copy everything, need to process */
						if (vlen != blksz)
							goto process_data;
					} else {
						if (vlen != blksz)
							goto process_data;

						/* remove any block which is not a data block */
						c_rew(res, blksz);
						blk = htx_remove_blk(htx, blk);
					}
				}

				/* if not finished, should be called again */
				if (!(htx_is_empty(htx) && (htx->flags & HTX_FL_EOM)))
					goto more;


				/* end of message, we should quit */
				appctx->st0 = HTTPCLIENT_S_RES_END;
			break;

			case HTTPCLIENT_S_RES_END:
				goto end;
			break;
		}
	}

process_data:

	sc_will_read(sc);

	return;
more:
	/* There was not enough data in the response channel */

	sc_need_room(sc);

	if (appctx->st0 == HTTPCLIENT_S_RES_END)
		goto end;

	/* The state machine tries to handle as much data as possible, if there
	 * isn't any data to handle and a shutdown is detected, let's stop
	 * everything */
	if ((req->flags & (CF_SHUTR|CF_SHUTR_NOW)) ||
	    (res->flags & CF_SHUTW) ||
	    ((res->flags & CF_SHUTW_NOW) && channel_is_empty(res))) {
		goto end;
	}
	return;

end:
	sc_shutw(sc);
	sc_shutr(sc);
	return;
}

static int httpclient_applet_init(struct appctx *appctx)
{
	struct httpclient *hc = appctx->svcctx;
	struct stream *s;
	struct sockaddr_storage *addr = NULL;
	struct sockaddr_storage ss_url = {};
	struct sockaddr_storage *ss_dst;
	enum obj_type *target = NULL;
	struct ist host = IST_NULL;
	enum http_scheme scheme;
	int port;
	int doresolve = 0;


	/* parse the URL and  */
	httpclient_spliturl(hc->req.url, &scheme, &host, &port);

	if (hc->dst) {
		/* if httpclient_set_dst() was used, sets the alternative address */
		ss_dst = hc->dst;
	} else {
		/* set the dst using the host, or 0.0.0.0 to resolve */
		ist2str(trash.area, host);
		ss_dst = str2ip2(trash.area, &ss_url, 0);
		if (!ss_dst) { /* couldn't get an IP from that, try to resolve */
			doresolve = 1;
			ss_dst = str2ip2("0.0.0.0", &ss_url, 0);
		}
		sock_inet_set_port(ss_dst, port);
	}

	if (!sockaddr_alloc(&addr, ss_dst, sizeof(*ss_dst)))
		goto out_error;

	/* choose the SSL server or not */
	switch (scheme) {
		case SCH_HTTP:
			target = &httpclient_srv_raw->obj_type;
			break;
		case SCH_HTTPS:
#ifdef USE_OPENSSL
			if (httpclient_srv_ssl) {
				target = &httpclient_srv_ssl->obj_type;
			} else {
				ha_alert("httpclient: SSL was disabled (wrong verify/ca-file)!\n");
				goto out_free_addr;
			}
#else
			ha_alert("httpclient: OpenSSL is not available %s:%d.\n", __FUNCTION__, __LINE__);
			goto out_free_addr;
#endif
			break;
	}

	if (appctx_finalize_startup(appctx, httpclient_proxy, &hc->req.buf) == -1) {
		ha_alert("httpclient: Failed to initialize appctx %s:%d.\n", __FUNCTION__, __LINE__);
		goto out_free_addr;
	}

	s = appctx_strm(appctx);
	s->target = target;
	/* set the "timeout server" */
	s->req.wto = hc->timeout_server;
	s->res.rto = hc->timeout_server;

	if (doresolve) {
		/* in order to do the set-dst we need to put the address on the front */
		s->scf->dst = addr;
	} else {
		/* in cases we don't use the resolve we already have the address
		 * and must put it on the backend side, some of the cases are
		 * not meant to be used on the frontend (sockpair, unix socket etc.) */
		s->scb->dst = addr;
	}

	s->scb->flags |= SC_FL_NOLINGER;
	s->flags |= SF_ASSIGNED;
	s->res.flags |= CF_READ_DONTWAIT;

	/* applet is waiting for data */
	applet_need_more_data(appctx);
	appctx_wakeup(appctx);

	hc->appctx = appctx;
	hc->flags |= HTTPCLIENT_FS_STARTED;

	/* The request was transferred when the stream was created. So switch
	 * directly to REQ_BODY or RES_STLINE state
	 */
	appctx->st0 = (hc->ops.req_payload ? HTTPCLIENT_S_REQ_BODY : HTTPCLIENT_S_RES_STLINE);
	return 0;

 out_free_addr:
	sockaddr_free(&addr);
 out_error:
	return -1;
}

static void httpclient_applet_release(struct appctx *appctx)
{
	struct httpclient *hc = appctx->svcctx;

	/* mark the httpclient as ended */
	hc->flags |= HTTPCLIENT_FS_ENDED;
	/* the applet is leaving, remove the ptr so we don't try to call it
	 * again from the caller */
	hc->appctx = NULL;

	if (hc->ops.res_end)
		hc->ops.res_end(hc);

	/* destroy the httpclient when set to autotokill */
	if (hc->flags & HTTPCLIENT_FA_AUTOKILL) {
		httpclient_destroy(hc);
	}

	return;
}

/* HTTP client applet */
static struct applet httpclient_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<HTTPCLIENT>",
	.fct = httpclient_applet_io_handler,
	.init = httpclient_applet_init,
	.release = httpclient_applet_release,
};


static int httpclient_resolve_init()
{
	struct act_rule *rule;
	int i;
	char *do_resolve = NULL;
	char *http_rules[][11] = {
	       { "set-var(txn.hc_ip)", "dst", "" },
	       { do_resolve, "hdr(Host),host_only", "if", "{", "var(txn.hc_ip)", "-m", "ip", "0.0.0.0", "}", "" },
	       { "return", "status", "503", "if", "{", "var(txn.hc_ip)", "-m", "ip", "0.0.0.0", "}", "" },
	       { "capture", "var(txn.hc_ip)", "len", "40", "" },
	       { "set-dst", "var(txn.hc_ip)", "" },
	       { "" }
	};

	if (!resolvers_id)
		resolvers_id = strdup("default");

	memprintf(&do_resolve, "do-resolve(txn.hc_ip,%s%s%s)", resolvers_id, resolvers_prefer ? "," : "", resolvers_prefer ? resolvers_prefer : "");
	http_rules[1][0] = do_resolve;

	/* Try to create the default resolvers section */
	resolvers_create_default();

	/* if the resolver does not exist and no hard_error was set, simply ignore resolving */
	if (!find_resolvers_by_id(resolvers_id) && !hard_error_resolvers) {
		free(do_resolve);
		return 0;
	}


	for (i = 0; *http_rules[i][0] != '\0'; i++) {
		rule = parse_http_req_cond((const char **)http_rules[i], "httpclient", 0, httpclient_proxy);
		if (!rule) {
			free(do_resolve);
			ha_alert("Couldn't setup the httpclient resolver.\n");
			return 1;
		}
		LIST_APPEND(&httpclient_proxy->http_req_rules, &rule->list);
	}

	free(do_resolve);
	return 0;
}



/*
 * Initialize the proxy for the HTTP client with 2 servers, one for raw HTTP,
 * the other for HTTPS.
 */
static int httpclient_precheck()
{
	int err_code = ERR_NONE;
	char *errmsg = NULL;

	if (global.mode & MODE_MWORKER_WAIT)
		return ERR_NONE;

	httpclient_proxy = alloc_new_proxy("<HTTPCLIENT>", PR_CAP_LISTEN|PR_CAP_INT, &errmsg);
	if (!httpclient_proxy) {
		memprintf(&errmsg, "couldn't allocate proxy.");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}

	proxy_preset_defaults(httpclient_proxy);

	httpclient_proxy->options |= PR_O_WREQ_BODY;
	httpclient_proxy->retry_type |= PR_RE_CONN_FAILED | PR_RE_DISCONNECTED | PR_RE_TIMEOUT;
	httpclient_proxy->options2 |= PR_O2_INDEPSTR;
	httpclient_proxy->mode = PR_MODE_HTTP;
	httpclient_proxy->maxconn = 0;
	httpclient_proxy->accept = NULL;
	httpclient_proxy->conn_retries = CONN_RETRIES;
	httpclient_proxy->timeout.client = TICK_ETERNITY;
	/* The HTTP Client use the "option httplog" with the global log server */
	httpclient_proxy->conf.logformat_string = default_http_log_format;
	httpclient_proxy->http_needed = 1;

	/* clear HTTP server */
	httpclient_srv_raw = new_server(httpclient_proxy);
	if (!httpclient_srv_raw) {
		memprintf(&errmsg, "out of memory.");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}

	srv_settings_cpy(httpclient_srv_raw, &httpclient_proxy->defsrv, 0);
	httpclient_srv_raw->iweight = 0;
	httpclient_srv_raw->uweight = 0;
	httpclient_srv_raw->xprt = xprt_get(XPRT_RAW);
	httpclient_srv_raw->flags |= SRV_F_MAPPORTS;  /* needed to apply the port change with resolving */
	httpclient_srv_raw->id = strdup("<HTTPCLIENT>");
	if (!httpclient_srv_raw->id) {
		memprintf(&errmsg, "out of memory.");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}

#ifdef USE_OPENSSL
	/* SSL HTTP server */
	httpclient_srv_ssl = new_server(httpclient_proxy);
	if (!httpclient_srv_ssl) {
		memprintf(&errmsg, "out of memory.");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}
	srv_settings_cpy(httpclient_srv_ssl, &httpclient_proxy->defsrv, 0);
	httpclient_srv_ssl->iweight = 0;
	httpclient_srv_ssl->uweight = 0;
	httpclient_srv_ssl->xprt = xprt_get(XPRT_SSL);
	httpclient_srv_ssl->use_ssl = 1;
	httpclient_srv_ssl->flags |= SRV_F_MAPPORTS;  /* needed to apply the port change with resolving */
	httpclient_srv_ssl->id = strdup("<HTTPSCLIENT>");
	if (!httpclient_srv_ssl->id) {
		memprintf(&errmsg, "out of memory.");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}

	httpclient_srv_ssl->ssl_ctx.verify = httpclient_ssl_verify;
	/* if the verify is required, try to load the system CA */
	if (httpclient_ssl_verify == SSL_SOCK_VERIFY_REQUIRED) {

		if (!httpclient_ssl_ca_file)
			httpclient_ssl_ca_file = strdup("@system-ca");

		httpclient_srv_ssl->ssl_ctx.ca_file = httpclient_ssl_ca_file;
		if (!ssl_store_load_locations_file(httpclient_srv_ssl->ssl_ctx.ca_file, 1, CAFILE_CERT)) {
			/* if we failed to load the ca-file, only quits in
			 * error with hard_error, otherwise just disable the
			 * feature. */
			if (hard_error_ssl) {
				memprintf(&errmsg, "cannot initialize SSL verify with 'ca-file \"%s\"'.", httpclient_srv_ssl->ssl_ctx.ca_file);
				err_code |= ERR_ALERT | ERR_FATAL;
				goto err;
			} else {
				ha_free(&httpclient_srv_ssl->ssl_ctx.ca_file);
				srv_drop(httpclient_srv_ssl);
				httpclient_srv_ssl = NULL;
			}
		}
	}

#endif

	/* add the proxy in the proxy list only if everything is successful */
	httpclient_proxy->next = proxies_list;
	proxies_list = httpclient_proxy;

	if (httpclient_resolve_init() != 0) {
		memprintf(&errmsg, "cannot initialize resolvers.");
		err_code |= ERR_ALERT | ERR_FATAL;
		goto err;
	}

	/* link the 2 servers in the proxy */
	httpclient_srv_raw->next = httpclient_proxy->srv;
	httpclient_proxy->srv = httpclient_srv_raw;

#ifdef USE_OPENSSL
	if (httpclient_srv_ssl) {
		httpclient_srv_ssl->next = httpclient_proxy->srv;
		httpclient_proxy->srv = httpclient_srv_ssl;
	}
#endif


err:
	if (err_code & ERR_CODE) {
		ha_alert("httpclient: cannot initialize: %s\n", errmsg);
		free(errmsg);
		srv_drop(httpclient_srv_raw);
#ifdef USE_OPENSSL
		srv_drop(httpclient_srv_ssl);
#endif
		free_proxy(httpclient_proxy);
	}
	return err_code;
}

static int httpclient_postcheck()
{
	int err_code = ERR_NONE;
	struct logsrv *logsrv;
	struct proxy *curproxy = httpclient_proxy;
	char *errmsg = NULL;

	if (global.mode & MODE_MWORKER_WAIT)
		return ERR_NONE;

	/* copy logs from "global" log list */
	list_for_each_entry(logsrv, &global.logsrvs, list) {
		struct logsrv *node = malloc(sizeof(*node));

		if (!node) {
			memprintf(&errmsg, "out of memory.");
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}

		memcpy(node, logsrv, sizeof(*node));
		LIST_INIT(&node->list);
		LIST_APPEND(&curproxy->logsrvs, &node->list);
		node->ring_name = logsrv->ring_name ? strdup(logsrv->ring_name) : NULL;
		node->conf.file = logsrv->conf.file ? strdup(logsrv->conf.file) : NULL;
	}
	if (curproxy->conf.logformat_string) {
		curproxy->conf.args.ctx = ARGC_LOG;
		if (!parse_logformat_string(curproxy->conf.logformat_string, curproxy, &curproxy->logformat,
					    LOG_OPT_MANDATORY|LOG_OPT_MERGE_SPACES,
					    SMP_VAL_FE_LOG_END, &errmsg)) {
			memprintf(&errmsg, "failed to parse log-format : %s.", errmsg);
			err_code |= ERR_ALERT | ERR_FATAL;
			goto err;
		}
		curproxy->conf.args.file = NULL;
		curproxy->conf.args.line = 0;
	}

#ifdef USE_OPENSSL
	if (httpclient_srv_ssl) {
		/* init the SNI expression */
		/* always use the host header as SNI, without the port */
		httpclient_srv_ssl->sni_expr = strdup("req.hdr(host),field(1,:)");
		err_code |= server_parse_sni_expr(httpclient_srv_ssl, httpclient_proxy, &errmsg);
		if (err_code & ERR_CODE) {
			memprintf(&errmsg, "failed to configure sni: %s.", errmsg);
			goto err;
		}
	}
#endif

err:
	if (err_code & ERR_CODE) {
		ha_alert("httpclient: failed to initialize: %s\n", errmsg);
		free(errmsg);

	}
	return err_code;
}

/* initialize the proxy and servers for the HTTP client */

REGISTER_PRE_CHECK(httpclient_precheck);
REGISTER_POST_CHECK(httpclient_postcheck);

static int httpclient_parse_global_resolvers(char **args, int section_type, struct proxy *curpx,
                                        const struct proxy *defpx, const char *file, int line,
                                        char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	/* any configuration should set the hard_error flag */
	hard_error_resolvers = 1;

	free(resolvers_id);
	resolvers_id = strdup(args[1]);

	return 0;
}

static int httpclient_parse_global_prefer(char **args, int section_type, struct proxy *curpx,
                                        const struct proxy *defpx, const char *file, int line,
                                        char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	/* any configuration should set the hard_error flag */
	hard_error_resolvers = 1;


	if (strcmp(args[1],"ipv4") == 0)
		resolvers_prefer = "ipv4";
	else if (strcmp(args[1],"ipv6") == 0)
		resolvers_prefer = "ipv6";
	else {
		ha_alert("parsing [%s:%d] : '%s' expects 'ipv4' or 'ipv6' as argument.\n", file, line, args[0]);
		return -1;
	}

	return 0;
}


#ifdef USE_OPENSSL
static int httpclient_parse_global_ca_file(char **args, int section_type, struct proxy *curpx,
                                        const struct proxy *defpx, const char *file, int line,
                                        char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	/* any configuration should set the hard_error flag */
	hard_error_ssl = 1;

	free(httpclient_ssl_ca_file);
	httpclient_ssl_ca_file = strdup(args[1]);

	return 0;
}

static int httpclient_parse_global_verify(char **args, int section_type, struct proxy *curpx,
                                        const struct proxy *defpx, const char *file, int line,
                                        char **err)
{
	if (too_many_args(1, args, err, NULL))
		return -1;

	/* any configuration should set the hard_error flag */
	hard_error_ssl = 1;

	if (strcmp(args[1],"none") == 0)
		httpclient_ssl_verify = SSL_SOCK_VERIFY_NONE;
	else if (strcmp(args[1],"required") == 0)
		httpclient_ssl_verify = SSL_SOCK_VERIFY_REQUIRED;
	else {
		ha_alert("parsing [%s:%d] : '%s' expects 'none' or 'required' as argument.\n", file, line, args[0]);
		return -1;
	}

	return 0;
}
#endif /* ! USE_OPENSSL */

static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "httpclient.resolvers.id", httpclient_parse_global_resolvers },
	{ CFG_GLOBAL, "httpclient.resolvers.prefer", httpclient_parse_global_prefer },
#ifdef USE_OPENSSL
	{ CFG_GLOBAL, "httpclient.ssl.verify", httpclient_parse_global_verify },
	{ CFG_GLOBAL, "httpclient.ssl.ca-file", httpclient_parse_global_ca_file },
#endif
	{ 0, NULL, NULL },
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);
