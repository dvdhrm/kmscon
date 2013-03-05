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
#include <linux/major.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shl_array.h"
#include "shl_flagset.h"
#include "shl_llog.h"
#include "shl_misc.h"
#include "uvt.h"
#include "uvt_internal.h"

#define LLOG_SUBSYSTEM "uvt_ctx"

SHL_EXPORT
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

	/* Default major/minor uses the TTY_MAJOR number with an offset of 2^15
	 * to avoid ID-clashes with any in-kernel TTY driver. As kernel drivers
	 * use static IDs only, a lower number would be fine, too, but lets be
	 * safe and just use high numbers. */
	ctx->major = TTY_MAJOR;
	ctx->minor_offset = 16384;

	llog_debug(ctx, "new ctx %p", ctx);

	ret = ev_eloop_new(&ctx->eloop, ctx->llog, ctx->llog_data);
	if (ret)
		goto err_free;

	ctx->cuse_file = strdup("/dev/cuse");
	if (!ctx->cuse_file) {
		ret = llog_ENOMEM(ctx);
		goto err_eloop;
	}

	ret = shl_flagset_new(&ctx->minors);
	if (ret)
		goto err_file;

	*out = ctx;
	return 0;

err_file:
	free(ctx->cuse_file);
err_eloop:
	ev_eloop_unref(ctx->eloop);
err_free:
	free(ctx);
	return ret;
}

SHL_EXPORT
void uvt_ctx_ref(struct uvt_ctx *ctx)
{
	if (!ctx || !ctx->ref)
		return;

	++ctx->ref;
}

SHL_EXPORT
void uvt_ctx_unref(struct uvt_ctx *ctx)
{
	if (!ctx || !ctx->ref || --ctx->ref)
		return;

	llog_debug(ctx, "free ctx %p", ctx);

	shl_flagset_free(ctx->minors);
	free(ctx->cuse_file);
	ev_eloop_unref(ctx->eloop);
	free(ctx);
}

SHL_EXPORT
int uvt_ctx_get_fd(struct uvt_ctx *ctx)
{
	if (!ctx)
		return -1;

	return ev_eloop_get_fd(ctx->eloop);
}

SHL_EXPORT
void uvt_ctx_dispatch(struct uvt_ctx *ctx)
{
	if (!ctx)
		return;

	ev_eloop_dispatch(ctx->eloop, 0);
}

SHL_EXPORT
unsigned int uvt_ctx_get_major(struct uvt_ctx *ctx)
{
	return ctx->major;
}

SHL_EXPORT
int uvt_ctx_new_minor(struct uvt_ctx *ctx, unsigned int *out)
{
	int ret;

	ret = shl_flagset_alloc(ctx->minors, out);
	if (ret)
		return ret;

	*out += ctx->minor_offset;
	return 0;
}

SHL_EXPORT
void uvt_ctx_free_minor(struct uvt_ctx *ctx, unsigned int minor)
{
	if (!ctx || minor < ctx->minor_offset)
		return;

	shl_flagset_unset(ctx->minors, minor - ctx->minor_offset);
}
