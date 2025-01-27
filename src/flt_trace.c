/*
 * Stream filters related variables and functions.
 *
 * Copyright (C) 2015 Qualys Inc., Christopher Faulet <cfaulet@qualys.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>

#include <common/hathreads.h>
#include <common/htx.h>
#include <common/initcall.h>
#include <common/standard.h>
#include <common/time.h>
#include <common/tools.h>

#include <types/channel.h>
#include <types/filters.h>
#include <types/global.h>
#include <types/proxy.h>
#include <types/stream.h>

#include <proto/filters.h>
#include <proto/hdr_idx.h>
#include <proto/http_htx.h>
#include <proto/log.h>
#include <proto/proto_http.h>
#include <proto/stream.h>

const char *trace_flt_id = "trace filter";

struct flt_ops trace_ops;

struct trace_config {
	struct proxy *proxy;
	char         *name;
	int           rand_parsing;
	int           rand_forwarding;
	int           hexdump;
};

#define TRACE(conf, fmt, ...)						\
	fprintf(stderr, "%d.%06d [%-20s] " fmt "\n",			\
		(int)now.tv_sec, (int)now.tv_usec, (conf)->name,	\
		##__VA_ARGS__)

#define STRM_TRACE(conf, strm, fmt, ...)						\
	fprintf(stderr, "%d.%06d [%-20s] [strm %p(%x) 0x%08x 0x%08x] " fmt "\n",	\
		(int)now.tv_sec, (int)now.tv_usec, (conf)->name,			\
		strm, (strm ? ((struct stream *)strm)->uniq_id : ~0U),			\
		(strm ? strm->req.analysers : 0), (strm ? strm->res.analysers : 0),	\
		##__VA_ARGS__)


static const char *
channel_label(const struct channel *chn)
{
	return (chn->flags & CF_ISRESP) ? "RESPONSE" : "REQUEST";
}

static const char *
proxy_mode(const struct stream *s)
{
	struct proxy *px = (s->flags & SF_BE_ASSIGNED ? s->be : strm_fe(s));

	return ((px->mode == PR_MODE_HTTP)
		? (IS_HTX_STRM(s) ? "HTX" : "HTTP")
		: "TCP");
}

static const char *
stream_pos(const struct stream *s)
{
	return (s->flags & SF_BE_ASSIGNED) ? "backend" : "frontend";
}

static const char *
filter_type(const struct filter *f)
{
	return (f->flags & FLT_FL_IS_BACKEND_FILTER) ? "backend" : "frontend";
}

static void
trace_hexdump(struct ist ist)
{
	int i, j, padding;

	padding = ((ist.len % 16) ? (16 - ist.len % 16) : 0);
	for (i = 0; i < ist.len + padding; i++) {
                if (!(i % 16))
                        fprintf(stderr, "\t0x%06x: ", i);
		else if (!(i % 8))
                        fprintf(stderr, "  ");

                if (i < ist.len)
                        fprintf(stderr, "%02x ", (unsigned char)*(ist.ptr+i));
                else
                        fprintf(stderr, "   ");

                /* print ASCII dump */
                if (i % 16 == 15) {
                        fprintf(stderr, "  |");
                        for(j = i - 15; j <= i && j < ist.len; j++)
				fprintf(stderr, "%c", (isprint(*(ist.ptr+j)) ? *(ist.ptr+j) : '.'));
                        fprintf(stderr, "|\n");
                }
        }
}

static void
trace_raw_hexdump(struct buffer *buf, int len, int out)
{
	unsigned char p[len];
	int block1, block2;

	block1 = len;
	if (block1 > b_contig_data(buf, out))
		block1 = b_contig_data(buf, out);
	block2 = len - block1;

	memcpy(p, b_head(buf), block1);
	memcpy(p+block1, b_orig(buf), block2);
	trace_hexdump(ist2(p, len));
}

static void
trace_htx_hexdump(struct htx *htx, unsigned int offset, unsigned int len)
{
	struct htx_blk *blk;

	for (blk = htx_get_first_blk(htx); blk && len; blk = htx_get_next_blk(htx, blk)) {
		enum htx_blk_type type = htx_get_blk_type(blk);
		uint32_t sz = htx_get_blksz(blk);
		struct ist v;

		if (offset >= sz) {
			offset -= sz;
			continue;
		}

		v = htx_get_blk_value(htx, blk);
		v.ptr += offset;
		v.len -= offset;
		offset = 0;

		if (v.len > len)
			v.len = len;
		len -= v.len;
		if (type == HTX_BLK_DATA)
			trace_hexdump(v);
	}
}

/***************************************************************************
 * Hooks that manage the filter lifecycle (init/check/deinit)
 **************************************************************************/
/* Initialize the filter. Returns -1 on error, else 0. */
static int
trace_init(struct proxy *px, struct flt_conf *fconf)
{
	struct trace_config *conf = fconf->conf;

	if (conf->name)
		memprintf(&conf->name, "%s/%s", conf->name, px->id);
	else
		memprintf(&conf->name, "TRACE/%s", px->id);

	fconf->flags |= FLT_CFG_FL_HTX;
	fconf->conf = conf;

	TRACE(conf, "filter initialized [read random=%s - fwd random=%s - hexdump=%s]",
	      (conf->rand_parsing ? "true" : "false"),
	      (conf->rand_forwarding ? "true" : "false"),
	      (conf->hexdump ? "true" : "false"));
	return 0;
}

/* Free ressources allocated by the trace filter. */
static void
trace_deinit(struct proxy *px, struct flt_conf *fconf)
{
	struct trace_config *conf = fconf->conf;

	if (conf) {
		TRACE(conf, "filter deinitialized");
		free(conf->name);
		free(conf);
	}
	fconf->conf = NULL;
}

/* Check configuration of a trace filter for a specified proxy.
 * Return 1 on error, else 0. */
static int
trace_check(struct proxy *px, struct flt_conf *fconf)
{
	return 0;
}

/* Initialize the filter for each thread. Return -1 on error, else 0. */
static int
trace_init_per_thread(struct proxy *px, struct flt_conf *fconf)
{
	struct trace_config *conf = fconf->conf;

	TRACE(conf, "filter initialized for thread tid %u", tid);
	return 0;
}

/* Free ressources allocate by the trace filter for each thread. */
static void
trace_deinit_per_thread(struct proxy *px, struct flt_conf *fconf)
{
	struct trace_config *conf = fconf->conf;

	if (conf)
		TRACE(conf, "filter deinitialized for thread tid %u", tid);
}

/**************************************************************************
 * Hooks to handle start/stop of streams
 *************************************************************************/
/* Called when a filter instance is created and attach to a stream */
static int
trace_attach(struct stream *s, struct filter *filter)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: filter-type=%s",
		   __FUNCTION__, filter_type(filter));

	return 1;
}

/* Called when a filter instance is detach from a stream, just before its
 * destruction */
static void
trace_detach(struct stream *s, struct filter *filter)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: filter-type=%s",
		   __FUNCTION__, filter_type(filter));
}

/* Called when a stream is created */
static int
trace_stream_start(struct stream *s, struct filter *filter)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s",
		   __FUNCTION__);
	return 0;
}


/* Called when a backend is set for a stream */
static int
trace_stream_set_backend(struct stream *s, struct filter *filter,
			 struct proxy *be)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: backend=%s",
		   __FUNCTION__, be->id);
	return 0;
}

/* Called when a stream is destroyed */
static void
trace_stream_stop(struct stream *s, struct filter *filter)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s",
		   __FUNCTION__);
}

/* Called when the stream is woken up because of an expired timer */
static void
trace_check_timeouts(struct stream *s, struct filter *filter)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s",
		   __FUNCTION__);
}

/**************************************************************************
 * Hooks to handle channels activity
 *************************************************************************/
/* Called when analyze starts for a given channel */
static int
trace_chn_start_analyze(struct stream *s, struct filter *filter,
			struct channel *chn)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s));
	filter->pre_analyzers  |= (AN_REQ_ALL | AN_RES_ALL);
	filter->post_analyzers |= (AN_REQ_ALL | AN_RES_ALL);
	register_data_filter(s, chn, filter);
	return 1;
}

/* Called before a processing happens on a given channel */
static int
trace_chn_analyze(struct stream *s, struct filter *filter,
		  struct channel *chn, unsigned an_bit)
{
	struct trace_config *conf = FLT_CONF(filter);
	char                *ana;

	switch (an_bit) {
		case AN_REQ_INSPECT_FE:
			ana = "AN_REQ_INSPECT_FE";
			break;
		case AN_REQ_WAIT_HTTP:
			ana = "AN_REQ_WAIT_HTTP";
			break;
		case AN_REQ_HTTP_BODY:
			ana = "AN_REQ_HTTP_BODY";
			break;
		case AN_REQ_HTTP_PROCESS_FE:
			ana = "AN_REQ_HTTP_PROCESS_FE";
			break;
		case AN_REQ_SWITCHING_RULES:
			ana = "AN_REQ_SWITCHING_RULES";
			break;
		case AN_REQ_INSPECT_BE:
			ana = "AN_REQ_INSPECT_BE";
			break;
		case AN_REQ_HTTP_PROCESS_BE:
			ana = "AN_REQ_HTTP_PROCESS_BE";
			break;
		case AN_REQ_SRV_RULES:
			ana = "AN_REQ_SRV_RULES";
			break;
		case AN_REQ_HTTP_INNER:
			ana = "AN_REQ_HTTP_INNER";
			break;
		case AN_REQ_HTTP_TARPIT:
			ana = "AN_REQ_HTTP_TARPIT";
			break;
		case AN_REQ_STICKING_RULES:
			ana = "AN_REQ_STICKING_RULES";
			break;
		case AN_REQ_PRST_RDP_COOKIE:
			ana = "AN_REQ_PRST_RDP_COOKIE";
			break;
		case AN_REQ_HTTP_XFER_BODY:
			ana = "AN_REQ_HTTP_XFER_BODY";
			break;
		case AN_RES_INSPECT:
			ana = "AN_RES_INSPECT";
			break;
		case AN_RES_WAIT_HTTP:
			ana = "AN_RES_WAIT_HTTP";
			break;
		case AN_RES_HTTP_PROCESS_FE: // AN_RES_HTTP_PROCESS_BE
			ana = "AN_RES_HTTP_PROCESS_FE/BE";
			break;
		case AN_RES_STORE_RULES:
			ana = "AN_RES_STORE_RULES";
			break;
		case AN_RES_HTTP_XFER_BODY:
			ana = "AN_RES_HTTP_XFER_BODY";
			break;
		default:
			ana = "unknown";
	}

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - "
		   "analyzer=%s - step=%s",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s),
		   ana, ((chn->analysers & an_bit) ? "PRE" : "POST"));
	return 1;
}

/* Called when analyze ends for a given channel */
static int
trace_chn_end_analyze(struct stream *s, struct filter *filter,
		      struct channel *chn)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s));
	return 1;
}

/**************************************************************************
 * Hooks to filter HTTP messages
 *************************************************************************/
static int
trace_http_headers(struct stream *s, struct filter *filter,
		   struct http_msg *msg)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s));

	if (IS_HTX_STRM(s)) {
		struct htx *htx = htxbuf(&msg->chn->buf);
		struct htx_sl *sl = http_get_stline(htx);
		int32_t pos;

		STRM_TRACE(conf, s, "\t%.*s %.*s %.*s",
			   HTX_SL_P1_LEN(sl), HTX_SL_P1_PTR(sl),
			   HTX_SL_P2_LEN(sl), HTX_SL_P2_PTR(sl),
			   HTX_SL_P3_LEN(sl), HTX_SL_P3_PTR(sl));

		for (pos = htx_get_first(htx); pos != -1; pos = htx_get_next(htx, pos)) {
			struct htx_blk *blk = htx_get_blk(htx, pos);
			enum htx_blk_type type = htx_get_blk_type(blk);
			struct ist n, v;

			if (type == HTX_BLK_EOH)
				break;
			if (type != HTX_BLK_HDR)
				continue;

			n = htx_get_blk_name(htx, blk);
			v = htx_get_blk_value(htx, blk);
			STRM_TRACE(conf, s, "\t%.*s: %.*s",
				   (int)n.len, n.ptr, (int)v.len, v.ptr);
		}
	}
	else {
		struct hdr_idx      *hdr_idx;
		char                *cur_hdr;
		int                  cur_idx;

		STRM_TRACE(conf, s, "\t%.*s", MIN(msg->sl.rq.l, 74), ci_head(msg->chn));
		hdr_idx = &s->txn->hdr_idx;
		cur_idx = hdr_idx_first_idx(hdr_idx);
		cur_hdr = ci_head(msg->chn) + hdr_idx_first_pos(hdr_idx);
		while (cur_idx) {
			STRM_TRACE(conf, s, "\t%.*s",
				   MIN(hdr_idx->v[cur_idx].len, 74), cur_hdr);
			cur_hdr += hdr_idx->v[cur_idx].len + hdr_idx->v[cur_idx].cr + 1;
			cur_idx = hdr_idx->v[cur_idx].next;
		}
	}
	return 1;
}

static int
trace_http_payload(struct stream *s, struct filter *filter, struct http_msg *msg,
		   unsigned int offset, unsigned int len)
{
	struct trace_config *conf = FLT_CONF(filter);
	int ret = len;

	if (ret && conf->rand_forwarding) {
		struct htx *htx = htxbuf(&msg->chn->buf);
		struct htx_blk *blk;
		uint32_t sz, data = 0;
		unsigned int off = offset;

		for (blk = htx_get_first_blk(htx); blk; blk = htx_get_next_blk(htx, blk)) {
			if (htx_get_blk_type(blk) != HTX_BLK_DATA)
				break;

			sz = htx_get_blksz(blk);
			if (off >= sz) {
				off -= sz;
				continue;
			}
			data  += sz - off;
			off = 0;
			if (data > len) {
				data = len;
				break;
			}
		}

		ret = random() % (ret+1);
		if (ret > data)
			ret = len;
	}

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - "
		   "offset=%u - len=%u - forward=%d",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s),
		   offset, len, ret);

	 if (conf->hexdump)
		 trace_htx_hexdump(htxbuf(&msg->chn->buf), offset, len);

	 if (ret != len)
		 task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

static int
trace_http_data(struct stream *s, struct filter *filter,
		      struct http_msg *msg)
{
	struct trace_config *conf = FLT_CONF(filter);
	int avail = MIN(msg->chunk_len + msg->next, ci_data(msg->chn)) - FLT_NXT(filter, msg->chn);
	int ret   = avail;

	if (ret && conf->rand_parsing)
		ret = random() % (ret+1);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - "
		   "chunk_len=%llu - next=%u - fwd=%u - avail=%d - consume=%d",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s),
		   msg->chunk_len, FLT_NXT(filter, msg->chn),
		   FLT_FWD(filter, msg->chn), avail, ret);
	if (ret != avail)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

static int
trace_http_chunk_trailers(struct stream *s, struct filter *filter,
			  struct http_msg *msg)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s));
	return 1;
}

static int
trace_http_end(struct stream *s, struct filter *filter,
	       struct http_msg *msg)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s));
	return 1;
}

static void
trace_http_reset(struct stream *s, struct filter *filter,
		 struct http_msg *msg)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s));
}

static void
trace_http_reply(struct stream *s, struct filter *filter, short status,
		 const struct buffer *msg)
{
	struct trace_config *conf = FLT_CONF(filter);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s)",
		   __FUNCTION__, "-", proxy_mode(s), stream_pos(s));
}

static int
trace_http_forward_data(struct stream *s, struct filter *filter,
			struct http_msg *msg, unsigned int len)
{
	struct trace_config *conf = FLT_CONF(filter);
	int                  ret  = len;

	if (ret && conf->rand_forwarding)
		ret = random() % (ret+1);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - "
		   "len=%u - nxt=%u - fwd=%u - forward=%d",
		   __FUNCTION__,
		   channel_label(msg->chn), proxy_mode(s), stream_pos(s), len,
		   FLT_NXT(filter, msg->chn), FLT_FWD(filter, msg->chn), ret);

	if (conf->hexdump) {
		c_adv(msg->chn, FLT_FWD(filter, msg->chn));
		trace_raw_hexdump(&msg->chn->buf, ret, co_data(msg->chn));
		c_rew(msg->chn, FLT_FWD(filter, msg->chn));
	}

	if ((ret != len) ||
	    (FLT_NXT(filter, msg->chn) != FLT_FWD(filter, msg->chn) + ret))
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

/**************************************************************************
 * Hooks to filter TCP data
 *************************************************************************/
static int
trace_tcp_data(struct stream *s, struct filter *filter, struct channel *chn)
{
	struct trace_config *conf = FLT_CONF(filter);
	int                  avail = ci_data(chn) - FLT_NXT(filter, chn);
	int                  ret  = avail;

	if (ret && conf->rand_parsing)
		ret = random() % (ret+1);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - next=%u - avail=%u - consume=%d",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s),
		   FLT_NXT(filter, chn), avail, ret);

	if (ret != avail)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

static int
trace_tcp_forward_data(struct stream *s, struct filter *filter, struct channel *chn,
		 unsigned int len)
{
	struct trace_config *conf = FLT_CONF(filter);
	int                  ret  = len;

	if (ret && conf->rand_forwarding)
		ret = random() % (ret+1);

	STRM_TRACE(conf, s, "%-25s: channel=%-10s - mode=%-5s (%s) - len=%u - fwd=%u - forward=%d",
		   __FUNCTION__,
		   channel_label(chn), proxy_mode(s), stream_pos(s), len,
		   FLT_FWD(filter, chn), ret);

	if (conf->hexdump) {
		c_adv(chn, FLT_FWD(filter, chn));
		trace_raw_hexdump(&chn->buf, ret, co_data(chn));
		c_rew(chn, FLT_FWD(filter, chn));
	}

	if (ret != len)
		task_wakeup(s->task, TASK_WOKEN_MSG);
	return ret;
}

/********************************************************************
 * Functions that manage the filter initialization
 ********************************************************************/
struct flt_ops trace_ops = {
	/* Manage trace filter, called for each filter declaration */
	.init              = trace_init,
	.deinit            = trace_deinit,
	.check             = trace_check,
	.init_per_thread   = trace_init_per_thread,
	.deinit_per_thread = trace_deinit_per_thread,

	/* Handle start/stop of streams */
	.attach             = trace_attach,
	.detach             = trace_detach,
	.stream_start       = trace_stream_start,
	.stream_set_backend = trace_stream_set_backend,
	.stream_stop        = trace_stream_stop,
	.check_timeouts     = trace_check_timeouts,

	/* Handle channels activity */
	.channel_start_analyze = trace_chn_start_analyze,
	.channel_pre_analyze   = trace_chn_analyze,
	.channel_post_analyze  = trace_chn_analyze,
	.channel_end_analyze   = trace_chn_end_analyze,

	/* Filter HTTP requests and responses */
	.http_headers        = trace_http_headers,
	.http_payload        = trace_http_payload,

	.http_data           = trace_http_data,
	.http_chunk_trailers = trace_http_chunk_trailers,
	.http_end            = trace_http_end,

	.http_reset          = trace_http_reset,
	.http_reply          = trace_http_reply,
	.http_forward_data   = trace_http_forward_data,

	/* Filter TCP data */
	.tcp_data         = trace_tcp_data,
	.tcp_forward_data = trace_tcp_forward_data,
};

/* Return -1 on error, else 0 */
static int
parse_trace_flt(char **args, int *cur_arg, struct proxy *px,
                struct flt_conf *fconf, char **err, void *private)
{
	struct trace_config *conf;
	int                  pos = *cur_arg;

	conf = calloc(1, sizeof(*conf));
	if (!conf) {
		memprintf(err, "%s: out of memory", args[*cur_arg]);
		return -1;
	}
	conf->proxy = px;

	if (!strcmp(args[pos], "trace")) {
		pos++;

		while (*args[pos]) {
			if (!strcmp(args[pos], "name")) {
				if (!*args[pos + 1]) {
					memprintf(err, "'%s' : '%s' option without value",
						  args[*cur_arg], args[pos]);
					goto error;
				}
				conf->name = strdup(args[pos + 1]);
				if (!conf->name) {
					memprintf(err, "%s: out of memory", args[*cur_arg]);
					goto error;
				}
				pos++;
			}
			else if (!strcmp(args[pos], "random-parsing"))
				conf->rand_parsing = 1;
			else if (!strcmp(args[pos], "random-forwarding"))
				conf->rand_forwarding = 1;
			else if (!strcmp(args[pos], "hexdump"))
				conf->hexdump = 1;
			else
				break;
			pos++;
		}
		*cur_arg = pos;
		fconf->id   = trace_flt_id;
		fconf->ops  = &trace_ops;
	}

	fconf->conf = conf;
	return 0;

 error:
	if (conf->name)
		free(conf->name);
	free(conf);
	return -1;
}

/* Declare the filter parser for "trace" keyword */
static struct flt_kw_list flt_kws = { "TRACE", { }, {
		{ "trace", parse_trace_flt, NULL },
		{ NULL, NULL, NULL },
	}
};

INITCALL1(STG_REGISTER, flt_register_keywords, &flt_kws);
