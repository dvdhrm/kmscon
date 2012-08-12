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
#include "log.h"
#include "main.h"
#include "static_misc.h"
#include "text.h"
#include "ui.h"
#include "uterm.h"

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

	bool awake;
	struct uterm_vt *vt;
	struct uterm_input *input;
	struct kmscon_ui *ui;
	struct kmscon_dlist videos;
};

struct kmscon_video {
	struct kmscon_dlist list;
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
	struct kmscon_dlist *iter;
	struct kmscon_video *vid;

	if (action == UTERM_VT_ACTIVATE) {
		seat->awake = true;
		uterm_input_wake_up(seat->input);

		kmscon_dlist_for_each(iter, &seat->videos) {
			vid = kmscon_dlist_entry(iter, struct kmscon_video, list);
			uterm_video_wake_up(vid->video);
		}
	} else if (action == UTERM_VT_DEACTIVATE) {
		kmscon_dlist_for_each(iter, &seat->videos) {
			vid = kmscon_dlist_entry(iter, struct kmscon_video, list);
			uterm_video_sleep(vid->video);
		}

		uterm_input_sleep(seat->input);
		seat->awake = false;
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
	kmscon_dlist_init(&seat->videos);

	seat->sname = strdup(sname);
	if (!seat->sname) {
		log_err("cannot allocate memory for seat name");
		goto err_free;
	}

	ret = uterm_vt_allocate(app->vtm, &seat->vt, seat->sname, vt_event,
				seat);
	if (ret)
		goto err_name;

	ret = uterm_input_new(&seat->input, app->eloop,
			      kmscon_conf.xkb_layout,
			      kmscon_conf.xkb_variant,
			      kmscon_conf.xkb_options);
	if (ret)
		goto err_vt;

	ret = kmscon_ui_new(&seat->ui, app->eloop, seat->input);
	if (ret)
		goto err_input;

	uterm_monitor_set_seat_data(seat->useat, seat);
	kmscon_dlist_link(&app->seats, &seat->list);

	log_info("new seat %s", seat->sname);
	return;

err_input:
	uterm_input_unref(seat->input);
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
	struct kmscon_video *vid;

	if ((type == UTERM_MONITOR_FBDEV) != !!kmscon_conf.use_fbdev)
		return;

	vid = malloc(sizeof(*vid));
	if (!vid)
		return;
	memset(vid, 0, sizeof(*vid));
	vid->vdev = dev;

	if (kmscon_conf.use_fbdev)
		mode = UTERM_VIDEO_FBDEV;
	else
		mode = UTERM_VIDEO_DRM;

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
	kmscon_dlist_link(&seat->videos, &vid->list);

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
	struct kmscon_dlist *iter;
	struct kmscon_video *vid;

	kmscon_dlist_for_each(iter, &seat->videos) {
		vid = kmscon_dlist_entry(iter, struct kmscon_video, list);
		if (vid->vdev != dev)
			continue;

		log_debug("free graphics device on seat %s", seat->sname);

		kmscon_ui_remove_video(seat->ui, vid->video);
		uterm_video_unref(vid->video);
		kmscon_dlist_unlink(&vid->list);
		free(vid);

		break;
	}
}

static void seat_hotplug_video(struct kmscon_seat *seat,
			       struct uterm_monitor_dev *dev)
{
	struct kmscon_dlist *iter;
	struct kmscon_video *vid;

	kmscon_dlist_for_each(iter, &seat->videos) {
		vid = kmscon_dlist_entry(iter, struct kmscon_video, list);
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

struct kmscon_conf_t kmscon_conf;

static void print_help()
{
	/*
	 * Usage/Help information
	 * This should be scaled to a maximum of 80 characters per line:
	 *
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
	fprintf(stderr,
		"Usage:\n"
		"\t%1$s [options]\n"
		"\t%1$s -h [options]\n"
		"\t%1$s -l [options] -- /bin/sh [sh-arguments]\n"
		"\n"
		"You can prefix boolean options with \"no-\" to negate it. If an argument is\n"
		"given multiple times, only the last argument matters if not otherwise stated.\n"
		"\n"
		"General Options:\n"
		"\t-h, --help                  [off]   Print this help and exit\n"
		"\t-v, --verbose               [off]   Print verbose messages\n"
		"\t    --debug                 [off]   Enable debug mode\n"
		"\t    --silent                [off]   Suppress notices and warnings\n"
		"\t-s, --switchvt              [off]   Automatically switch to VT\n"
		"\t    --seats <list,of,seats> [seat0] Select seats or pass 'all' to make\n"
		"\t                                    kmscon run on all seats\n"
		"\n"
		"Terminal Options:\n"
		"\t-l, --login                 [/bin/sh]\n"
		"\t                              Start the given login process instead\n"
		"\t                              of the default process; all arguments\n"
		"\t                              following '--' will be be parsed as\n"
		"\t                              argv to this process. No more options\n"
		"\t                              after '--' will be parsed so use it at\n"
		"\t                              the end of the argument string\n"
		"\t-t, --term <TERM>           [vt220]\n"
		"\t                              Value of the TERM environment variable\n"
		"\t                              for the child process\n"
		"\n"
		"Video Options:\n"
		"\t    --fbdev                 [off]   Use fbdev instead of DRM\n"
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-layout <layout>   [us]    Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant> [-]     Set XkbVariant for input devices\n"
		"\t    --xkb-options <options> [-]     Set XkbOptions for input devices\n"
		"\n"
		"Font Options:\n"
		"\t    --font-engine <engine>  [pango] Font engine\n",
		"kmscon");
	/*
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
}

static int aftercheck_debug(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	/* --debug implies --verbose */
	if (kmscon_conf.debug)
		kmscon_conf.verbose = 1;

	return 0;
}

static int aftercheck_help(struct conf_option *opt, int argc, char **argv,
			   int idx)
{
	/* exit after printing --help information */
	if (kmscon_conf.help) {
		print_help();
		kmscon_conf.exit = true;
	}

	return 0;
}

static char *def_argv[] = { NULL, "-i", NULL };

static int aftercheck_login(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	int ret;

	/* parse "--login [...] -- args" arguments */
	if (kmscon_conf.login) {
		if (idx >= argc) {
			fprintf(stderr, "Arguments for --login missing\n");
			return -EFAULT;
		}

		kmscon_conf.argv = &argv[idx];
		ret = argc - idx;
	} else {
		def_argv[0] = getenv("SHELL") ? : _PATH_BSHELL;
		kmscon_conf.argv = def_argv;
		ret = 0;
	}

	return ret;
}

static int aftercheck_seats(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	if (kmscon_conf.seats[0] &&
	    !kmscon_conf.seats[1] &&
	    !strcmp(kmscon_conf.seats[0], "all"))
		kmscon_conf.all_seats = true;

	return 0;
}

static char *def_seats[] = { "seat0", NULL };

struct conf_option options[] = {
	CONF_OPTION_BOOL('h', "help", aftercheck_help, &kmscon_conf.help, false),
	CONF_OPTION_BOOL('v', "verbose", NULL, &kmscon_conf.verbose, false),
	CONF_OPTION_BOOL(0, "debug", aftercheck_debug, &kmscon_conf.debug, false),
	CONF_OPTION_BOOL(0, "silent", NULL, &kmscon_conf.silent, false),
	CONF_OPTION_BOOL(0, "fbdev", NULL, &kmscon_conf.use_fbdev, false),
	CONF_OPTION_BOOL('s', "switchvt", NULL, &kmscon_conf.switchvt, false),
	CONF_OPTION_BOOL('l', "login", aftercheck_login, &kmscon_conf.login, false),
	CONF_OPTION_STRING('t', "term", NULL, &kmscon_conf.term, "vt220"),
	CONF_OPTION_STRING(0, "xkb-layout", NULL, &kmscon_conf.xkb_layout, "us"),
	CONF_OPTION_STRING(0, "xkb-variant", NULL, &kmscon_conf.xkb_variant, ""),
	CONF_OPTION_STRING(0, "xkb-options", NULL, &kmscon_conf.xkb_options, ""),
	CONF_OPTION_STRING(0, "font-engine", NULL, &kmscon_conf.font_engine, "pango"),
	CONF_OPTION_STRING_LIST(0, "seats", aftercheck_seats, &kmscon_conf.seats, def_seats),
};

int main(int argc, char **argv)
{
	int ret;
	struct kmscon_app app;
	size_t onum;

	onum = sizeof(options) / sizeof(*options);
	ret = conf_parse_argv(options, onum, argc, argv);
	if (ret)
		goto err_out;

	if (kmscon_conf.exit) {
		conf_free(options, onum);
		return EXIT_SUCCESS;
	}

	if (!kmscon_conf.debug && !kmscon_conf.verbose && kmscon_conf.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(kmscon_conf.debug,
						kmscon_conf.verbose));

	log_print_init("kmscon");

	ret = conf_parse_all_files(options, onum);
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

	if (kmscon_conf.switchvt) {
		/* TODO: implement automatic VT switching */
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
	conf_free(options, onum);
	log_info("exiting");

	return EXIT_SUCCESS;

err_unload:
	kmscon_text_gltex_unload();
	kmscon_text_bblit_unload();
	kmscon_font_freetype2_unload();
	kmscon_font_pango_unload();
	kmscon_font_8x16_unload();
err_out:
	conf_free(options, onum);
	log_err("cannot initialize kmscon, errno %d: %s", ret, strerror(-ret));
	return -ret;
}
