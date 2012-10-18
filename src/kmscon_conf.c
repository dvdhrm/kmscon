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
#include <xkbcommon/xkbcommon-keysyms.h>
#include "conf.h"
#include "kmscon_conf.h"
#include "log.h"
#include "shl_misc.h"

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
		"\t    --vt <vt-number>        [auto]  Select which VT to run on on seat0\n"
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
		"\t-t, --term <TERM>           [xterm-256color]\n"
		"\t                              Value of the TERM environment variable\n"
		"\t                              for the child process\n"
		"\t    --palette <name>        [default]\n"
		"\t                              Select the used color palette\n"
		"\t    --sb-size <num>         [1000]\n"
		"\t                              Size of the scrollback-buffer in lines\n"
		"\n"
		"Video Options:\n"
		"\t    --fbdev                 [off]   Use fbdev instead of DRM\n"
		"\t    --dumb                  [off]   Use dumb DRM instead of hardware-\n"
		"\t                                    accelerated DRM devices\n"
		"\t    --fps                   [50]    Limit frame-rate\n"
		"\t    --render-engine <eng>   [-]     Console renderer\n"
		"\t    --render-timing         [off]   Print renderer timing information\n"
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-layout <layout>      [us] Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant>    [-]  Set XkbVariant for input devices\n"
		"\t    --xkb-options <options>    [-]  Set XkbOptions for input devices\n"
		"\t    --xkb-repeat-delay <msecs> [250]\n"
		"\t                                 Initial delay for key-repeat in ms\n"
		"\t    --xkb-repeat-rate <msecs>  [50]\n"
		"\t                                 Delay between two key-repeats in ms\n"
		"\n"
		"\t    --grab-scroll-up <grab>     [<Shift>Up]\n"
		"\t                                  Shortcut to scroll up\n"
		"\t    --grab-scroll-down <grab>   [<Shift>Down]\n"
		"\t                                  Shortcut to scroll down\n"
		"\t    --grab-page-up <grab>       [<Shift>Prior]\n"
		"\t                                  Shortcut to scroll page up\n"
		"\t    --grab-page-down <grab>     [<Shift>Next]\n"
		"\t                                  Shortcut to scroll page down\n"
		"\t    --grab-session-next <grab>  [<Ctrl><Alt>Right]\n"
		"\t                                  Switch to next session\n"
		"\t    --grab-session-prev <grab>  [<Ctrl><Alt>Left]\n"
		"\t                                  Switch to previous session\n"
		"\t    --grab-session-close <grab> [<Ctrl><Alt>W]\n"
		"\t                                  Close current session\n"
		"\t    --grab-terminal-new <grab>  [<Ctrl><Alt>Return]\n"
		"\t                                  Create new terminal session\n"
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

static void conf_default_vt(struct conf_option *opt)
{
	*(void**)opt->mem = opt->def;
}

static const struct conf_type conf_vt = {
	.flags = CONF_HAS_ARG,
	.parse = conf_parse_vt,
	.free = conf_free_value,
	.set_default = conf_default_vt,
};

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

static struct conf_grab def_grab_scroll_up =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Up);

static struct conf_grab def_grab_scroll_down =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Down);

static struct conf_grab def_grab_page_up =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Prior);

static struct conf_grab def_grab_page_down =
		CONF_SINGLE_GRAB(SHL_SHIFT_MASK, XKB_KEY_Next);

static struct conf_grab def_grab_session_next =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_ALT_MASK, XKB_KEY_Right);

static struct conf_grab def_grab_session_prev =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_ALT_MASK, XKB_KEY_Left);

static struct conf_grab def_grab_session_close =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_ALT_MASK, XKB_KEY_w);

static struct conf_grab def_grab_terminal_new =
		CONF_SINGLE_GRAB(SHL_CONTROL_MASK | SHL_ALT_MASK, XKB_KEY_Return);

struct conf_option options[] = {
	CONF_OPTION_BOOL('h', "help", aftercheck_help, &kmscon_conf.help, false),
	CONF_OPTION_BOOL('v', "verbose", NULL, &kmscon_conf.verbose, false),
	CONF_OPTION_BOOL(0, "debug", aftercheck_debug, &kmscon_conf.debug, false),
	CONF_OPTION_BOOL(0, "silent", NULL, &kmscon_conf.silent, false),
	CONF_OPTION_BOOL(0, "fbdev", NULL, &kmscon_conf.use_fbdev, false),
	CONF_OPTION_BOOL(0, "dumb", NULL, &kmscon_conf.dumb, false),
	CONF_OPTION_UINT(0, "fps", NULL, &kmscon_conf.fps, 50),
	CONF_OPTION_STRING(0, "render-engine", NULL, &kmscon_conf.render_engine, NULL),
	CONF_OPTION_BOOL(0, "render-timing", NULL, &kmscon_conf.render_timing, false),
	CONF_OPTION(0, 0, "vt", &conf_vt, NULL, &kmscon_conf.vt, NULL),
	CONF_OPTION_BOOL('s', "switchvt", NULL, &kmscon_conf.switchvt, false),
	CONF_OPTION_BOOL('l', "login", aftercheck_login, &kmscon_conf.login, false),
	CONF_OPTION_STRING('t', "term", NULL, &kmscon_conf.term, "xterm-256color"),
	CONF_OPTION_STRING(0, "palette", NULL, &kmscon_conf.palette, NULL),
	CONF_OPTION_UINT(0, "sb-size", NULL, &kmscon_conf.sb_size, 1000),
	CONF_OPTION_GRAB(0, "grab-scroll-up", NULL, &kmscon_conf.grab_scroll_up, &def_grab_scroll_up),
	CONF_OPTION_GRAB(0, "grab-scroll-down", NULL, &kmscon_conf.grab_scroll_down, &def_grab_scroll_down),
	CONF_OPTION_GRAB(0, "grab-page-up", NULL, &kmscon_conf.grab_page_up, &def_grab_page_up),
	CONF_OPTION_GRAB(0, "grab-page-down", NULL, &kmscon_conf.grab_page_down, &def_grab_page_down),
	CONF_OPTION_GRAB(0, "grab-session-next", NULL, &kmscon_conf.grab_session_next, &def_grab_session_next),
	CONF_OPTION_GRAB(0, "grab-session-prev", NULL, &kmscon_conf.grab_session_prev, &def_grab_session_prev),
	CONF_OPTION_GRAB(0, "grab-session-close", NULL, &kmscon_conf.grab_session_close, &def_grab_session_close),
	CONF_OPTION_GRAB(0, "grab-terminal-new", NULL, &kmscon_conf.grab_terminal_new, &def_grab_terminal_new),
	CONF_OPTION_STRING(0, "xkb-layout", NULL, &kmscon_conf.xkb_layout, "us"),
	CONF_OPTION_STRING(0, "xkb-variant", NULL, &kmscon_conf.xkb_variant, ""),
	CONF_OPTION_STRING(0, "xkb-options", NULL, &kmscon_conf.xkb_options, ""),
	CONF_OPTION_UINT(0, "xkb-repeat-delay", NULL, &kmscon_conf.xkb_repeat_delay, 250),
	CONF_OPTION_UINT(0, "xkb-repeat-rate", NULL, &kmscon_conf.xkb_repeat_rate, 50),
	CONF_OPTION_STRING(0, "font-engine", NULL, &kmscon_conf.font_engine, "pango"),
	CONF_OPTION_UINT(0, "font-size", NULL, &kmscon_conf.font_size, 12),
	CONF_OPTION_STRING(0, "font-name", NULL, &kmscon_conf.font_name, "monospace"),
	CONF_OPTION_UINT(0, "font-dpi", NULL, &kmscon_conf.font_ppi, 96),
	CONF_OPTION_STRING_LIST(0, "seats", aftercheck_seats, &kmscon_conf.seats, def_seats),
};

int kmscon_load_config(int argc, char **argv)
{
	size_t onum;
	int ret;

	onum = sizeof(options) / sizeof(*options);
	ret = conf_parse_argv(options, onum, argc, argv);
	if (ret)
		return ret;

	if (kmscon_conf.exit)
		return 0;

	if (!kmscon_conf.debug && !kmscon_conf.verbose && kmscon_conf.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(kmscon_conf.debug,
						kmscon_conf.verbose));

	log_print_init("kmscon");

	ret = conf_parse_all_files(options, onum);
	if (ret)
		return ret;

	return 0;
}

void kmscon_free_config(void)
{
	conf_free(options, sizeof(options) / sizeof(*options));
}
