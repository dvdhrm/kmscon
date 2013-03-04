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
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <wayland-client.h>
#include "conf.h"
#include "eloop.h"
#include "font.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "shl_misc.h"
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

/* TODO: The font layer depends on kmscon modules. However, the WLT applications
 * doesn't use them. Therefore, we provide dummy kmscon_module_* helpers here
 * which satisfy the dependencies and allow us to link to font.so. */

void kmscon_module_ref(struct kmscon_module *m)
{
}

void kmscon_module_unref(struct kmscon_module *m)
{
}

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

	ret = ev_eloop_new(&app->eloop, log_llog, NULL);
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
#define WLT_CONF_FROM_FIELD(_mem, _name) \
	shl_offsetof((_mem), struct wlt_conf_t, _name)

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
		"\n"
		"Terminal Options:\n"
		"\t-l, --login                 [/bin/sh]\n"
		"\t                              Start the given login process instead\n"
		"\t                              of the default process; all arguments\n"
		"\t                              following '--' will be be parsed as\n"
		"\t                              argv to this process. No more options\n"
		"\t                              after '--' will be parsed so use it at\n"
		"\t                              the end of the argument string\n"
		"\t-t, --term <TERM>           [xterm-256color]\n"
		"\t                              Value of the TERM environment variable\n"
		"\t                              for the child process\n"
		"\t    --palette <name>        [default]\n"
		"\t                              Select the used color palette\n"
		"\t    --sb-size <num>         [1000]\n"
		"\t                              Size of the scrollback-buffer in lines\n"
		"\n"
		"Keyboard Shortcuts and Grabs:\n"
		"\t    --grab-scroll-up <grab>   [<Shift>Up]\n"
		"\t                                Shortcut to scroll up\n"
		"\t    --grab-scroll-down <grab> [<Shift>Down]\n"
		"\t                                Shortcut to scroll down\n"
		"\t    --grab-page-up <grab>     [<Shift>Prior]\n"
		"\t                                Shortcut to scroll page up\n"
		"\t    --grab-page-down <grab>   [<Shift>Next]\n"
		"\t                                Shortcut to scroll page down\n"
		"\t    --grab-fullscreen <grab>  [F11]\n"
		"\t                                Shortcut to toggle fullscreen mode\n"
		"\t    --grab-zoom-in <grab>     [<Ctrl>plus]\n"
		"\t                                Shortcut to increase font size\n"
		"\t    --grab-zoom-out <grab>    [<Ctrl>minus]\n"
		"\t                                Shortcut to decrease font size\n"
		"\t    --grab-copy <grab>        [<Logo>c]\n"
		"\t                                Copy selected text\n"
		"\t    --grab-paste <grab>       [<Logo>v]\n"
		"\t                                Paste selection buffer\n"
		"\n"
		"Font Options:\n"
		"\t    --font-engine <engine>  [pango]\n"
		"\t                              Font engine\n"
		"\t    --font-size <points>    [15]\n"
		"\t                              Font size in points\n"
		"\t    --font-name <name>      [monospace]\n"
		"\t                              Font name\n"
		"\t    --font-dpi <dpi>        [96]\n"
		"\t                              Force DPI value for all fonts\n"
		"\n"
		"Input Options:\n"
		"\t    --xkb-repeat-delay <msecs> [250]\n"
		"\t                                 Initial delay for key-repeat in ms\n"
		"\t    --xkb-repeat-rate <msecs>  [50]\n"
		"\t                                 Delay between two key-repeats in ms\n",
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
	struct wlt_conf_t *conf = WLT_CONF_FROM_FIELD(opt->mem, debug);

	/* --debug implies --verbose */
	if (conf->debug)
		conf->verbose = true;

	return 0;
}

static int aftercheck_help(struct conf_option *opt, int argc, char **argv,
			   int idx)
{
	struct wlt_conf_t *conf = WLT_CONF_FROM_FIELD(opt->mem, help);

	/* exit after printing --help information */
	if (conf->help) {
		print_help();
		conf->exit = true;
	}

	return 0;
}

static char *def_argv[] = { NULL, "-i", NULL };

static int aftercheck_login(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	struct wlt_conf_t *conf = WLT_CONF_FROM_FIELD(opt->mem, login);
	int ret;

	if (!argv)
		return 0;

	/* parse "--login [...] -- args" arguments */
	if (argv && conf->login) {
		if (idx >= argc) {
			fprintf(stderr, "Arguments for --login missing\n");
			return -EFAULT;
		}

		conf->argv = &argv[idx];
		ret = argc - idx;
	} else if (!conf->argv) {
		def_argv[0] = getenv("SHELL") ? : _PATH_BSHELL;
		conf->argv = def_argv;
		ret = 0;
	} else {
		ret = 0;
	}

	return ret;
}

static int copy_login(struct conf_option *opt, const struct conf_option *src)
{
	struct wlt_conf_t *conf = WLT_CONF_FROM_FIELD(opt->mem, login);
	struct wlt_conf_t *s = WLT_CONF_FROM_FIELD(src->mem, login);
	int ret;
	char **t;

	ret = shl_dup_array(&t, s->argv);
	if (ret)
		return ret;

	free(conf->argv);
	conf->argv = t;

	return 0;
}

static struct conf_grab def_grab_scroll_up =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Up);

static struct conf_grab def_grab_scroll_down =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Down);

static struct conf_grab def_grab_page_up =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Prior);

static struct conf_grab def_grab_page_down =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Next);

static struct conf_grab def_grab_fullscreen =
		CONF_SINGLE_GRAB(0, XKB_KEY_F11);

static struct conf_grab def_grab_zoom_in =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK, XKB_KEY_plus);

static struct conf_grab def_grab_zoom_out =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK, XKB_KEY_minus);

static struct conf_grab def_grab_copy =
		CONF_SINGLE_GRAB(SHL_LOGO_MASK, XKB_KEY_c);

static struct conf_grab def_grab_paste =
		CONF_SINGLE_GRAB(SHL_LOGO_MASK, XKB_KEY_v);

struct conf_option options[] = {
	CONF_OPTION_BOOL_FULL('h', "help", aftercheck_help, NULL, NULL, &wlt_conf.help, false),
	CONF_OPTION_BOOL('v', "verbose", &wlt_conf.verbose, false),
	CONF_OPTION_BOOL_FULL(0, "debug", aftercheck_debug, NULL, NULL, &wlt_conf.debug, false),
	CONF_OPTION_BOOL(0, "silent", &wlt_conf.silent, false),

	CONF_OPTION_BOOL_FULL('l', "login", aftercheck_login, copy_login, NULL, &wlt_conf.login, false),
	CONF_OPTION_STRING('t', "term", &wlt_conf.term, "xterm-256color"),
	CONF_OPTION_STRING(0, "palette", &wlt_conf.palette, NULL),
	CONF_OPTION_UINT(0, "sb-size", &wlt_conf.sb_size, 1000),

	CONF_OPTION_GRAB(0, "grab-scroll-up", &wlt_conf.grab_scroll_up, &def_grab_scroll_up),
	CONF_OPTION_GRAB(0, "grab-scroll-down", &wlt_conf.grab_scroll_down, &def_grab_scroll_down),
	CONF_OPTION_GRAB(0, "grab-page-up", &wlt_conf.grab_page_up, &def_grab_page_up),
	CONF_OPTION_GRAB(0, "grab-page-down", &wlt_conf.grab_page_down, &def_grab_page_down),
	CONF_OPTION_GRAB(0, "grab-fullscreen", &wlt_conf.grab_fullscreen, &def_grab_fullscreen),
	CONF_OPTION_GRAB(0, "grab-zoom-in", &wlt_conf.grab_zoom_in, &def_grab_zoom_in),
	CONF_OPTION_GRAB(0, "grab-zoom-out", &wlt_conf.grab_zoom_out, &def_grab_zoom_out),
	CONF_OPTION_GRAB(0, "grab-copy", &wlt_conf.grab_copy, &def_grab_copy),
	CONF_OPTION_GRAB(0, "grab-paste", &wlt_conf.grab_paste, &def_grab_paste),

	CONF_OPTION_STRING(0, "font-engine", &wlt_conf.font_engine, "pango"),
	CONF_OPTION_UINT(0, "font-size", &wlt_conf.font_size, 12),
	CONF_OPTION_STRING(0, "font-name", &wlt_conf.font_name, "monospace"),
	CONF_OPTION_UINT(0, "font-dpi", &wlt_conf.font_ppi, 96),

	CONF_OPTION_UINT(0, "xkb-repeat-delay", &wlt_conf.xkb_repeat_delay, 250),
	CONF_OPTION_UINT(0, "xkb-repeat-rate", &wlt_conf.xkb_repeat_rate, 50),
};

int main(int argc, char **argv)
{
	int ret;
	struct wlt_app app;
	struct conf_ctx *conf;

	ret = conf_ctx_new(&conf, options, sizeof(options) / sizeof(*options),
			   &wlt_conf);
	if (ret)
		goto err_out;

	ret = conf_ctx_parse_argv(conf, argc, argv);
	if (ret)
		goto err_conf;

	if (wlt_conf.exit) {
		conf_ctx_free(conf);
		return EXIT_SUCCESS;
	}

	if (!wlt_conf.debug && !wlt_conf.verbose && wlt_conf.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(wlt_conf.debug,
						wlt_conf.verbose));

	log_print_init("wlterm");

	kmscon_font_register(&kmscon_font_8x16_ops);
	kmscon_font_register(&kmscon_font_pango_ops);

	memset(&app, 0, sizeof(app));
	ret = setup_app(&app);
	if (ret)
		goto err_unload;

	ev_eloop_run(app.eloop, -1);

	destroy_app(&app);
	kmscon_font_unregister(kmscon_font_pango_ops.name);
	kmscon_font_unregister(kmscon_font_8x16_ops.name);
	conf_ctx_free(conf);
	log_info("exiting");

	return EXIT_SUCCESS;

err_unload:
	kmscon_font_unregister(kmscon_font_pango_ops.name);
	kmscon_font_unregister(kmscon_font_8x16_ops.name);
err_conf:
	conf_ctx_free(conf);
err_out:
	log_err("cannot initialize wlterm, errno %d: %s", ret, strerror(-ret));
	return -ret;
}
