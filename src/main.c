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
#include "conf.h"
#include "eloop.h"
#include "log.h"
#include "uterm.h"
#include "vt.h"

struct kmscon_app {
	struct ev_eloop *eloop;
	struct ev_eloop *vt_eloop;
	struct ev_signal *sig_term;
	struct ev_signal *sig_int;
	struct kmscon_vt *vt;
	bool exit;
	struct uterm_video *video;
};

static void sig_generic(struct ev_signal *sig, int signum, void *data)
{
	struct kmscon_app *app = data;

	ev_eloop_exit(app->eloop);
	log_info("terminating due to caught signal %d", signum);
}

static void update_displays(struct kmscon_app *app)
{
}

static bool vt_switch(struct kmscon_vt *vt,
			enum kmscon_vt_action action,
			void *data)
{
	struct kmscon_app *app = data;
	int ret;

	if (action == KMSCON_VT_ENTER) {
		ret = uterm_video_wake_up(app->video);
		if (ret) {
			log_err("cannot wake-up video system");
		} else {
			update_displays(app);
		}
	} else if (action == KMSCON_VT_LEAVE) {
		uterm_video_sleep(app->video);
		if (app->exit)
			ev_eloop_exit(app->vt_eloop);
	}

	return true;
}

static void destroy_app(struct kmscon_app *app)
{
	uterm_video_unref(app->video);
	kmscon_vt_unref(app->vt);
	ev_eloop_rm_eloop(app->vt_eloop);
	ev_eloop_rm_signal(app->sig_int);
	ev_eloop_rm_signal(app->sig_term);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct kmscon_app *app)
{
	int ret;

	ret = ev_eloop_new(&app->eloop);
	if (ret)
		goto err_app;

	ret = ev_eloop_new_signal(app->eloop, &app->sig_term, SIGTERM,
					sig_generic, app);
	if (ret)
		goto err_app;

	ret = ev_eloop_new_signal(app->eloop, &app->sig_int, SIGINT,
					sig_generic, app);
	if (ret)
		goto err_app;

	ret = ev_eloop_new_eloop(app->eloop, &app->vt_eloop);
	if (ret)
		goto err_app;

	ret = kmscon_vt_new(&app->vt, vt_switch, app);
	if (ret)
		goto err_app;

	ret = kmscon_vt_open(app->vt, KMSCON_VT_NEW, app->vt_eloop);
	if (ret)
		goto err_app;

	ret = uterm_video_new(&app->video, UTERM_VIDEO_DRM, app->eloop);
	if (ret)
		goto err_app;

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

	if (conf_global.exit)
		return EXIT_SUCCESS;

	log_print_init("kmscon");
	log_set_config(&LOG_CONFIG_INFO(conf_global.debug,
					conf_global.verbose));

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_out;

	if (conf_global.switchvt) {
		ret = kmscon_vt_enter(app.vt);
		if (ret)
			log_warn("cannot enter VT");
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
		app.exit = true;
		ret = kmscon_vt_leave(app.vt);
		if (ret == -EINPROGRESS)
			ev_eloop_run(app.vt_eloop, 50);
	}

	destroy_app(&app);
	log_info("exiting");

	return EXIT_SUCCESS;

err_out:
	log_err("cannot initialize kmscon, errno %d: %s", ret, strerror(-ret));
	return -ret;
}
