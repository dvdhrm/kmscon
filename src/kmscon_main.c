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
#include "kmscon_ui.h"
#include "log.h"
#include "shl_dlist.h"
#include "shl_misc.h"
#include "text.h"
#include "uterm.h"

struct kmscon_app {
	struct ev_eloop *eloop;
	struct ev_eloop *vt_eloop;
	unsigned int vt_exit_count;

	struct uterm_vt_master *vtm;
	struct uterm_monitor *mon;
	struct shl_dlist seats;
};

struct kmscon_seat {
	struct shl_dlist list;
	struct kmscon_app *app;

	struct uterm_monitor_seat *useat;
	char *sname;

	bool awake;
	struct uterm_vt *vt;
	struct uterm_input *input;
	struct kmscon_ui *ui;
	struct shl_dlist videos;
};

struct kmscon_video {
	struct shl_dlist list;
	struct uterm_monitor_dev *vdev;
	struct uterm_video *video;
};

static void sig_generic(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data)
{
	struct kmscon_app *app = data;

	ev_eloop_exit(app->eloop);
	log_info("terminating due to caught signal %d", info->ssi_signo);
}

static int vt_event(struct uterm_vt *vt, unsigned int action, void *data)
{
	struct kmscon_seat *seat = data;
	struct shl_dlist *iter;
	struct kmscon_video *vid;
	struct kmscon_app *app = seat->app;

	if (action == UTERM_VT_ACTIVATE) {
		seat->awake = true;
		uterm_input_wake_up(seat->input);

		shl_dlist_for_each(iter, &seat->videos) {
			vid = shl_dlist_entry(iter, struct kmscon_video, list);
			uterm_video_wake_up(vid->video);
		}
		kmscon_ui_wake_up(seat->ui);
	} else if (action == UTERM_VT_DEACTIVATE) {
		kmscon_ui_sleep(seat->ui);
		shl_dlist_for_each(iter, &seat->videos) {
			vid = shl_dlist_entry(iter, struct kmscon_video, list);
			uterm_video_sleep(vid->video);
		}

		uterm_input_sleep(seat->input);
		seat->awake = false;

		if (app->vt_exit_count > 0) {
			log_debug("deactivating VT on exit, %d to go",
				  app->vt_exit_count - 1);
			if (!--app->vt_exit_count)
				ev_eloop_exit(app->vt_eloop);
		}
	}

	return 0;
}

static void seat_new(struct kmscon_app *app,
		     struct uterm_monitor_seat *useat,
		     const char *sname)
{
	struct kmscon_seat *seat;
	int ret;
	unsigned int i;
	bool found;

	found = false;
	if (kmscon_conf.all_seats) {
		found = true;
	} else {
		for (i = 0; kmscon_conf.seats[i]; ++i) {
			if (!strcmp(kmscon_conf.seats[i], sname)) {
				found = true;
				break;
			}
		}
	}

	if (!found) {
		log_info("ignoring seat %s as not specified in seat-list",
			 sname);
		return;
	}

	seat = malloc(sizeof(*seat));
	if (!seat)
		return;
	memset(seat, 0, sizeof(*seat));
	seat->app = app;
	seat->useat = useat;
	shl_dlist_init(&seat->videos);

	seat->sname = strdup(sname);
	if (!seat->sname) {
		log_err("cannot allocate memory for seat name");
		goto err_free;
	}

	ret = uterm_input_new(&seat->input, app->eloop,
			      kmscon_conf.xkb_layout,
			      kmscon_conf.xkb_variant,
			      kmscon_conf.xkb_options,
			      kmscon_conf.xkb_repeat_delay,
			      kmscon_conf.xkb_repeat_rate);
	if (ret)
		goto err_name;

	ret = uterm_vt_allocate(app->vtm, &seat->vt, seat->sname,
				seat->input, kmscon_conf.vt, vt_event, seat);
	if (ret)
		goto err_input;

	ret = kmscon_ui_new(&seat->ui, app->eloop, seat->input, seat->sname);
	if (ret)
		goto err_vt;

	uterm_monitor_set_seat_data(seat->useat, seat);
	shl_dlist_link(&app->seats, &seat->list);

	log_info("new seat %s", seat->sname);
	return;

err_vt:
	uterm_vt_deallocate(seat->vt);
err_input:
	uterm_input_unref(seat->input);
err_name:
	free(seat->sname);
err_free:
	free(seat);
}

static void seat_free(struct kmscon_seat *seat)
{
	log_info("free seat %s", seat->sname);

	shl_dlist_unlink(&seat->list);
	uterm_monitor_set_seat_data(seat->useat, NULL);
	kmscon_ui_free(seat->ui);
	uterm_input_unref(seat->input);
	uterm_vt_deallocate(seat->vt);
	free(seat->sname);
	free(seat);
}

static void seat_add_video(struct kmscon_seat *seat,
			   struct uterm_monitor_dev *dev,
			   unsigned int type,
			   const char *node)
{
	int ret;
	unsigned int mode;
	struct kmscon_video *vid;

	if (kmscon_conf.use_fbdev) {
		if (type != UTERM_MONITOR_FBDEV &&
		    type != UTERM_MONITOR_FBDEV_DRM) {
			log_debug("ignoring %s as it is not fbdev device",
				  node);
			return;
		}
	} else {
		if (type == UTERM_MONITOR_FBDEV_DRM) {
			log_debug("ignoring %s as it is a DRM-fbdev device",
				  node);
			return;
		}
	}

	vid = malloc(sizeof(*vid));
	if (!vid)
		return;
	memset(vid, 0, sizeof(*vid));
	vid->vdev = dev;

	if (type == UTERM_MONITOR_DRM) {
		if (kmscon_conf.dumb)
			mode = UTERM_VIDEO_DUMB;
		else
			mode = UTERM_VIDEO_DRM;
	} else {
		mode = UTERM_VIDEO_FBDEV;
	}

	ret = uterm_video_new(&vid->video, seat->app->eloop, mode, node);
	if (ret) {
		if (mode == UTERM_VIDEO_DRM) {
			log_info("cannot create drm device; trying dumb drm mode");
			ret = uterm_video_new(&vid->video, seat->app->eloop,
					      UTERM_VIDEO_DUMB, node);
			if (ret)
				goto err_free;
		} else {
			goto err_free;
		}
	}

	kmscon_ui_add_video(seat->ui, vid->video);
	if (seat->awake)
		uterm_video_wake_up(vid->video);
	shl_dlist_link(&seat->videos, &vid->list);

	log_debug("new graphics device on seat %s", seat->sname);
	return;

err_free:
	free(vid);
	log_warning("cannot add video object %s on %s", node, seat->sname);
	return;
}

static void seat_rm_video(struct kmscon_seat *seat,
			  struct uterm_monitor_dev *dev)
{
	struct shl_dlist *iter;
	struct kmscon_video *vid;

	shl_dlist_for_each(iter, &seat->videos) {
		vid = shl_dlist_entry(iter, struct kmscon_video, list);
		if (vid->vdev != dev)
			continue;

		log_debug("free graphics device on seat %s", seat->sname);

		kmscon_ui_remove_video(seat->ui, vid->video);
		uterm_video_unref(vid->video);
		shl_dlist_unlink(&vid->list);
		free(vid);

		break;
	}
}

static void seat_hotplug_video(struct kmscon_seat *seat,
			       struct uterm_monitor_dev *dev)
{
	struct shl_dlist *iter;
	struct kmscon_video *vid;

	shl_dlist_for_each(iter, &seat->videos) {
		vid = shl_dlist_entry(iter, struct kmscon_video, list);
		if (vid->vdev != dev)
			continue;

		uterm_video_poll(vid->video);
		break;
	}
}

static void monitor_event(struct uterm_monitor *mon,
			  struct uterm_monitor_event *ev,
			  void *data)
{
	struct kmscon_app *app = data;
	struct kmscon_seat *seat;

	switch (ev->type) {
	case UTERM_MONITOR_NEW_SEAT:
		seat_new(app, ev->seat, ev->seat_name);
		break;
	case UTERM_MONITOR_FREE_SEAT:
		if (ev->seat_data)
			seat_free(ev->seat_data);
		break;
	case UTERM_MONITOR_NEW_DEV:
		seat = ev->seat_data;
		if (!seat)
			break;
		if (ev->dev_type == UTERM_MONITOR_DRM ||
		    ev->dev_type == UTERM_MONITOR_FBDEV ||
		    ev->dev_type == UTERM_MONITOR_FBDEV_DRM)
			seat_add_video(seat, ev->dev, ev->dev_type,
				       ev->dev_node);
		else if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_add_dev(seat->input, ev->dev_node);
		break;
	case UTERM_MONITOR_FREE_DEV:
		seat = ev->seat_data;
		if (!seat)
			break;
		if (ev->dev_type == UTERM_MONITOR_DRM ||
		    ev->dev_type == UTERM_MONITOR_FBDEV ||
		    ev->dev_type == UTERM_MONITOR_FBDEV_DRM)
			seat_rm_video(seat, ev->dev);
		else if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_remove_dev(seat->input, ev->dev_node);
		break;
	case UTERM_MONITOR_HOTPLUG_DEV:
		seat = ev->seat_data;
		if (!seat)
			break;
		seat_hotplug_video(seat, ev->dev);
		break;
	}
}

static void destroy_app(struct kmscon_app *app)
{
	uterm_monitor_unref(app->mon);
	uterm_vt_master_unref(app->vtm);
	ev_eloop_unregister_signal_cb(app->eloop, SIGINT, sig_generic, app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGTERM, sig_generic, app);
	ev_eloop_rm_eloop(app->vt_eloop);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct kmscon_app *app)
{
	int ret;

	ret = ev_eloop_new(&app->eloop, log_llog);
	if (ret)
		goto err_app;

	ret = ev_eloop_register_signal_cb(app->eloop, SIGTERM,
						sig_generic, app);
	if (ret)
		goto err_app;

	ret = ev_eloop_register_signal_cb(app->eloop, SIGINT,
						sig_generic, app);
	if (ret)
		goto err_app;

	ret = ev_eloop_new_eloop(app->eloop, &app->vt_eloop);
	if (ret)
		goto err_app;

	ret = uterm_vt_master_new(&app->vtm, app->vt_eloop);
	if (ret)
		goto err_app;

	shl_dlist_init(&app->seats);

	ret = uterm_monitor_new(&app->mon, app->eloop, monitor_event, app);
	if (ret)
		goto err_app;

	uterm_monitor_scan(app->mon);

	return 0;

err_app:
	destroy_app(app);
	return ret;
}

int main(int argc, char **argv)
{
	int ret;
	struct kmscon_app app;

	ret = kmscon_load_config(argc, argv);
	if (ret) {
		log_error("cannot parse configuration: %d", ret);
		goto err_out;
	}

	if (kmscon_conf.exit) {
		kmscon_free_config();
		return 0;
	}

	kmscon_font_load_all();
	kmscon_text_load_all();

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	if (kmscon_conf.switchvt) {
		log_debug("activating VTs during startup");
		uterm_vt_master_activate_all(app.vtm);
	}

	ev_eloop_run(app.eloop, -1);

	if (kmscon_conf.switchvt) {
		/* The VT subsystem needs to acknowledge the VT-leave so if it
		 * returns -EINPROGRESS we need to wait for the VT-leave SIGUSR2
		 * signal to arrive. Therefore, we use a separate eloop object
		 * which is used by the VT system only. Therefore, waiting on
		 * this eloop allows us to safely wait 50ms for the SIGUSR2 to
		 * arrive.
		 * We use a timeout of 100ms to avoid haning on exit.
		 * We could also wait on app.eloop but this would allow other
		 * subsystems to continue receiving events and this is not what
		 * we want. */
		log_debug("deactivating VTs during shutdown");
		ret = uterm_vt_master_deactivate_all(app.vtm);
		if (ret > 0) {
			log_debug("waiting for %d VTs to deactivate", ret);
			app.vt_exit_count = ret;
			ev_eloop_run(app.vt_eloop, 50);
		}
	}

	ret = 0;

	destroy_app(&app);
err_unload:
	kmscon_text_unload_all();
	kmscon_font_unload_all();
err_out:
	kmscon_free_config();

	if (ret)
		log_err("cannot initialize kmscon, errno %d: %s",
			ret, strerror(-ret));
	log_info("exiting");
	return -ret;
}
