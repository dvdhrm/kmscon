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
 * Null TTY
 * This tty simply discards all incoming messages and never produces any
 * outgoing messages. Ioctls return static data or fail with some generic error
 * code if they would modify internal state that we cannot emulate easily.
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

#define LLOG_SUBSYSTEM "uvt_tty_null"

struct uvt_tty_null {
	unsigned long ref;
	struct uvt_ctx *ctx;
	llog_submit_t llog;
	void *llog_data;
};

static void tty_null_ref(void *data)
{
	uvt_tty_null_ref(data);
}

static void tty_null_unref(void *data)
{
	uvt_tty_null_unref(data);
}

static int tty_null_register_cb(void *data, uvt_tty_cb cb, void *cb_data)
{
	return 0;
}

static void tty_null_unregister_cb(void *data, uvt_tty_cb cb, void *cb_data)
{
}

static int tty_null_read(void *data, uint8_t *buf, size_t size)
{
	return -EAGAIN;
}

static int tty_null_write(void *data, const uint8_t *buf, size_t size)
{
	return size;
}

static unsigned int tty_null_poll(void *data)
{
	return UVT_TTY_WRITE;
}

const struct uvt_tty_ops uvt_tty_null_ops = {
	.ref = tty_null_ref,
	.unref = tty_null_unref,
	.register_cb = tty_null_register_cb,
	.unregister_cb = tty_null_unregister_cb,

	.read = tty_null_read,
	.write = tty_null_write,
	.poll = tty_null_poll,
};

int uvt_tty_null_new(struct uvt_tty_null **out, struct uvt_ctx *ctx)
{
	struct uvt_tty_null *tty;

	if (!ctx)
		return -EINVAL;
	if (!out)
		return llog_EINVAL(ctx);

	tty = malloc(sizeof(*tty));
	if (!tty)
		return llog_ENOMEM(ctx);
	memset(tty, 0, sizeof(*tty));
	tty->ref = 1;
	tty->ctx = ctx;
	tty->llog = tty->ctx->llog;
	tty->llog_data = tty->ctx->llog_data;

	uvt_ctx_ref(tty->ctx);
	*out = tty;
	return 0;
}

void uvt_tty_null_ref(struct uvt_tty_null *tty)
{
	if (!tty || !tty->ref)
		return;

	++tty->ref;
}

void uvt_tty_null_unref(struct uvt_tty_null *tty)
{
	if (!tty || !tty->ref || --tty->ref)
		return;

	uvt_ctx_unref(tty->ctx);
	free(tty);
}
