/*
 * uvtd - User-space VT daemon
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
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
 * Contexts
 * A context manages a single UVT seat. It creates the seat object, allocates
 * the VTs and provides all the bookkeeping for the sessions. It's the main
 * entry point after the seat selectors in uvtd-main.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "uvt.h"
#include "uvtd_ctx.h"
#include "uvtd_seat.h"

#define LOG_SUBSYSTEM "ctx"

struct ctx_legacy {
	struct shl_dlist list;
	struct uvtd_ctx *ctx;
	unsigned int minor;
	unsigned int id;
	struct uvt_cdev *cdev;
};

struct uvtd_ctx {
	struct ev_eloop *eloop;
	struct uvt_ctx *uctx;
	struct uvtd_seat *seat;
	char *seatname;

	unsigned int main_cdev_minor;
	struct uvt_cdev *main_cdev;

	struct shl_dlist legacy_cdevs;
	unsigned int legacy_num;
};

static void ctx_legacy_cdev_event(struct uvt_cdev *cdev,
				  struct uvt_cdev_event *ev, void *data)
{
	switch (ev->type) {
	case UVT_CDEV_HUP:
		break;
	case UVT_CDEV_OPEN:
		break;
	}
}

static int ctx_legacy_cdev_init(struct uvtd_ctx *ctx, unsigned int id)
{
	struct ctx_legacy *legacy;
	char *name;
	int ret;

	legacy = malloc(sizeof(*legacy));
	if (!legacy)
		return -ENOMEM;

	memset(legacy, 0, sizeof(*legacy));
	legacy->id = id;
	legacy->ctx = ctx;

	ret = uvt_ctx_new_minor(ctx->uctx, &legacy->minor);
	if (ret)
		goto err_free;

	ret = asprintf(&name, "ttysF%s!tty%u", ctx->seatname, legacy->minor);
	if (ret <= 0) {
		ret = -ENOMEM;
		goto err_minor;
	}

	ret = uvt_cdev_new(&legacy->cdev, ctx->uctx, name,
			   uvt_ctx_get_major(ctx->uctx), legacy->minor);
	free(name);

	if (ret)
		goto err_minor;

	ret = uvt_cdev_register_cb(legacy->cdev, ctx_legacy_cdev_event,
				   legacy);
	if (ret)
		goto err_cdev;

	shl_dlist_link(&ctx->legacy_cdevs, &legacy->list);
	return 0;

err_cdev:
	uvt_cdev_unref(legacy->cdev);
err_minor:
	uvt_ctx_free_minor(ctx->uctx, legacy->minor);
err_free:
	free(legacy);
	return ret;
}

static void ctx_legacy_cdev_destroy(struct ctx_legacy *legacy)
{
	shl_dlist_unlink(&legacy->list);
	uvt_cdev_unregister_cb(legacy->cdev, ctx_legacy_cdev_event, legacy);
	uvt_cdev_unref(legacy->cdev);
	uvt_ctx_free_minor(legacy->ctx->uctx, legacy->minor);
	free(legacy);
}

static void ctx_legacy_cdev_conf(struct uvtd_ctx *ctx, unsigned int num)
{
	struct ctx_legacy *l;
	unsigned int i;
	int ret;

	if (num > ctx->legacy_num) {
		for (i = ctx->legacy_num; i < num; ++i) {
			ret = ctx_legacy_cdev_init(ctx, i);
			if (ret)
				break;
		}

		ctx->legacy_num = i;
	} else {
		for (i = num; i < ctx->legacy_num; ++i) {
			l = shl_dlist_last(&ctx->legacy_cdevs,
					   struct ctx_legacy, list);
			ctx_legacy_cdev_destroy(l);
		}

		ctx->legacy_num = num;
	}
}

static void ctx_main_cdev_event(struct uvt_cdev *cdev,
				struct uvt_cdev_event *ev,
				void *data)
{
	struct uvtd_ctx *ctx = data;

	switch (ev->type) {
	case UVT_CDEV_HUP:
		log_error("HUP on main cdev on seat %s", ctx->seatname);
		break;
	case UVT_CDEV_OPEN:
		log_debug("new client on main cdev on seat %s",
			  ctx->seatname);
		break;
	}
}

static void ctx_seat_event(struct uvtd_seat *seat, unsigned int ev, void *data)
{
}

static bool has_real_vts(const char *seatname)
{
	return !strcmp(seatname, "seat0") &&
	       !access("/dev/tty0", F_OK);
}

int uvtd_ctx_new(struct uvtd_ctx **out, const char *seatname,
		 struct ev_eloop *eloop, struct uvt_ctx *uctx)
{
	struct uvtd_ctx *ctx;
	int ret;
	char *name;

	if (!out || !seatname || !eloop || !uctx)
		return -EINVAL;

	if (has_real_vts(seatname))
		return -EEXIST;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	log_debug("new ctx %p on seat %s", ctx, seatname);

	memset(ctx, 0, sizeof(*ctx));
	ctx->eloop = eloop;
	ctx->uctx = uctx;
	shl_dlist_init(&ctx->legacy_cdevs);

	ctx->seatname = strdup(seatname);
	if (!ctx->seatname) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = uvtd_seat_new(&ctx->seat, seatname, ctx->eloop, ctx_seat_event,
			    ctx);
	if (ret)
		goto err_name;

	ret = uvt_ctx_new_minor(ctx->uctx, &ctx->main_cdev_minor);
	if (ret)
		goto err_seat;

	ret = asprintf(&name, "ttyF%s", seatname);
	if (ret <= 0) {
		ret = -ENOMEM;
		goto err_minor;
	}

	ret = uvt_cdev_new(&ctx->main_cdev, ctx->uctx, name,
			   uvt_ctx_get_major(ctx->uctx), ctx->main_cdev_minor);
	free(name);

	if (ret)
		goto err_minor;

	ret = uvt_cdev_register_cb(ctx->main_cdev, ctx_main_cdev_event, ctx);
	if (ret)
		goto err_cdev;

	ctx_legacy_cdev_conf(ctx, 8);

	ev_eloop_ref(ctx->eloop);
	uvt_ctx_ref(ctx->uctx);
	*out = ctx;
	return 0;

err_cdev:
	uvt_cdev_unref(ctx->main_cdev);
err_minor:
	uvt_ctx_free_minor(ctx->uctx, ctx->main_cdev_minor);
err_seat:
	uvtd_seat_free(ctx->seat);
err_name:
	free(ctx->seatname);
err_free:
	free(ctx);
	return ret;
}

void uvtd_ctx_free(struct uvtd_ctx *ctx)
{
	if (!ctx)
		return;

	ctx_legacy_cdev_conf(ctx, 0);
	uvt_cdev_unregister_cb(ctx->main_cdev, ctx_main_cdev_event, ctx);
	uvt_cdev_unref(ctx->main_cdev);
	uvt_ctx_free_minor(ctx->uctx, ctx->main_cdev_minor);
	uvtd_seat_free(ctx->seat);
	uvt_ctx_unref(ctx->uctx);
	ev_eloop_unref(ctx->eloop);
	free(ctx->seatname);
	free(ctx);
}
