/*
 * fakevt - Fake Terminal Helper
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

/*
 * Fake Terminal
 * This is a daemon which is run system wide and listens for input devices. It
 * is seat-aware and everything affects only the seat where the input data was
 * received.
 * On special key-combinations this sends a SIGUSR1 and SIGUSR2 to all global
 * kmscon instances. This will notify kmscon to activate or deactivate the
 * fake-seat. This does not affect seat0 if it uses real-VTs.
 *
 * This should be used for debugging only! You may create an emergency tool
 * based on this, but then you should probably improve the logic a bit.
 */

#include <errno.h>
#include <paths.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <X11/keysym.h>
#include "conf.h"
#include "eloop.h"
#include "log.h"
#include "static_misc.h"
#include "uterm.h"

struct fakevt_app {
	struct ev_eloop *eloop;

	struct uterm_monitor *mon;
	struct kmscon_dlist seats;
};

struct fakevt_seat {
	struct kmscon_dlist list;
	struct fakevt_app *app;

	bool active;
	struct uterm_monitor_seat *useat;
	char *sname;

	struct uterm_input *input;
};

struct {
	bool debug;
	bool verbose;
	bool help;
	bool silent;
	bool exit;

	bool all_seats;
	char **seats;

	char *xkb_layout;
	char *xkb_variant;
	char *xkb_options;
} fakevt_conf;

static void sig_generic(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data)
{
	struct fakevt_app *app = data;

	ev_eloop_exit(app->eloop);
	log_info("terminating due to caught signal %d", info->ssi_signo);
}

static void activate_seat(struct fakevt_seat *seat)
{
	log_info("activating kmscon");
	system("killall -SIGUSR1 kmscon");
}

static void deactivate_seat(struct fakevt_seat *seat)
{
	log_info("deactivating kmscon");
	system("killall -SIGUSR2 kmscon");
}

static void input_event(struct uterm_input *input,
			struct uterm_input_event *ev,
			void *data)
{
	struct fakevt_seat *seat = data;

	if (UTERM_INPUT_HAS_MODS(ev, UTERM_CONTROL_MASK | UTERM_MOD4_MASK)) {
		if (ev->keysym == XK_F12) {
			if (seat->active)
				deactivate_seat(seat);
			else
				activate_seat(seat);
			seat->active = !seat->active;
		}
	}
}

static void seat_new(struct fakevt_app *app,
		     struct uterm_monitor_seat *useat,
		     const char *sname)
{
	struct fakevt_seat *seat;
	int ret;
	unsigned int i;
	bool found;

	found = false;
	if (fakevt_conf.all_seats) {
		found = true;
	} else {
		for (i = 0; fakevt_conf.seats[i]; ++i) {
			if (!strcmp(fakevt_conf.seats[i], sname)) {
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
	seat->active = false;

	seat->sname = strdup(sname);
	if (!seat->sname) {
		log_err("cannot allocate memory for seat name");
		goto err_free;
	}

	ret = uterm_input_new(&seat->input, app->eloop,
			      fakevt_conf.xkb_layout,
			      fakevt_conf.xkb_variant,
			      fakevt_conf.xkb_options);
	if (ret)
		goto err_name;

	ret = uterm_input_register_cb(seat->input, input_event, seat);
	if (ret)
		goto err_input;

	uterm_input_wake_up(seat->input);
	uterm_monitor_set_seat_data(seat->useat, seat);
	kmscon_dlist_link(&app->seats, &seat->list);

	log_info("new seat %s", seat->sname);
	return;

err_input:
	uterm_input_unref(seat->input);
err_name:
	free(seat->sname);
err_free:
	free(seat);
}

static void seat_free(struct fakevt_seat *seat)
{
	log_info("free seat %s", seat->sname);

	kmscon_dlist_unlink(&seat->list);
	uterm_monitor_set_seat_data(seat->useat, NULL);
	uterm_input_unregister_cb(seat->input, input_event, seat);
	uterm_input_unref(seat->input);
	free(seat->sname);
	free(seat);
}

static void monitor_event(struct uterm_monitor *mon,
			  struct uterm_monitor_event *ev,
			  void *data)
{
	struct fakevt_app *app = data;
	struct fakevt_seat *seat;

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
		if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_add_dev(seat->input, ev->dev_node);
		break;
	case UTERM_MONITOR_FREE_DEV:
		seat = ev->seat_data;
		if (!seat)
			break;
		if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_remove_dev(seat->input, ev->dev_node);
		break;
	}
}

static void destroy_app(struct fakevt_app *app)
{
	uterm_monitor_unref(app->mon);
	ev_eloop_unregister_signal_cb(app->eloop, SIGINT, sig_generic, app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGTERM, sig_generic, app);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct fakevt_app *app)
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
		"\n"
		"You can prefix boolean options with \"no-\" to negate it. If an argument is\n"
		"given multiple times, only the last argument matters if not otherwise stated.\n"
		"\n"
		"General Options:\n"
		"\t-h, --help                  [off]   Print this help and exit\n"
		"\t-v, --verbose               [off]   Print verbose messages\n"
		"\t    --debug                 [off]   Enable debug mode\n"
		"\t    --silent                [off]   Suppress notices and warnings\n"
		"\t    --seats <list,of,seats> [seat0] Select seats or pass 'all' to make\n"
		"\t                                    fakevt run on all seats\n"
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-layout <layout>   [us]    Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant> [-]     Set XkbVariant for input devices\n"
		"\t    --xkb-options <options> [-]     Set XkbOptions for input devices\n",
		"fakevt");
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
	if (fakevt_conf.debug)
		fakevt_conf.verbose = 1;

	return 0;
}

static int aftercheck_help(struct conf_option *opt, int argc, char **argv,
			   int idx)
{
	/* exit after printing --help information */
	if (fakevt_conf.help) {
		print_help();
		fakevt_conf.exit = true;
	}

	return 0;
}

static int aftercheck_seats(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	if (fakevt_conf.seats[0] &&
	    !fakevt_conf.seats[1] &&
	    !strcmp(fakevt_conf.seats[0], "all"))
		fakevt_conf.all_seats = true;

	return 0;
}

static char *def_seats[] = { "seat0", NULL };

struct conf_option options[] = {
	CONF_OPTION_BOOL('h', "help", aftercheck_help, &fakevt_conf.help, false),
	CONF_OPTION_BOOL('v', "verbose", NULL, &fakevt_conf.verbose, false),
	CONF_OPTION_BOOL(0, "debug", aftercheck_debug, &fakevt_conf.debug, false),
	CONF_OPTION_BOOL(0, "silent", NULL, &fakevt_conf.silent, false),
	CONF_OPTION_STRING(0, "xkb-layout", NULL, &fakevt_conf.xkb_layout, "us"),
	CONF_OPTION_STRING(0, "xkb-variant", NULL, &fakevt_conf.xkb_variant, ""),
	CONF_OPTION_STRING(0, "xkb-options", NULL, &fakevt_conf.xkb_options, ""),
	CONF_OPTION_STRING_LIST(0, "seats", aftercheck_seats, &fakevt_conf.seats, def_seats),
};

int main(int argc, char **argv)
{
	int ret;
	struct fakevt_app app;
	size_t onum;

	onum = sizeof(options) / sizeof(*options);
	ret = conf_parse_argv(options, onum, argc, argv);
	if (ret)
		goto err_out;

	if (fakevt_conf.exit) {
		conf_free(options, onum);
		return EXIT_SUCCESS;
	}

	if (!fakevt_conf.debug && !fakevt_conf.verbose && fakevt_conf.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(fakevt_conf.debug,
						fakevt_conf.verbose));

	log_print_init("fakevt");

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_out;

	ev_eloop_run(app.eloop, -1);

	destroy_app(&app);
	conf_free(options, onum);
	log_info("exiting");

	return EXIT_SUCCESS;

err_out:
	conf_free(options, onum);
	log_err("cannot initialize fakevt, errno %d: %s", ret, strerror(-ret));
	return -ret;
}
