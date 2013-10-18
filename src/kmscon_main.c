/*
 * kmscon - KMS Console
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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

#include <errno.h>
#include <paths.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include "conf.h"
#include "eloop.h"
#include "kmscon_conf.h"
#include "kmscon_module.h"
#include "kmscon_seat.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "text.h"
#include "uterm_input.h"
#include "uterm_monitor.h"
#include "uterm_video.h"
#include "uterm_vt.h"

struct app_video {
	struct shl_dlist list;
	struct app_seat *seat;
	struct uterm_monitor_dev *udev;

	char *node;
	struct uterm_video *video;
};

struct app_seat {
	struct shl_dlist list;
	struct kmscon_app *app;
	struct uterm_monitor_seat *useat;

	bool awake;
	char *name;
	struct kmscon_seat *seat;
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct shl_dlist videos;
};

struct kmscon_app {
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	bool exiting;

	struct ev_eloop *eloop;
	unsigned int vt_exit_count;

	struct uterm_vt_master *vtm;
	struct uterm_monitor *mon;
	struct shl_dlist seats;
	unsigned int running_seats;
};

static int app_seat_event(struct kmscon_seat *s, unsigned int event,
			  void *data)
{
	struct app_seat *seat = data;
	struct kmscon_app *app = seat->app;
	struct shl_dlist *iter;
	struct app_video *vid;

	switch (event) {
	case KMSCON_SEAT_FOREGROUND:
		seat->awake = true;

		shl_dlist_for_each(iter, &seat->videos) {
			vid = shl_dlist_entry(iter, struct app_video, list);
			uterm_video_wake_up(vid->video);
		}
		break;
	case KMSCON_SEAT_BACKGROUND:
		shl_dlist_for_each(iter, &seat->videos) {
			vid = shl_dlist_entry(iter, struct app_video, list);
			uterm_video_sleep(vid->video);
		}

		seat->awake = false;
		break;
	case KMSCON_SEAT_SLEEP:
		if (app->vt_exit_count > 0) {
			log_debug("deactivating VT on exit, %d to go",
				  app->vt_exit_count - 1);
			if (!--app->vt_exit_count)
				ev_eloop_exit(app->eloop);
		}
		break;
	case KMSCON_SEAT_WAKE_UP:
		if (app->exiting)
			return -EBUSY;
		break;
	case KMSCON_SEAT_HUP:
		kmscon_seat_free(seat->seat);
		seat->seat = NULL;

		if (!app->conf->listen) {
			--app->running_seats;
			if (!app->running_seats) {
				log_debug("seat HUP on %s in default-mode; exiting...",
					  seat->name);
				ev_eloop_exit(app->eloop);
			} else {
				log_debug("seat HUP on %s in default-mode; %u more running seats",
					  seat->name, app->running_seats);
			}
		} else {
			/* Seat HUP here means that we are running in
			 * listen-mode on a modular-VT like kmscon-fake-VTs. But
			 * this is an invalid setup. In listen-mode we
			 * exclusively run as seat-VT-master without a
			 * controlling VT and we effectively prevent other
			 * setups during startup. Hence, we can safely drop the
			 * seat here and ignore it.
			 * You can destroy and recreate the seat to make kmscon
			 * pick it up again in listen-mode. */
			log_warning("seat HUP on %s in listen-mode; dropping seat...",
				    seat->name);
		}

		break;
	}

	return 0;
}

static int app_seat_new(struct kmscon_app *app, const char *sname,
			struct uterm_monitor_seat *useat)
{
	struct app_seat *seat;
	int ret;
	unsigned int i, types;
	bool found;
	char *cseat;

	if (app->exiting)
		return -EBUSY;

	found = false;
	if (kmscon_conf_is_all_seats(app->conf)) {
		found = true;
	} else if (kmscon_conf_is_current_seat(app->conf)) {
		cseat = getenv("XDG_SEAT");
		if (!cseat)
			cseat = "seat0";
		if (!strcmp(cseat, sname))
			found = true;
	} else {
		for (i = 0; app->conf->seats[i]; ++i) {
			if (!strcmp(app->conf->seats[i], sname)) {
				found = true;
				break;
			}
		}
	}

	if (!found) {
		log_info("ignoring new seat %s as not specified in seat-list",
			 sname);
		return -ERANGE;
	}

	log_debug("new seat %s", sname);

	seat = malloc(sizeof(*seat));
	if (!seat) {
		log_error("cannot allocate memory for seat %s", sname);
		return -ENOMEM;
	}
	memset(seat, 0, sizeof(*seat));
	seat->app = app;
	seat->useat = useat;
	shl_dlist_init(&seat->videos);

	seat->name = strdup(sname);
	if (!seat->name) {
		log_error("cannot copy seat name on seat %s", sname);
		ret = -ENOMEM;
		goto err_free;
	}

	types = UTERM_VT_FAKE;
	if (!app->conf->listen)
		types |= UTERM_VT_REAL;

	ret = kmscon_seat_new(&seat->seat, app->conf_ctx, app->eloop, app->vtm,
			      types, sname, app_seat_event, seat);
	if (ret) {
		if (ret == -ERANGE)
			log_debug("ignoring seat %s as it already has a seat manager",
				  sname);
		else
			log_error("cannot create seat object on seat %s: %d",
				  sname, ret);
		goto err_name;
	}
	seat->conf_ctx = kmscon_seat_get_conf(seat->seat);
	seat->conf = conf_ctx_get_mem(seat->conf_ctx);

	uterm_monitor_set_seat_data(seat->useat, seat);
	shl_dlist_link(&app->seats, &seat->list);
	++app->running_seats;

	kmscon_seat_startup(seat->seat);

	return 0;

err_name:
	free(seat->name);
err_free:
	free(seat);
	return ret;
}

static void app_seat_free(struct app_seat *seat)
{
	log_debug("free seat %s", seat->name);

	shl_dlist_unlink(&seat->list);
	uterm_monitor_set_seat_data(seat->useat, NULL);
	kmscon_seat_free(seat->seat);
	free(seat->name);
	free(seat);
}

static void app_seat_video_event(struct uterm_video *video,
				 struct uterm_video_hotplug *ev,
				 void *data)
{
	struct app_video *vid = data;

	switch (ev->action) {
	case UTERM_NEW:
		if (!vid->seat->app->exiting)
			kmscon_seat_add_display(vid->seat->seat, ev->display);
		break;
	case UTERM_GONE:
		kmscon_seat_remove_display(vid->seat->seat, ev->display);
		break;
	case UTERM_REFRESH:
		if (!vid->seat->app->exiting)
			kmscon_seat_refresh_display(vid->seat->seat,
						    ev->display);
		break;
	}
}

static bool app_seat_gpu_is_ignored(struct app_seat *seat,
				    unsigned int type,
				    bool drm_backed,
				    bool primary,
				    bool aux,
				    const char *node)
{
	switch (type) {
	case UTERM_MONITOR_FBDEV:
		if (seat->conf->drm) {
			if (drm_backed) {
				log_info("ignoring video device %s on seat %s as it is a DRM-fbdev device",
					 node, seat->name);
				return true;
			}
		}
		break;
	case UTERM_MONITOR_DRM:
		if (!seat->conf->drm) {
			log_info("ignoring video device %s on seat %s as it is a DRM device",
				  node, seat->name);
			return true;
		}
		break;
	default:
		log_info("ignoring unknown video device %s on seat %s",
			 node, seat->name);
		return true;
	}

	if (seat->conf->gpus == KMSCON_GPU_PRIMARY && !primary) {
		log_info("ignoring video device %s on seat %s as it is no primary GPU",
			 node, seat->name);
		return true;
	}

	if (seat->conf->gpus == KMSCON_GPU_AUX && !primary && !aux) {
		log_info("ignoring video device %s on seat %s as it is neither a primary nor auxiliary GPU",
			 node, seat->name);
		return true;
	}

	return false;
}

static int app_seat_add_video(struct app_seat *seat,
			      unsigned int type,
			      unsigned int flags,
			      const char *node,
			      struct uterm_monitor_dev *udev)
{
	int ret;
	const struct uterm_video_module *mode;
	struct app_video *vid;

	if (seat->app->exiting)
		return -EBUSY;

	if (app_seat_gpu_is_ignored(seat, type,
				    flags & UTERM_MONITOR_DRM_BACKED,
				    flags & UTERM_MONITOR_PRIMARY,
				    flags & UTERM_MONITOR_AUX,
				    node))
		return -ERANGE;

	log_debug("new video device %s on seat %s", node, seat->name);

	vid = malloc(sizeof(*vid));
	if (!vid) {
		log_error("cannot allocate memory for video device %s on seat %s",
			  node, seat->name);
		return -ENOMEM;
	}
	memset(vid, 0, sizeof(*vid));
	vid->seat = seat;
	vid->udev = udev;

	vid->node = strdup(node);
	if (!vid->node) {
		log_error("cannot copy video device name %s on seat %s",
			  node, seat->name);
		ret = -ENOMEM;
		goto err_free;
	}

	if (type == UTERM_MONITOR_DRM) {
		if (seat->conf->hwaccel)
			mode = UTERM_VIDEO_DRM3D;
		else
			mode = UTERM_VIDEO_DRM2D;
	} else {
		mode = UTERM_VIDEO_FBDEV;
	}

	ret = uterm_video_new(&vid->video, seat->app->eloop, node, mode);
	if (ret) {
		if (mode == UTERM_VIDEO_DRM3D) {
			log_info("cannot create drm3d device %s on seat %s (%d); trying drm2d mode",
				 vid->node, seat->name, ret);
			ret = uterm_video_new(&vid->video, seat->app->eloop,
					      node, UTERM_VIDEO_DRM2D);
			if (ret)
				goto err_node;
		} else {
			goto err_node;
		}
	}

	ret = uterm_video_register_cb(vid->video, app_seat_video_event, vid);
	if (ret) {
		log_error("cannot register video callback for device %s on seat %s: %d",
			  vid->node, seat->name, ret);
		goto err_video;
	}

	if (seat->awake)
		uterm_video_wake_up(vid->video);

	uterm_monitor_set_dev_data(vid->udev, vid);
	shl_dlist_link(&seat->videos, &vid->list);
	return 0;

err_video:
	uterm_video_unref(vid->video);
err_node:
	free(vid->node);
err_free:
	free(vid);
	return ret;
}

static void app_seat_remove_video(struct app_seat *seat, struct app_video *vid)
{
	struct uterm_display *disp;

	log_debug("free video device %s on seat %s", vid->node, seat->name);

	shl_dlist_unlink(&vid->list);
	uterm_monitor_set_dev_data(vid->udev, NULL);
	uterm_video_unregister_cb(vid->video, app_seat_video_event, vid);

	disp = uterm_video_get_displays(vid->video);
	while (disp) {
		kmscon_seat_remove_display(seat->seat, disp);
		disp = uterm_display_next(disp);
	}

	uterm_video_unref(vid->video);
	free(vid->node);
	free(vid);
}

static void app_monitor_event(struct uterm_monitor *mon,
			      struct uterm_monitor_event *ev,
			      void *data)
{
	struct kmscon_app *app = data;
	struct app_seat *seat;
	struct app_video *vid;
	int ret;

	switch (ev->type) {
	case UTERM_MONITOR_NEW_SEAT:
		ret = app_seat_new(app, ev->seat_name, ev->seat);
		if (ret)
			return;
		break;
	case UTERM_MONITOR_FREE_SEAT:
		if (ev->seat_data)
			app_seat_free(ev->seat_data);
		break;
	case UTERM_MONITOR_NEW_DEV:
		seat = ev->seat_data;
		if (!seat)
			return;

		switch (ev->dev_type) {
		case UTERM_MONITOR_DRM:
		case UTERM_MONITOR_FBDEV:
			ret = app_seat_add_video(seat, ev->dev_type,
						 ev->dev_flags,
						 ev->dev_node, ev->dev);
			if (ret)
				return;
			break;
		case UTERM_MONITOR_INPUT:
			log_debug("new input device %s on seat %s",
				  ev->dev_node, seat->name);
			kmscon_seat_add_input(seat->seat, ev->dev_node);
			break;
		}
		break;
	case UTERM_MONITOR_FREE_DEV:
		seat = ev->seat_data;
		if (!seat)
			return;

		switch (ev->dev_type) {
		case UTERM_MONITOR_DRM:
		case UTERM_MONITOR_FBDEV:
			if (ev->dev_data)
				app_seat_remove_video(seat, ev->dev_data);
			break;
		case UTERM_MONITOR_INPUT:
			log_debug("free input device %s on seat %s",
				  ev->dev_node, seat->name);
			kmscon_seat_remove_input(seat->seat, ev->dev_node);
			break;
		}
		break;
	case UTERM_MONITOR_HOTPLUG_DEV:
		seat = ev->seat_data;
		if (!seat)
			return;

		switch (ev->dev_type) {
		case UTERM_MONITOR_DRM:
		case UTERM_MONITOR_FBDEV:
			vid = ev->dev_data;
			if (!vid)
				return;

			log_debug("video hotplug event on device %s on seat %s",
				  vid->node, seat->name);
			uterm_video_poll(vid->video);
			break;
		}
		break;
	}
}

static void app_sig_generic(struct ev_eloop *eloop,
			    struct signalfd_siginfo *info,
			    void *data)
{
	struct kmscon_app *app = data;

	log_info("terminating due to caught signal %d", info->ssi_signo);
	ev_eloop_exit(app->eloop);
}

static void app_sig_ignore(struct ev_eloop *eloop,
			   struct signalfd_siginfo *info,
			   void *data)
{
}

static void destroy_app(struct kmscon_app *app)
{
	uterm_monitor_unref(app->mon);
	uterm_vt_master_unref(app->vtm);
	ev_eloop_unregister_signal_cb(app->eloop, SIGPIPE, app_sig_ignore,
				      app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGINT, app_sig_generic,
				      app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGTERM, app_sig_generic,
				      app);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct kmscon_app *app)
{
	int ret;

	shl_dlist_init(&app->seats);

	ret = ev_eloop_new(&app->eloop, log_llog, NULL);
	if (ret) {
		log_error("cannot create eloop object: %d", ret);
		goto err_app;
	}

	ret = ev_eloop_register_signal_cb(app->eloop, SIGTERM,
					  app_sig_generic, app);
	if (ret) {
		log_error("cannot register SIGTERM signal handler: %d", ret);
		goto err_app;
	}

	ret = ev_eloop_register_signal_cb(app->eloop, SIGINT,
					  app_sig_generic, app);
	if (ret) {
		log_error("cannot register SIGINT signal handler: %d", ret);
		goto err_app;
	}

	ret = ev_eloop_register_signal_cb(app->eloop, SIGPIPE,
					  app_sig_ignore, app);
	if (ret) {
		log_error("cannot register SIGPIPE signal handler: %d", ret);
		goto err_app;
	}

	ret = uterm_vt_master_new(&app->vtm, app->eloop);
	if (ret) {
		log_error("cannot create VT master: %d", ret);
		goto err_app;
	}

	ret = uterm_monitor_new(&app->mon, app->eloop, app_monitor_event, app);
	if (ret) {
		log_error("cannot create device monitor: %d", ret);
		goto err_app;
	}

	log_debug("scanning for devices...");
	uterm_monitor_scan(app->mon);

	return 0;

err_app:
	destroy_app(app);
	return ret;
}

int main(int argc, char **argv)
{
	int ret;
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct kmscon_app app;

	ret = kmscon_conf_new(&conf_ctx);
	if (ret) {
		log_error("cannot create configuration: %d", ret);
		goto err_out;
	}
	conf = conf_ctx_get_mem(conf_ctx);

	ret = kmscon_conf_load_main(conf_ctx, argc, argv);
	if (ret) {
		log_error("cannot load configuration: %d", ret);
		goto err_conf;
	}

	if (conf->exit) {
		kmscon_conf_free(conf_ctx);
		return 0;
	}

	kmscon_load_modules();
	kmscon_font_register(&kmscon_font_8x16_ops);
	kmscon_text_register(&kmscon_text_bblit_ops);

	memset(&app, 0, sizeof(app));
	app.conf_ctx = conf_ctx;
	app.conf = conf;

	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	if (!app.conf->listen && !app.running_seats) {
		log_notice("no running seats; exiting");
	} else {
		log_debug("%u running seats after startup", app.running_seats);
		ev_eloop_run(app.eloop, -1);
	}

	app.exiting = true;

	if (app.conf->switchvt) {
		/* The VT subsystem needs to acknowledge the VT-leave so if it
		 * returns -EINPROGRESS we need to wait for the VT-leave SIGUSR2
		 * signal to arrive. Therefore, we use a separate eloop object
		 * which is used by the VT system only. Therefore, waiting on
		 * this eloop allows us to safely wait 50ms for the SIGUSR2 to
		 * arrive.
		 * We use a timeout of 100ms to avoid hanging on exit. */
		log_debug("deactivating VTs during shutdown");
		ret = uterm_vt_master_deactivate_all(app.vtm);
		if (ret > 0) {
			log_debug("waiting for %d VTs to deactivate", ret);
			app.vt_exit_count = ret;
			ev_eloop_run(app.eloop, 50);
		}
	}

	ret = 0;

	destroy_app(&app);
err_unload:
	kmscon_text_unregister(kmscon_text_bblit_ops.name);
	kmscon_font_unregister(kmscon_font_8x16_ops.name);
	kmscon_unload_modules();
err_conf:
	kmscon_conf_free(conf_ctx);
err_out:
	if (ret)
		log_err("cannot initialize kmscon, errno %d: %s",
			ret, strerror(-ret));
	log_info("exiting");
	return -ret;
}
