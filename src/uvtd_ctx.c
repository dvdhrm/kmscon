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
 *
 * For each seat we create two different kinds of character-devices:
 *   /dev/ttyFC<seat>:
 *     This is the control-node. It's the preferred way to open new VTs. It
 *     provides a fully-backwards compatible VT API so legacy apps should be
 *     able to make use of it. Each open-file is associated to a different VT so
 *     you cannot share these easily, anymore. You need to pass the FD instead.
 *     This avoids problems with multiple users on the same VT that we had in
 *     the past.
 *   /dev/ttyFD<seat>/tty<num>:
 *     These are legacy VTs. They are put into a subdirectory and provide full
 *     backwards compatibility to real VTs. They are preallocated and there is
 *     only a limited number of them. You can control how many of these are
 *     allocated via the configuration options.
 *     These VTs can be shared between processes easily as all open-files on a
 *     single node share the same VT.
 * All character devices share the MAJOR number that is also used by real VTs.
 * However, the minor numbers use a relatively high offset (default: 2^14) so
 * they don't clash with real VTs.
 * If you need backwards-compatible symlinks, you can use the minor number of a
 * VT node in /dev/ttyFD<seat>/tty<num> and create a symlink:
 *   /dev/tty<minor> -> /dev/ttyFD<seat>/tty<num>
 * As the minors are globally unique, they won't clash with other tty nodes in
 * /dev. However, you loose the ability to see which seat a node is associated
 * to. So you normally look into /dev/ttyFD<seat>/, choose a node, look at the
 * minor and then open /dev/tty<minor> respectively.
 * This provides full backwards compatibility for applications that require
 * /dev/tty<num> paths (like old xservers).
 *
 * The VT logic is found in uvtd-vt subsystem. This file only provides the
 * character-device control nodes and links them to the right VTs.
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
#include "uvtd_vt.h"

#define LOG_SUBSYSTEM "ctx"

struct ctx_legacy {
	struct shl_dlist list;
	struct uvtd_ctx *ctx;
	unsigned int minor;
	unsigned int id;
	struct uvt_cdev *cdev;
	struct uvtd_vt *vt;
};

struct uvtd_ctx {
	struct ev_eloop *eloop;
	struct uvt_ctx *uctx;
	struct uvtd_seat *seat;
	char *seatname;

	unsigned int main_cdev_minor;
	struct uvt_cdev *main_cdev;

	struct shl_dlist legacy_list;
	unsigned int legacy_num;
};

static int ctx_legacy_init_vt(struct ctx_legacy *legacy);

static void ctx_legacy_vt_event(void *vt, struct uvt_vt_event *ev, void *data)
{
	struct ctx_legacy *legacy = data;

	switch (ev->type) {
	case UVT_VT_TTY:
		if (ev->tty.type != UVT_TTY_HUP)
			break;

		/* fallthrough */
	case UVT_VT_HUP:
		log_debug("HUP on legacy VT %p", legacy);

		uvtd_vt_unregister_cb(legacy->vt, ctx_legacy_vt_event, legacy);
		uvtd_vt_unref(legacy->vt);
		legacy->vt = NULL;
		break;
	}
}

static void ctx_legacy_cdev_event(struct uvt_cdev *cdev,
				  struct uvt_cdev_event *ev, void *data)
{
	struct ctx_legacy *legacy = data;
	int ret;

	switch (ev->type) {
	case UVT_CDEV_HUP:
		log_warning("HUP on legacy cdev %p", cdev);
		uvt_cdev_unregister_cb(legacy->cdev, ctx_legacy_cdev_event,
				       legacy);
		uvt_cdev_unref(legacy->cdev);
		legacy->cdev = NULL;
		break;
	case UVT_CDEV_OPEN:
		/* A legacy VT might get closed by the seat/session-scheduler at
		 * any time. We want to avoid respawning it right away to avoid
		 * error-throttling, so instead we respawn it when the next
		 * client opens the underlying cdev. */
		if (!legacy->vt) {
			log_debug("reinitializing VT on legacy cdev %p",
				  legacy);
			ret = ctx_legacy_init_vt(legacy);
			if (ret) {
				log_warning("cannot reinitialize VT on legacy cdev %p",
					    legacy);
				uvt_client_kill(ev->client);
				break;
			}
		}

		ret = uvt_client_set_vt(ev->client, &uvtd_vt_ops, legacy->vt);
		if (ret) {
			log_warning("cannot assign VT to new client: %d",
				    ret);
			uvt_client_kill(ev->client);
		}
		break;
	}
}

static int ctx_legacy_init_vt(struct ctx_legacy *legacy)
{
	int ret;

	ret = uvtd_vt_new(&legacy->vt, legacy->ctx->uctx, legacy->id,
			  legacy->ctx->seat, true);
	if (ret)
		return ret;

	ret = uvtd_vt_register_cb(legacy->vt, ctx_legacy_vt_event, legacy);
	if (ret) {
		uvtd_vt_unref(legacy->vt);
		legacy->vt = NULL;
		return ret;
	}

	return 0;
}

static int ctx_legacy_init_cdev(struct ctx_legacy *legacy)
{
	char *name;
	int ret;

	ret = asprintf(&name, "ttyFD%s!tty%u", legacy->ctx->seatname,
		       legacy->minor);
	if (ret <= 0)
		return -ENOMEM;

	ret = uvt_cdev_new(&legacy->cdev, legacy->ctx->uctx, name,
			   uvt_ctx_get_major(legacy->ctx->uctx),
			   legacy->minor);
	free(name);

	if (ret)
		return ret;

	ret = uvt_cdev_register_cb(legacy->cdev, ctx_legacy_cdev_event,
				   legacy);
	if (ret) {
		uvt_cdev_unref(legacy->cdev);
		legacy->cdev = NULL;
		return ret;
	}

	return 0;
}

static int ctx_legacy_init(struct uvtd_ctx *ctx, unsigned int id)
{
	struct ctx_legacy *legacy;
	int ret;

	legacy = malloc(sizeof(*legacy));
	if (!legacy)
		return -ENOMEM;

	log_debug("new legacy cdev %p on ctx %p", legacy, ctx);

	memset(legacy, 0, sizeof(*legacy));
	legacy->id = id;
	legacy->ctx = ctx;

	ret = uvt_ctx_new_minor(ctx->uctx, &legacy->minor);
	if (ret)
		goto err_free;

	ret = ctx_legacy_init_cdev(legacy);
	if (ret)
		goto err_minor;

	ret = ctx_legacy_init_vt(legacy);
	if (ret)
		goto err_cdev;

	shl_dlist_link(&ctx->legacy_list, &legacy->list);
	return 0;

err_cdev:
	uvt_cdev_unregister_cb(legacy->cdev, ctx_legacy_cdev_event, legacy);
	uvt_cdev_unref(legacy->cdev);
err_minor:
	uvt_ctx_free_minor(ctx->uctx, legacy->minor);
err_free:
	free(legacy);
	return ret;
}

static void ctx_legacy_destroy(struct ctx_legacy *legacy)
{
	log_debug("free legacy cdev %p", legacy);

	shl_dlist_unlink(&legacy->list);
	uvtd_vt_unregister_cb(legacy->vt, ctx_legacy_vt_event, legacy);
	uvtd_vt_unref(legacy->vt);
	uvt_cdev_unregister_cb(legacy->cdev, ctx_legacy_cdev_event, legacy);
	uvt_cdev_unref(legacy->cdev);
	uvt_ctx_free_minor(legacy->ctx->uctx, legacy->minor);
	free(legacy);
}

static void ctx_legacy_reconf(struct uvtd_ctx *ctx, unsigned int num)
{
	struct ctx_legacy *l;
	struct shl_dlist *iter;
	unsigned int i;
	int ret;

	/* If a legacy cdev received a HUP or some other error and got closed,
	 * we try to reinitialize it whenever the context is reconfigured. This
	 * avoids implementing any error-throttling while at the same time users
	 * can trigger a reinitialization with a reconfiguration.
	 * This doesn't touch running cdevs, but only HUP'ed cdevs. */
	shl_dlist_for_each(iter, &ctx->legacy_list) {
		l = shl_dlist_entry(iter, struct ctx_legacy, list);
		if (l->cdev)
			continue;

		log_debug("reinitialize legacy cdev %p", l);

		ret = ctx_legacy_init_cdev(l);
		if (ret)
			log_warning("cannot reinitialize legacy cdev %p: %d",
				    l, ret);
	}

	if (num == ctx->legacy_num)
		return;

	log_debug("changing #num of legacy cdevs on ctx %p from %u to %u",
		  ctx, ctx->legacy_num, num);

	if (num > ctx->legacy_num) {
		for (i = ctx->legacy_num; i < num; ++i) {
			ret = ctx_legacy_init(ctx, i);
			if (ret)
				break;
		}

		ctx->legacy_num = i;
	} else {
		for (i = num; i < ctx->legacy_num; ++i) {
			l = shl_dlist_last(&ctx->legacy_list,
					   struct ctx_legacy, list);
			ctx_legacy_destroy(l);
		}

		ctx->legacy_num = num;
	}
}

static void ctx_main_cdev_event(struct uvt_cdev *cdev,
				struct uvt_cdev_event *ev,
				void *data)
{
	struct uvtd_ctx *ctx = data;
	struct uvtd_vt *vt;
	int ret;

	switch (ev->type) {
	case UVT_CDEV_HUP:
		log_warning("HUP on main cdev on ctx %p", ctx);
		uvt_cdev_unregister_cb(ctx->main_cdev, ctx_main_cdev_event,
				       ctx);
		uvt_cdev_unref(ctx->main_cdev);
		ctx->main_cdev = NULL;
		break;
	case UVT_CDEV_OPEN:
		ret = uvtd_vt_new(&vt, ctx->uctx, 0, ctx->seat, false);
		if (ret)
			break;

		uvt_client_set_vt(ev->client, &uvtd_vt_ops, vt);
		uvtd_vt_unref(vt);
		break;
	}
}

static int ctx_init_cdev(struct uvtd_ctx *ctx)
{
	int ret;
	char *name;

	ret = asprintf(&name, "ttyFC%s", ctx->seatname);
	if (ret <= 0)
		return -ENOMEM;

	ret = uvt_cdev_new(&ctx->main_cdev, ctx->uctx, name,
			   uvt_ctx_get_major(ctx->uctx), ctx->main_cdev_minor);
	free(name);

	if (ret)
		return ret;

	ret = uvt_cdev_register_cb(ctx->main_cdev, ctx_main_cdev_event, ctx);
	if (ret) {
		uvt_cdev_unref(ctx->main_cdev);
		ctx->main_cdev = NULL;
		return ret;
	}

	return 0;
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
	shl_dlist_init(&ctx->legacy_list);

	ctx->seatname = strdup(seatname);
	if (!ctx->seatname) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = uvtd_seat_new(&ctx->seat, seatname, ctx->eloop, NULL, NULL);
	if (ret)
		goto err_name;

	ret = uvt_ctx_new_minor(ctx->uctx, &ctx->main_cdev_minor);
	if (ret)
		goto err_seat;

	ret = ctx_init_cdev(ctx);
	if (ret)
		goto err_minor;

	ev_eloop_ref(ctx->eloop);
	uvt_ctx_ref(ctx->uctx);
	*out = ctx;

	ctx_legacy_reconf(ctx, 8);

	return 0;

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

	log_debug("free ctx %p", ctx);

	ctx_legacy_reconf(ctx, 0);
	uvt_cdev_unregister_cb(ctx->main_cdev, ctx_main_cdev_event, ctx);
	uvt_cdev_unref(ctx->main_cdev);
	uvt_ctx_free_minor(ctx->uctx, ctx->main_cdev_minor);
	uvtd_seat_free(ctx->seat);
	free(ctx->seatname);
	uvt_ctx_unref(ctx->uctx);
	ev_eloop_unref(ctx->eloop);
	free(ctx);
}

void uvtd_ctx_reconf(struct uvtd_ctx *ctx, unsigned int legacy_num)
{
	int ret;

	if (!ctx)
		return;

	ctx_legacy_reconf(ctx, legacy_num);

	/* Lets recreate the control node if it got busted during runtime. We do
	 * not recreate it right away after receiving a HUP signal to avoid
	 * trapping into the same error that caused the HUP.
	 * Instead we recreate the node on reconfiguration so users can control
	 * when to recreate them. */
	if (!ctx->main_cdev) {
		log_debug("recreating main cdev on ctx %p", ctx);
		ret = ctx_init_cdev(ctx);
		if (ret)
			log_warning("cannot recreate main cdev on ctx %p",
				    ctx);
	}
}
