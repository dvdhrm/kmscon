/*
 * kmscon - Configuration Parser
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "conf.h"
#include "kmscon_conf.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_video.h"

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
		"\t%1$s -l [options] -- /bin/login [login-arguments]\n"
		"\n"
		"You can prefix boolean options with \"no-\" to negate them. If an argument is\n"
		"given multiple times, only the last argument matters if not otherwise stated.\n"
		"\n"
		"General Options:\n"
		"\t-h, --help                  [off]   Print this help and exit\n"
		"\t-v, --verbose               [off]   Print verbose messages\n"
		"\t    --debug                 [off]   Enable debug mode\n"
		"\t    --silent                [off]   Suppress notices and warnings\n"
		"\t-c, --configdir </foo/bar>  [/etc/kmscon]\n"
		"\t                                    Path to config directory\n"
		"\t    --listen                [off]   Listen for new seats and spawn\n"
		"\t                                    sessions accordingly (daemon mode)\n"
		"\n"
		"Seat Options:\n"
		"\t    --vt <vt>               [auto]  Select which VT to run on\n"
		"\t    --switchvt              [on]    Automatically switch to VT\n"
		"\t    --seats <list,of,seats> [current] Select seats to run on\n"
		"\n"
		"Session Options:\n"
		"\t    --session-max <max>         [50]  Maximum number of sessions\n"
		"\t    --session-control           [off] Allow keyboard session-control\n"
		"\t    --terminal-session          [on]  Enable terminal session\n"
		"\t    --cdev-session              [off] Enable kernel VT emulation session\n"
		"\n"
		"Terminal Options:\n"
		"\t-l, --login                 [/bin/login -p]\n"
		"\t                              Start the given login process instead\n"
		"\t                              of the default process; all arguments\n"
		"\t                              following '--' will be be parsed as\n"
		"\t                              argv to this process. No more options\n"
		"\t                              after '--' will be parsed so use it at\n"
		"\t                              the end of the argument string\n"
		"\t-t, --term <TERM>           [xterm-256color]\n"
		"\t                              Value of the TERM environment variable\n"
		"\t                              for the child process\n"
		"\t    --reset-env             [on]\n"
		"\t                              Reset environment before running child\n"
		"\t                              process\n"
		"\t    --palette <name>        [default]\n"
		"\t                              Select the used color palette\n"
		"\t    --sb-size <num>         [1000]\n"
		"\t                              Size of the scrollback-buffer in lines\n"
		"\n"
		"Input Options:\n"
		"\t    --xkb-model <model>        [-]  Set XkbModel for input devices\n"
		"\t    --xkb-layout <layout>      [-]  Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant>    [-]  Set XkbVariant for input devices\n"
		"\t    --xkb-options <options>    [-]  Set XkbOptions for input devices\n"
		"\t    --xkb-keymap <FILE>        [-]  Use a predefined keymap for\n"
		"\t                                    input devices\n"
		"\t    --xkb-repeat-delay <msecs> [250]\n"
		"\t                                 Initial delay for key-repeat in ms\n"
		"\t    --xkb-repeat-rate <msecs>  [50]\n"
		"\t                                 Delay between two key-repeats in ms\n"
		"\n"
		"Grabs / Keyboard-Shortcuts:\n"
		"\t    --grab-scroll-up <grab>     [<Shift>Up]\n"
		"\t                                  Shortcut to scroll up\n"
		"\t    --grab-scroll-down <grab>   [<Shift>Down]\n"
		"\t                                  Shortcut to scroll down\n"
		"\t    --grab-page-up <grab>       [<Shift>Prior]\n"
		"\t                                  Shortcut to scroll page up\n"
		"\t    --grab-page-down <grab>     [<Shift>Next]\n"
		"\t                                  Shortcut to scroll page down\n"
		"\t    --grab-zoom-in <grab>       [<Ctrl>Plus]\n"
		"\t                                  Shortcut to increase font size\n"
		"\t    --grab-zoom-out <grab>      [<Ctrl>Minus]\n"
		"\t                                  Shortcut to decrease font size\n"
		"\t    --grab-session-next <grab>  [<Ctrl><Logo>Right]\n"
		"\t                                  Switch to next session\n"
		"\t    --grab-session-prev <grab>  [<Ctrl><Logo>Left]\n"
		"\t                                  Switch to previous session\n"
		"\t    --grab-session-dummy <grab> [<Ctrl><Logo>Escape]\n"
		"\t                                  Switch to dummy session\n"
		"\t    --grab-session-close <grab> [<Ctrl><Logo>BackSpace]\n"
		"\t                                  Close current session\n"
		"\t    --grab-terminal-new <grab>  [<Ctrl><Logo>Return]\n"
		"\t                                  Create new terminal session\n"
		"\n"
		"Video Options:\n"
		"\t    --drm                   [on]    Use DRM if available\n"
		"\t    --hwaccel               [off]   Use 3D hardware-acceleration if\n"
		"\t                                    available\n"
		"\t    --gpus={all,aux,primary}[all]   GPU selection mode\n"
		"\t    --render-engine <eng>   [-]     Console renderer\n"
		"\t    --render-timing         [off]   Print renderer timing information\n"
		"\n"
		"Font Options:\n"
		"\t    --font-engine <engine>  [pango]\n"
		"\t                              Font engine\n"
		"\t    --font-size <points>    [12]\n"
		"\t                              Font size in points\n"
		"\t    --font-name <name>      [monospace]\n"
		"\t                              Font name\n"
		"\t    --font-dpi <dpi>        [96]\n"
		"\t                              Force DPI value for all fonts\n",
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

#define KMSCON_CONF_FROM_FIELD(_mem, _name) \
	shl_offsetof((_mem), struct kmscon_conf_t, _name)

/*
 * VT Type
 * The --vt option is special in that it can be an integer, a string or a path.
 * We use the string-handling of conf_string but the parser is different. See
 * below for more information on the parser.
 */

static void conf_default_vt(struct conf_option *opt)
{
	conf_string.set_default(opt);
}

static void conf_free_vt(struct conf_option *opt)
{
	conf_string.free(opt);
}

static int conf_parse_vt(struct conf_option *opt, bool on, const char *arg)
{
	static const char prefix[] = "/dev/";
	unsigned int val;
	char *str;
	int ret;

	if (!shl_strtou(arg, &val)) {
		ret = asprintf(&str, "%stty%u", prefix, val);
		if (ret == -1)
			return -ENOMEM;
	} else if (*arg && *arg != '.' && *arg != '/') {
		str = malloc(sizeof(prefix) + strlen(arg));
		if (!str)
			return -ENOMEM;

		strcpy(str, prefix);
		strcat(str, arg);
	} else {
		str = strdup(arg);
		if (!str)
			return -ENOMEM;
	}

	opt->type->free(opt);
	*(void**)opt->mem = str;
	return 0;
}

static int conf_copy_vt(struct conf_option *opt,
			const struct conf_option *src)
{
	return conf_string.copy(opt, src);
}

static const struct conf_type conf_vt = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_vt,
	.free = conf_free_vt,
	.parse = conf_parse_vt,
	.copy = conf_copy_vt,
};

/*
 * Login handling
 * The --login option is special in that it can have an unlimited number of
 * arguments on the command-line. So on the command-line it is an boolean option
 * that specifies whether default login or custom login is used.
 * However, the file-parser does simple string-parsing as it does not need the
 * special handling that the command-line does.
 */

static char *def_argv[] = { "/bin/login", "-p", NULL };

static void conf_default_login(struct conf_option *opt)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, login);

	opt->type->free(opt);
	conf->login = false;
	conf->argv = def_argv;
}

static void conf_free_login(struct conf_option *opt)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, login);

	if (conf->argv != def_argv)
		free(conf->argv);
	conf->argv = NULL;
	conf->login = false;
}

static int conf_parse_login(struct conf_option *opt, bool on, const char *arg)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, login);

	opt->type->free(opt);
	conf->login = on;
	return 0;
}

static int conf_copy_login(struct conf_option *opt,
			   const struct conf_option *src)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, login);
	struct kmscon_conf_t *s = KMSCON_CONF_FROM_FIELD(src->mem, login);
	int ret;
	char **t;

	if (s->argv) {
		ret = shl_dup_array(&t, s->argv);
		if (ret)
			return ret;
	} else {
		t = NULL;
	}

	opt->type->free(opt);
	conf->argv = t;
	conf->login = s->login;
	return 0;
}

static const struct conf_type conf_login = {
	.flags = 0,
	.set_default = conf_default_login,
	.free = conf_free_login,
	.parse = conf_parse_login,
	.copy = conf_copy_login,
};

static int aftercheck_login(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, login);
	int ret = 0;
	char **t;

	/* parse "--login [...] -- args" arguments */
	if (argv && conf->login) {
		if (idx >= argc) {
			fprintf(stderr, "Arguments for --login missing\n");
			return -EFAULT;
		}

		ret = shl_dup_array_size(&t, &argv[idx], argc - idx);
		if (ret)
			return ret;

		conf->argv = t;
		ret = argc - idx;
	} else if (!conf->argv) {
		ret = shl_dup_array_size(&t, def_argv,
					 sizeof(def_argv) / sizeof(*def_argv));
		if (ret)
			return ret;

		conf->argv = t;
	}

	return ret;
}

static int file_login(struct conf_option *opt, bool on, const char *arg)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, login);
	char **t;
	unsigned int size;
	int ret;

	if (!arg) {
		log_error("no arguments for 'login' config-option");
		return -EFAULT;
	}

	ret = shl_split_string(arg, &t, &size, ' ', false);
	if (ret) {
		log_error("cannot split 'login' config-option argument");
		return ret;
	}

	if (size < 1) {
		log_error("empty argument given for 'login' config-option");
		return -EFAULT;
	}

	opt->type->free(opt);
	conf->login = on;
	conf->argv = t;
	return 0;
}

/*
 * GPU selection type
 * The GPU selection mode is a simple string to enum parser.
 */

static void conf_default_gpus(struct conf_option *opt)
{
	conf_uint.set_default(opt);
}

static void conf_free_gpus(struct conf_option *opt)
{
	conf_uint.free(opt);
}

static int conf_parse_gpus(struct conf_option *opt, bool on, const char *arg)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, gpus);
	unsigned int mode;

	if (!strcmp(arg, "all")) {
		mode = KMSCON_GPU_ALL;
	} else if (!strcmp(arg, "aux") || !strcmp(arg, "auxiliary")) {
		mode = KMSCON_GPU_AUX;
	} else if (!strcmp(arg, "primary") || !strcmp(arg, "single")) {
		mode = KMSCON_GPU_PRIMARY;
	} else {
		log_error("invalid GPU selection mode --gpus='%s'", arg);
		return -EFAULT;
	}

	opt->type->free(opt);
	conf->gpus = mode;
	return 0;
}

static int conf_copy_gpus(struct conf_option *opt,
			  const struct conf_option *src)
{
	return conf_uint.copy(opt, src);
}

static const struct conf_type conf_gpus = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_gpus,
	.free = conf_free_gpus,
	.parse = conf_parse_gpus,
	.copy = conf_copy_gpus,
};

/*
 * Custom Afterchecks
 * Several other options have side-effects on other options. We use afterchecks
 * to enforce these. They're pretty simple. See below.
 * Some of them also need copy-helpers because they copy more than one value.
 */

static int aftercheck_debug(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, debug);

	/* --debug implies --verbose */
	if (conf->debug)
		conf->verbose = 1;

	return 0;
}

static int aftercheck_help(struct conf_option *opt, int argc, char **argv,
			   int idx)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, help);

	/* exit after printing --help information */
	if (conf->help) {
		print_help();
		conf->exit = true;
	}

	return 0;
}

static int aftercheck_drm(struct conf_option *opt, int argc, char **argv,
			  int idx)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, drm);

	/* disable --drm if DRM runtime support is not available */
	/* TODO: This prevents people from booting without DRM and loading DRM
	 * drivers during runtime. However, if we remove it, we will be unable
	 * to automatically fall back to fbdev-mode.
	 * But with blacklists fbdev-mode is the default so we can run with DRM
	 * enabled but will still correctly use fbdev devices so we can then
	 * remove this check. */
	if (conf->drm) {
		if (!uterm_video_available(UTERM_VIDEO_DRM3D) &&
		    !uterm_video_available(UTERM_VIDEO_DRM2D)) {
			log_info("no DRM runtime support available; disabling --drm");
			conf->drm = false;
		}
	}

	return 0;
}

static int aftercheck_vt(struct conf_option *opt, int argc, char **argv,
			 int idx)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, vt);

	if (!conf->vt || conf->seat_config)
		return 0;

	if (!kmscon_conf_is_single_seat(conf)) {
		log_error("you cannot use global --vt if --seats contains not exactly one seat");
		return -EFAULT;
	}

	return 0;
}

static int aftercheck_listen(struct conf_option *opt, int argc, char **argv,
			     int idx)
{
	struct kmscon_conf_t *conf = KMSCON_CONF_FROM_FIELD(opt->mem, listen);
	int ret = -EFAULT;

	if (conf->listen)
		return 0;

	if (conf->cdev_session)
		log_error("you can use --cdev-session only in combination with --listen");
	else
		ret = 0;

	return ret;
}

/*
 * Default Values
 * We use static default values to avoid allocating memory for these. This
 * speeds up config-parser considerably.
 */

static char *def_seats[] = { "current", NULL };

static struct conf_grab def_grab_scroll_up =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Up);

static struct conf_grab def_grab_scroll_down =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Down);

static struct conf_grab def_grab_page_up =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Prior);

static struct conf_grab def_grab_page_down =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Next);

static struct conf_grab def_grab_zoom_in =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK, XKB_KEY_plus);

static struct conf_grab def_grab_zoom_out =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK, XKB_KEY_minus);

static struct conf_grab def_grab_session_next =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_LOGO_MASK, XKB_KEY_Right);

static struct conf_grab def_grab_session_prev =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_LOGO_MASK, XKB_KEY_Left);

static struct conf_grab def_grab_session_dummy =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_LOGO_MASK, XKB_KEY_Escape);

static struct conf_grab def_grab_session_close =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_LOGO_MASK, XKB_KEY_BackSpace);

static struct conf_grab def_grab_terminal_new =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_LOGO_MASK, XKB_KEY_Return);

int kmscon_conf_new(struct conf_ctx **out)
{
	struct conf_ctx *ctx;
	int ret;
	struct kmscon_conf_t *conf;

	if (!out)
		return -EINVAL;

	conf = malloc(sizeof(*conf));
	if (!conf)
		return -ENOMEM;
	memset(conf, 0, sizeof(*conf));

	struct conf_option options[] = {
		/* Global Options */
		CONF_OPTION_BOOL_FULL('h', "help", aftercheck_help, NULL, NULL, &conf->help, false),
		CONF_OPTION_BOOL('v', "verbose", &conf->verbose, false),
		CONF_OPTION_BOOL_FULL(0, "debug", aftercheck_debug, NULL, NULL, &conf->debug, false),
		CONF_OPTION_BOOL(0, "silent", &conf->silent, false),
		CONF_OPTION_STRING('c', "configdir", &conf->configdir, "/etc/kmscon"),
		CONF_OPTION_BOOL_FULL(0, "listen", aftercheck_listen, NULL, NULL, &conf->listen, false),

		/* Seat Options */
		CONF_OPTION(0, 0, "vt", &conf_vt, aftercheck_vt, NULL, NULL, &conf->vt, NULL),
		CONF_OPTION_BOOL(0, "switchvt", &conf->switchvt, true),
		CONF_OPTION_STRING_LIST(0, "seats", &conf->seats, def_seats),

		/* Session Options */
		CONF_OPTION_UINT(0, "session-max", &conf->session_max, 50),
		CONF_OPTION_BOOL(0, "session-control", &conf->session_control, false),
		CONF_OPTION_BOOL(0, "terminal-session", &conf->terminal_session, true),
		CONF_OPTION_BOOL(0, "cdev-session", &conf->cdev_session, false),

		/* Terminal Options */
		CONF_OPTION(0, 'l', "login", &conf_login, aftercheck_login, NULL, file_login, &conf->login, false),
		CONF_OPTION_STRING('t', "term", &conf->term, "xterm-256color"),
		CONF_OPTION_BOOL(0, "reset-env", &conf->reset_env, true),
		CONF_OPTION_STRING(0, "palette", &conf->palette, NULL),
		CONF_OPTION_UINT(0, "sb-size", &conf->sb_size, 1000),

		/* Input Options */
		CONF_OPTION_STRING(0, "xkb-model", &conf->xkb_model, ""),
		CONF_OPTION_STRING(0, "xkb-layout", &conf->xkb_layout, ""),
		CONF_OPTION_STRING(0, "xkb-variant", &conf->xkb_variant, ""),
		CONF_OPTION_STRING(0, "xkb-options", &conf->xkb_options, ""),
		CONF_OPTION_STRING(0, "xkb-keymap", &conf->xkb_keymap, ""),
		CONF_OPTION_UINT(0, "xkb-repeat-delay", &conf->xkb_repeat_delay, 250),
		CONF_OPTION_UINT(0, "xkb-repeat-rate", &conf->xkb_repeat_rate, 50),

		/* Grabs / Keyboard-Shortcuts */
		CONF_OPTION_GRAB(0, "grab-scroll-up", &conf->grab_scroll_up, &def_grab_scroll_up),
		CONF_OPTION_GRAB(0, "grab-scroll-down", &conf->grab_scroll_down, &def_grab_scroll_down),
		CONF_OPTION_GRAB(0, "grab-page-up", &conf->grab_page_up, &def_grab_page_up),
		CONF_OPTION_GRAB(0, "grab-page-down", &conf->grab_page_down, &def_grab_page_down),
		CONF_OPTION_GRAB(0, "grab-zoom-in", &conf->grab_zoom_in, &def_grab_zoom_in),
		CONF_OPTION_GRAB(0, "grab-zoom-out", &conf->grab_zoom_out, &def_grab_zoom_out),
		CONF_OPTION_GRAB(0, "grab-session-next", &conf->grab_session_next, &def_grab_session_next),
		CONF_OPTION_GRAB(0, "grab-session-prev", &conf->grab_session_prev, &def_grab_session_prev),
		CONF_OPTION_GRAB(0, "grab-session-dummy", &conf->grab_session_dummy, &def_grab_session_dummy),
		CONF_OPTION_GRAB(0, "grab-session-close", &conf->grab_session_close, &def_grab_session_close),
		CONF_OPTION_GRAB(0, "grab-terminal-new", &conf->grab_terminal_new, &def_grab_terminal_new),

		/* Video Options */
		CONF_OPTION_BOOL_FULL(0, "drm", aftercheck_drm, NULL, NULL, &conf->drm, true),
		CONF_OPTION_BOOL(0, "hwaccel", &conf->hwaccel, false),
		CONF_OPTION(0, 0, "gpus", &conf_gpus, NULL, NULL, NULL, &conf->gpus, KMSCON_GPU_ALL),
		CONF_OPTION_STRING(0, "render-engine", &conf->render_engine, NULL),
		CONF_OPTION_BOOL(0, "render-timing", &conf->render_timing, false),

		/* Font Options */
		CONF_OPTION_STRING(0, "font-engine", &conf->font_engine, "pango"),
		CONF_OPTION_UINT(0, "font-size", &conf->font_size, 12),
		CONF_OPTION_STRING(0, "font-name", &conf->font_name, "monospace"),
		CONF_OPTION_UINT(0, "font-dpi", &conf->font_ppi, 96),
	};

	ret = conf_ctx_new(&ctx, options, sizeof(options) / sizeof(*options),
			   conf);
	if (ret) {
		free(conf);
		return ret;
	}

	*out = ctx;
	return 0;
}

void kmscon_conf_free(struct conf_ctx *ctx)
{
	void *conf;
	if (!ctx)
		return;

	conf = conf_ctx_get_mem(ctx);
	conf_ctx_free(ctx);
	free(conf);
}

int kmscon_conf_load_main(struct conf_ctx *ctx, int argc, char **argv)
{
	int ret;
	struct kmscon_conf_t *conf;

	if (!ctx)
		return -EINVAL;

	conf = conf_ctx_get_mem(ctx);
	conf->seat_config = false;

	ret = conf_ctx_parse_argv(ctx, argc, argv);
	if (ret)
		return ret;

	if (conf->exit)
		return 0;

	if (!conf->debug && !conf->verbose && conf->silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(conf->debug,
						conf->verbose));

	log_print_init("kmscon");

	ret = conf_ctx_parse_file(ctx, "%s/kmscon.conf", conf->configdir);
	if (ret)
		return ret;

	return 0;
}

int kmscon_conf_load_seat(struct conf_ctx *ctx, const struct conf_ctx *main,
			  const char *seat)
{
	int ret;
	struct kmscon_conf_t *conf;

	if (!ctx || !main || !seat)
		return -EINVAL;

	log_debug("parsing seat configuration for seat %s", seat);

	conf = conf_ctx_get_mem(ctx);
	conf->seat_config = true;

	ret = conf_ctx_parse_ctx(ctx, main);
	if (ret)
		return ret;

	ret = conf_ctx_parse_file(ctx, "%s/%s.seat.conf", conf->configdir,
				  seat);
	if (ret)
		return ret;

	return 0;
}
