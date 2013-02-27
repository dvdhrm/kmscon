/*
 * UVT - Userspace Virtual Terminals
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * UVT Contexts
 * A UVT context is used to provide basic infrastructure for all other UVT
 * objects. It allows easy integration of multiple UVT objects into a single
 * application.
 */

#include <eloop.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shl_llog.h"
#include "uvt.h"
#include "uvt_internal.h"

#define LLOG_SUBSYSTEM "uvt_ctx"

int uvt_ctx_new(struct uvt_ctx **out, uvt_log_t log, void *log_data)
{
	struct uvt_ctx *ctx;
	int ret;

	if (!out)
		return llog_dEINVAL(log, log_data);

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return llog_dENOMEM(log, log_data);
	memset(ctx, 0, sizeof(*ctx));
	ctx->ref = 1;
	ctx->llog = log;
	ctx->llog_data = log_data;

	llog_debug(ctx, "new ctx %p", ctx);

	ret = ev_eloop_new(&ctx->eloop, ctx->llog, ctx->llog_data);
	if (ret)
		goto err_free;

	ctx->cuse_file = strdup("/dev/cuse");
	if (!ctx->cuse_file) {
		ret = llog_ENOMEM(ctx);
		goto err_eloop;
	}

	*out = ctx;
	return 0;

err_eloop:
	ev_eloop_unref(ctx->eloop);
err_free:
	free(ctx);
	return ret;
}

void uvt_ctx_ref(struct uvt_ctx *ctx)
{
	if (!ctx || !ctx->ref)
		return;

	++ctx->ref;
}

void uvt_ctx_unref(struct uvt_ctx *ctx)
{
	if (!ctx || !ctx->ref || --ctx->ref)
		return;

	llog_debug(ctx, "free ctx %p", ctx);

	free(ctx->cuse_file);
	ev_eloop_unref(ctx->eloop);
	free(ctx);
}

int uvt_ctx_get_fd(struct uvt_ctx *ctx)
{
	if (!ctx)
		return -1;

	return ev_eloop_get_fd(ctx->eloop);
}

void uvt_ctx_dispatch(struct uvt_ctx *ctx)
{
	if (!ctx)
		return;

	ev_eloop_dispatch(ctx->eloop, 0);
}
