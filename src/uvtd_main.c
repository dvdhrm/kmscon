/*
 * uvtd - User-space VT daemon
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "uterm_input.h"
#include "uterm_monitor.h"
#include "uvt.h"
#include "uvtd_ctx.h"

struct app_seat {
	struct shl_dlist list;
	struct uvtd_app *app;
	struct uterm_monitor_seat *useat;
	struct uvtd_ctx *ctx;
};

struct uvtd_app {
	struct ev_eloop *eloop;
	struct uterm_monitor *mon;
	struct uvt_ctx *ctx;
	struct ev_fd *ctx_fd;
	struct shl_dlist seats;
};

static int app_seat_new(struct uvtd_app *app, const char *sname,
			struct uterm_monitor_seat *useat)
{
	struct app_seat *seat;
	int ret;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return -ENOMEM;

	log_debug("new seat %p on %s", seat, sname);

	memset(seat, 0, sizeof(*seat));
	seat->app = app;
	seat->useat = useat;

	ret = uvtd_ctx_new(&seat->ctx, sname, app->eloop, app->ctx);
	if (ret == -EEXIST) {
		log_debug("ignoring seat %s as it has real VTs", sname);
		goto err_free;
	} else if (ret) {
		goto err_free;
	}

	uterm_monitor_set_seat_data(seat->useat, seat);
	shl_dlist_link(&app->seats, &seat->list);
	return 0;

err_free:
	free(seat);
	return ret;
}

static void app_seat_free(struct app_seat *seat)
{
	log_debug("free seat %p", seat);

	shl_dlist_unlink(&seat->list);
	uterm_monitor_set_seat_data(seat->useat, NULL);
	uvtd_ctx_free(seat->ctx);
	free(seat);
}

static void app_monitor_event(struct uterm_monitor *mon,
			      struct uterm_monitor_event *ev,
			      void *data)
{
	struct uvtd_app *app = data;
	struct app_seat *seat;
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
		case UTERM_MONITOR_INPUT:
			log_debug("new input device %s on seat %p",
				  ev->dev_node, seat);
			break;
		}
		break;
	case UTERM_MONITOR_FREE_DEV:
		seat = ev->seat_data;
		if (!seat)
			return;

		switch (ev->dev_type) {
		case UTERM_MONITOR_INPUT:
			log_debug("free input device %s on seat %p",
				  ev->dev_node, seat);
			break;
		}
		break;
	}
}

static void app_sig_generic(struct ev_eloop *eloop,
			    struct signalfd_siginfo *info,
			    void *data)
{
	struct uvtd_app *app = data;

	log_info("terminating due to caught signal %d", info->ssi_signo);
	ev_eloop_exit(app->eloop);
}

static void app_sig_ignore(struct ev_eloop *eloop,
			   struct signalfd_siginfo *info,
			   void *data)
{
}

static void app_ctx_event(struct ev_fd *fd, int mask, void *data)
{
	struct uvtd_app *app = data;

	uvt_ctx_dispatch(app->ctx);

	if (!(mask & EV_READABLE) && mask & (EV_HUP | EV_ERR)) {
		log_error("HUP on UVT ctx fd");
		ev_eloop_rm_fd(fd);
		app->ctx_fd = NULL;
	}
}

static void destroy_app(struct uvtd_app *app)
{
	ev_eloop_rm_fd(app->ctx_fd);
	uvt_ctx_unref(app->ctx);
	uterm_monitor_unref(app->mon);
	ev_eloop_unregister_signal_cb(app->eloop, SIGPIPE, app_sig_ignore,
				      app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGINT, app_sig_generic,
				      app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGTERM, app_sig_generic,
				      app);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct uvtd_app *app)
{
	int ret, fd;

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

	ret = uterm_monitor_new(&app->mon, app->eloop, app_monitor_event, app);
	if (ret) {
		log_error("cannot create device monitor: %d", ret);
		goto err_app;
	}

	ret = uvt_ctx_new(&app->ctx, log_llog, NULL);
	if (ret) {
		log_error("cannot create UVT context: %d", ret);
		goto err_app;
	}

	fd = uvt_ctx_get_fd(app->ctx);
	if (fd >= 0) {
		ret = ev_eloop_new_fd(app->eloop, &app->ctx_fd, fd,
				      EV_READABLE, app_ctx_event, app);
		if (ret) {
			log_error("cannot create UVT ctx efd: %d", ret);
			goto err_app;
		}
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
	struct uvtd_app app;

	log_set_config(&LOG_CONFIG_INFO(1, 1));
	log_print_init("uvtd");

	memset(&app, 0, sizeof(app));

	ret = setup_app(&app);
	if (ret)
		goto err_out;

	ev_eloop_run(app.eloop, -1);

	ret = 0;
	destroy_app(&app);
err_out:
	if (ret)
		log_err("cannot initialize uvtd, errno %d: %s",
			ret, strerror(-ret));
	log_info("exiting");
	return -ret;
}
