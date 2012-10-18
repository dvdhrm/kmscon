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
#include "kmscon_seat.h"
#include "log.h"
#include "shl_dlist.h"
#include "shl_misc.h"
#include "text.h"
#include "uterm.h"

struct app_video {
	struct shl_dlist list;
	struct app_seat *seat;

	char *node;
	struct uterm_video *video;
};

struct app_seat {
	struct shl_dlist list;
	struct kmscon_app *app;

	bool awake;
	char *name;
	struct kmscon_seat *seat;
	struct shl_dlist videos;
};

struct kmscon_app {
	struct ev_eloop *eloop;
	struct ev_eloop *vt_eloop;
	unsigned int vt_exit_count;

	struct uterm_vt_master *vtm;
	struct uterm_monitor *mon;
	struct shl_dlist seats;
};

static void app_seat_event(struct kmscon_seat *s, unsigned int event,
			   void *data)
{
	struct app_seat *seat = data;
	struct kmscon_app *app = seat->app;
	struct shl_dlist *iter;
	struct app_video *vid;

	switch (event) {
	case KMSCON_SEAT_WAKE_UP:
		seat->awake = true;

		shl_dlist_for_each(iter, &seat->videos) {
			vid = shl_dlist_entry(iter, struct app_video, list);
			uterm_video_wake_up(vid->video);
		}
		break;
	case KMSCON_SEAT_SLEEP:
		shl_dlist_for_each(iter, &seat->videos) {
			vid = shl_dlist_entry(iter, struct app_video, list);
			uterm_video_sleep(vid->video);
		}

		if (app->vt_exit_count > 0) {
			log_debug("deactivating VT on exit, %d to go",
				  app->vt_exit_count - 1);
			if (!--app->vt_exit_count)
				ev_eloop_exit(app->vt_eloop);
		}

		seat->awake = false;
		break;
	}
}

static int app_seat_new(struct kmscon_app *app, struct app_seat **out,
			const char *sname)
{
	struct app_seat *seat;
	int ret;
	unsigned int i;
	bool found;

	found = false;
	if (KMSCON_CONF_BOOL(all_seats)) {
		found = true;
	} else {
		for (i = 0; KMSCON_CONF_STRINGLIST(seats)[i]; ++i) {
			if (!strcmp(KMSCON_CONF_STRINGLIST(seats)[i], sname)) {
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
	shl_dlist_init(&seat->videos);

	seat->name = strdup(sname);
	if (!seat->name) {
		log_error("cannot copy seat name on seat %s", sname);
		ret = -ENOMEM;
		goto err_free;
	}

	ret = kmscon_seat_new(&seat->seat, app->eloop, app->vtm, sname,
			      app_seat_event, seat);
	if (ret) {
		log_error("cannot create seat object on seat %s: %d",
			  sname, ret);
		goto err_name;
	}

	shl_dlist_link(&app->seats, &seat->list);
	*out = seat;
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
		kmscon_seat_add_display(vid->seat->seat, ev->display);
		break;
	case UTERM_GONE:
		kmscon_seat_remove_display(vid->seat->seat, ev->display);
		break;
	}
}

static int app_seat_add_video(struct app_seat *seat,
			      struct app_video **out,
			      unsigned int type,
			      const char *node)
{
	int ret;
	unsigned int mode;
	struct app_video *vid;

	if (KMSCON_CONF_BOOL(fbdev)) {
		if (type != UTERM_MONITOR_FBDEV &&
		    type != UTERM_MONITOR_FBDEV_DRM) {
			log_info("ignoring video device %s on seat %s as it is not an fbdev device",
				  node, seat->name);
			return -ERANGE;
		}
	} else {
		if (type == UTERM_MONITOR_FBDEV_DRM) {
			log_info("ignoring video device %s on seat %s as it is a DRM-fbdev device",
				  node, seat->name);
			return -ERANGE;
		}
	}

	log_debug("new video device %s on seat %s", node, seat->name);

	vid = malloc(sizeof(*vid));
	if (!vid) {
		log_error("cannot allocate memory for video device %s on seat %s",
			  node, seat->name);
		return -ENOMEM;
	}
	memset(vid, 0, sizeof(*vid));
	vid->seat = seat;

	vid->node = strdup(node);
	if (!vid->node) {
		log_error("cannot copy video device name %s on seat %s",
			  node, seat->name);
		ret = -ENOMEM;
		goto err_free;
	}

	if (type == UTERM_MONITOR_DRM) {
		if (KMSCON_CONF_BOOL(dumb))
			mode = UTERM_VIDEO_DUMB;
		else
			mode = UTERM_VIDEO_DRM;
	} else {
		mode = UTERM_VIDEO_FBDEV;
	}

	ret = uterm_video_new(&vid->video, seat->app->eloop, mode, node);
	if (ret) {
		if (mode == UTERM_VIDEO_DRM) {
			log_info("cannot create drm device %s on seat %s (%d); trying dumb drm mode",
				 vid->node, seat->name, ret);
			ret = uterm_video_new(&vid->video, seat->app->eloop,
					      UTERM_VIDEO_DUMB, node);
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

	shl_dlist_link(&seat->videos, &vid->list);
	*out = vid;
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
	log_debug("free video device %s on seat %s", vid->node, seat->name);

	shl_dlist_unlink(&vid->list);
	uterm_video_unregister_cb(vid->video, app_seat_video_event, vid);
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
		ret = app_seat_new(app, &seat, ev->seat_name);
		if (ret)
			return;
		uterm_monitor_set_seat_data(ev->seat, seat);
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
		case UTERM_MONITOR_FBDEV_DRM:
			ret = app_seat_add_video(seat, &vid, ev->dev_type,
						 ev->dev_node);
			if (ret)
				return;
			uterm_monitor_set_dev_data(ev->dev, vid);
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
		case UTERM_MONITOR_FBDEV_DRM:
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
		case UTERM_MONITOR_FBDEV_DRM:
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

static void destroy_app(struct kmscon_app *app)
{
	uterm_monitor_unref(app->mon);
	uterm_vt_master_unref(app->vtm);
	ev_eloop_rm_eloop(app->vt_eloop);
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

	ret = ev_eloop_new(&app->eloop, log_llog);
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

	ret = ev_eloop_new_eloop(app->eloop, &app->vt_eloop);
	if (ret) {
		log_error("cannot create VT eloop object: %d", ret);
		goto err_app;
	}

	ret = uterm_vt_master_new(&app->vtm, app->vt_eloop);
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
	struct kmscon_app app;

	ret = kmscon_load_config(argc, argv);
	if (ret) {
		log_error("cannot parse configuration: %d", ret);
		goto err_out;
	}

	if (KMSCON_CONF_BOOL(exit)) {
		kmscon_free_config();
		return 0;
	}

	kmscon_font_load_all();
	kmscon_text_load_all();

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	if (KMSCON_CONF_BOOL(switchvt)) {
		log_debug("activating VTs during startup");
		uterm_vt_master_activate_all(app.vtm);
	}

	ev_eloop_run(app.eloop, -1);

	if (KMSCON_CONF_BOOL(switchvt)) {
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
