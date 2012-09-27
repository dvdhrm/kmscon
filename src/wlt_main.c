/*
 * wlt - Wayland Terminal
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
 * Wayland Terminal main application
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <wayland-client.h>
#include "conf.h"
#include "eloop.h"
#include "log.h"
#include "text.h"
#include "wlt_main.h"
#include "wlt_terminal.h"
#include "wlt_theme.h"
#include "wlt_toolkit.h"

#define LOG_SUBSYSTEM "wlt"

struct wlt_app {
	struct ev_eloop *eloop;
	struct wlt_display *disp;
	struct wlt_window *wnd;
};

static void sig_generic(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data)
{
	struct wlt_app *app = data;

	ev_eloop_exit(app->eloop);
	log_info("terminating due to caught signal %d", info->ssi_signo);
}

static void window_close(struct wlt_window *wnd, void *data)
{
	struct wlt_app *app = data;

	log_info("closing window");
	ev_eloop_exit(app->eloop);
}

static void terminal_close(struct wlt_terminal *term, unsigned int event,
			   void *data)
{
	struct wlt_app *app = data;

	if (event == WLT_TERMINAL_HUP) {
		log_info("closing pty");
		ev_eloop_exit(app->eloop);
	}
}

static int window_init(struct wlt_app *app)
{
	int ret;
	struct wlt_theme *theme;
	struct wlt_terminal *term;

	ret = wlt_display_create_window(app->disp, &app->wnd,
					600, 400, app);
	if (ret) {
		log_error("cannot create wayland window");
		return ret;
	}
	wlt_window_set_close_cb(app->wnd, window_close);

	ret = wlt_theme_new(&theme, app->wnd);
	if (ret) {
		log_error("cannot create theme");
		return ret;
	}

	ret = wlt_terminal_new(&term, app->wnd);
	if (ret) {
		log_error("cannot create terminal");
		return ret;
	}

	ret = wlt_terminal_open(term, terminal_close, app);
	if (ret) {
		log_error("cannot open terminal");
		return ret;
	}

	return 0;
}

static void display_event(struct wlt_display *disp, unsigned int event,
			  void *data)
{
	struct wlt_app *app = data;
	int ret;

	switch (event) {
	case WLT_DISPLAY_READY:
		log_info("wayland display initialized");
		ret = window_init(app);
		if (ret)
			ev_eloop_exit(app->eloop);
		break;
	case WLT_DISPLAY_HUP:
		log_info("wayland display connection lost");
		ev_eloop_exit(app->eloop);
		break;
	}
}

static void destroy_app(struct wlt_app *app)
{
	wlt_window_unref(app->wnd);
	wlt_display_unregister_cb(app->disp, display_event, app);
	wlt_display_unref(app->disp);
	ev_eloop_unregister_signal_cb(app->eloop, SIGINT, sig_generic, app);
	ev_eloop_unregister_signal_cb(app->eloop, SIGTERM, sig_generic, app);
	ev_eloop_unref(app->eloop);
}

static int setup_app(struct wlt_app *app)
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

	ret = wlt_display_new(&app->disp, app->eloop);
	if (ret)
		goto err_app;

	ret = wlt_display_register_cb(app->disp, display_event, app);
	if (ret)
		goto err_app;

	return 0;

err_app:
	destroy_app(app);
	return ret;
}

struct wlt_conf_t wlt_conf;

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
		"\n"
		"Font Options:\n"
		"\t    --font-engine <engine>  [pango]\n"
		"\t                              Font engine\n"
		"\t    --font-size <points>    [15]\n"
		"\t                              Font size in points\n"
		"\t    --font-name <name>      [monospace]\n"
		"\t                              Font name\n"
		"\t    --font-dpi <dpi>        [96]\n"
		"\t                              Force DPI value for all fonts\n",
		"wlterm");
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
	if (wlt_conf.debug)
		wlt_conf.verbose = 1;

	return 0;
}

static int aftercheck_help(struct conf_option *opt, int argc, char **argv,
			   int idx)
{
	/* exit after printing --help information */
	if (wlt_conf.help) {
		print_help();
		wlt_conf.exit = true;
	}

	return 0;
}

struct conf_option options[] = {
	CONF_OPTION_BOOL('h', "help", aftercheck_help, &wlt_conf.help, false),
	CONF_OPTION_BOOL('v', "verbose", NULL, &wlt_conf.verbose, false),
	CONF_OPTION_BOOL(0, "debug", aftercheck_debug, &wlt_conf.debug, false),
	CONF_OPTION_BOOL(0, "silent", NULL, &wlt_conf.silent, false),
	CONF_OPTION_STRING(0, "font-engine", NULL, &wlt_conf.font_engine, "pango"),
	CONF_OPTION_UINT(0, "font-size", NULL, &wlt_conf.font_size, 12),
	CONF_OPTION_STRING(0, "font-name", NULL, &wlt_conf.font_name, "monospace"),
	CONF_OPTION_UINT(0, "font-dpi", NULL, &wlt_conf.font_ppi, 96),
};

int main(int argc, char **argv)
{
	int ret;
	struct wlt_app app;
	size_t onum;

	onum = sizeof(options) / sizeof(*options);
	ret = conf_parse_argv(options, onum, argc, argv);
	if (ret)
		goto err_out;

	if (wlt_conf.exit) {
		conf_free(options, onum);
		return EXIT_SUCCESS;
	}

	if (!wlt_conf.debug && !wlt_conf.verbose && wlt_conf.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(wlt_conf.debug,
						wlt_conf.verbose));

	log_print_init("wlterm");

	kmscon_font_load_all();

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	ev_eloop_run(app.eloop, -1);

	destroy_app(&app);
	kmscon_font_unload_all();
	conf_free(options, onum);
	log_info("exiting");

	return EXIT_SUCCESS;

err_unload:
	kmscon_font_unload_all();
err_out:
	conf_free(options, onum);
	log_err("cannot initialize wlterm, errno %d: %s", ret, strerror(-ret));
	return -ret;
}
