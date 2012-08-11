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
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include "conf.h"
#include "eloop.h"
#include "log.h"
#include "static_misc.h"
#include "text.h"
#include "ui.h"
#include "uterm.h"
#include "vt.h"

struct kmscon_app {
	struct ev_eloop *eloop;
	struct ev_eloop *vt_eloop;

	struct uterm_vt_master *vtm;
	struct uterm_monitor *mon;
	struct kmscon_dlist seats;
};

struct kmscon_seat {
	struct kmscon_dlist list;
	struct kmscon_app *app;

	struct uterm_monitor_seat *useat;
	char *sname;

	/*
	 * Session Data
	 * We currently allow only a single session on each seat. That is, only
	 * one kmscon instance can run in a single process on each seat. If you
	 * want multiple instances, then start kmscon twice.
	 * We also allow only a single graphics card per seat.
	 * TODO: support multiple sessions per seat in a single kmscon process
	 */

	struct uterm_vt *vt;
	struct uterm_input *input;
	struct uterm_monitor_dev *vdev;
	struct uterm_video *video;
	struct kmscon_ui *ui;
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

	if (action == UTERM_VT_ACTIVATE) {
		uterm_input_wake_up(seat->input);
		uterm_video_wake_up(seat->video);
	} else if (action == UTERM_VT_DEACTIVATE) {
		uterm_video_sleep(seat->video);
		uterm_input_sleep(seat->input);
	}

	return 0;
}

static void seat_new(struct kmscon_app *app,
		     struct uterm_monitor_seat *useat,
		     const char *sname)
{
	struct kmscon_seat *seat;
	int ret;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return;
	memset(seat, 0, sizeof(*seat));
	seat->app = app;
	seat->useat = useat;

	seat->sname = strdup(sname);
	if (!seat->sname) {
		log_err("cannot allocate memory for seat name");
		goto err_free;
	}

	ret = uterm_vt_allocate(app->vtm, &seat->vt, seat->sname, vt_event,
				seat);
	if (ret)
		goto err_name;

	ret = uterm_input_new(&seat->input, app->eloop);
	if (ret)
		goto err_vt;

	uterm_monitor_set_seat_data(seat->useat, seat);
	kmscon_dlist_link(&app->seats, &seat->list);

	log_info("new seat %s", seat->sname);
	return;

err_vt:
	uterm_vt_deallocate(seat->vt);
err_name:
	free(seat->sname);
err_free:
	free(seat);
}

static void seat_free(struct kmscon_seat *seat)
{
	log_info("free seat %s", seat->sname);

	kmscon_dlist_unlink(&seat->list);
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

	if (seat->video)
		return;
	if ((type == UTERM_MONITOR_FBDEV) != !!conf_global.use_fbdev)
		return;

	if (conf_global.use_fbdev)
		mode = UTERM_VIDEO_FBDEV;
	else
		mode = UTERM_VIDEO_DRM;

	ret = uterm_video_new(&seat->video, seat->app->eloop, mode, node);
	if (ret) {
		if (mode == UTERM_VIDEO_DRM) {
			log_info("cannot create drm device; trying dumb drm mode");
			ret = uterm_video_new(&seat->video, seat->app->eloop,
					      UTERM_VIDEO_DUMB, node);
			if (ret)
				return;
		} else {
			return;
		}
	}

	ret = kmscon_ui_new(&seat->ui, seat->app->eloop,
			    seat->input);
	if (ret) {
		log_error("cannot create UI object");
		goto err_video;
	}

	kmscon_ui_add_video(seat->ui, seat->video);

	seat->vdev = dev;
	log_debug("new graphics device on seat %s", seat->sname);
	return;

err_video:
	uterm_video_unref(seat->video);
	seat->video = NULL;
}

static void seat_rm_video(struct kmscon_seat *seat,
			  struct uterm_monitor_dev *dev)
{
	if (!seat->video || seat->vdev != dev)
		return;

	log_debug("free graphics device on seat %s", seat->sname);

	kmscon_ui_remove_video(seat->ui, seat->video);
	kmscon_ui_free(seat->ui);
	seat->ui = NULL;
	seat->vdev = NULL;
	uterm_video_unref(seat->video);
	seat->video = NULL;
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
		    ev->dev_type == UTERM_MONITOR_FBDEV)
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
		    ev->dev_type == UTERM_MONITOR_FBDEV)
			seat_rm_video(seat, ev->dev);
		else if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_remove_dev(seat->input, ev->dev_node);
		break;
	case UTERM_MONITOR_HOTPLUG_DEV:
		seat = ev->seat_data;
		if (!seat || seat->vdev != ev->dev)
			break;
		uterm_video_poll(seat->video);
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

	kmscon_dlist_init(&app->seats);

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

	ret = conf_parse_argv(argc, argv);
	if (ret)
		goto err_out;

	if (conf_global.exit) {
		conf_free();
		return EXIT_SUCCESS;
	}

	if (!conf_global.debug && !conf_global.verbose && conf_global.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(conf_global.debug,
						conf_global.verbose));

	log_print_init("kmscon");

	ret = conf_parse_all_files();
	if (ret)
		goto err_out;

	kmscon_font_8x16_load();
	kmscon_font_pango_load();
	kmscon_font_freetype2_load();
	kmscon_text_bblit_load();
	kmscon_text_gltex_load();

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	if (conf_global.switchvt) {
		/* TODO: implement automatic VT switching */
	}

	ev_eloop_run(app.eloop, -1);

	if (conf_global.switchvt) {
		/* The VT subsystem needs to acknowledge the VT-leave so if it
		 * returns -EINPROGRESS we need to wait for the VT-leave SIGUSR2
		 * signal to arrive. Therefore, we use a separate eloop object
		 * which is used by the VT system only. Therefore, waiting on
		 * this eloop allows us to safely wait 50ms for the SIGUSR2 to
		 * arrive.
		 * We use a timeout of 100ms to avoid haning on exit.
		 * We could also wait on app.eloop but this would allow other
		 * subsystems to continue receiving events and this is not what
		 * we want.
		 */
		if (ret == -EINPROGRESS)
			ev_eloop_run(app.vt_eloop, 50);
	}

	destroy_app(&app);
	kmscon_text_gltex_unload();
	kmscon_text_bblit_unload();
	kmscon_font_freetype2_unload();
	kmscon_font_pango_unload();
	kmscon_font_8x16_unload();
	conf_free();
	log_info("exiting");

	return EXIT_SUCCESS;

err_unload:
	kmscon_text_gltex_unload();
	kmscon_text_bblit_unload();
	kmscon_font_freetype2_unload();
	kmscon_font_pango_unload();
	kmscon_font_8x16_unload();
err_out:
	conf_free();
	log_err("cannot initialize kmscon, errno %d: %s", ret, strerror(-ret));
	return -ret;
}
