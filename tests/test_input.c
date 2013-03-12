/*
 * test_input - Test the input system - hotplug and keypresses
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
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

static void print_help();

#include <errno.h>
#include <linux/input.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <xkbcommon/xkbcommon.h>
#include "eloop.h"
#include "shl_log.h"
#include "uterm_input.h"
#include "uterm_monitor.h"
#include "test_include.h"

static struct ev_eloop *eloop;
static struct uterm_input *input;

struct {
	char *xkb_model;
	char *xkb_layout;
	char *xkb_variant;
	char *xkb_options;
	char *xkb_keymap;
} input_conf;

/* Pressing Ctrl-\ should toggle the capturing. */
static void sig_quit(struct ev_eloop *p,
			struct signalfd_siginfo *info,
			void *data)
{
	if (!input)
		return;

	if (uterm_input_is_awake(input)) {
		uterm_input_sleep(input);
		log_info("Went to sleep\n");
	} else {
		uterm_input_wake_up(input);
		log_info("Woke Up\n");
	}
}

static void print_modifiers(unsigned int mods)
{
	if (mods & UTERM_SHIFT_MASK)
		printf("SHIFT ");
	if (mods & UTERM_LOCK_MASK)
		printf("LOCK ");
	if (mods & UTERM_CONTROL_MASK)
		printf("CONTROL ");
	if (mods & UTERM_ALT_MASK)
		printf("ALT ");
	if (mods & UTERM_LOGO_MASK)
		printf("LOGO ");
	printf("\n");
}

static void input_arrived(struct uterm_input *input,
			  struct uterm_input_event *ev,
			  void *data)
{
	char s[32];

	xkb_keysym_get_name(ev->keysyms[0], s, sizeof(s));
	printf("sym %s ", s);
	if (ev->codepoints[0] != UTERM_INPUT_INVALID) {
		/*
		 * Just a proof-of-concept hack. This works because glibc uses
		 * UTF-32 (= UCS-4) as the internal wchar_t encoding.
		 */
		printf("unicode %lc ", ev->codepoints[0]);
	}
	print_modifiers(ev->mods);
}

static void monitor_event(struct uterm_monitor *mon,
				struct uterm_monitor_event *ev,
				void *data)
{
	int ret;
	char *keymap;

	if (ev->type == UTERM_MONITOR_NEW_SEAT) {
		if (strcmp(ev->seat_name, "seat0"))
			return;

		keymap = NULL;
		if (input_conf.xkb_keymap && *input_conf.xkb_keymap) {
			ret = shl_read_file(input_conf.xkb_keymap, &keymap,
					    NULL);
			if (ret)
				log_error("cannot read keymap file %s: %d",
					  input_conf.xkb_keymap, ret);
		}

		ret = uterm_input_new(&input, eloop,
				      input_conf.xkb_model,
				      input_conf.xkb_layout,
				      input_conf.xkb_variant,
				      input_conf.xkb_options,
				      keymap,
				      0, 0);
		if (ret)
			return;
		ret = uterm_input_register_cb(input, input_arrived, NULL);
		if (ret)
			return;
		uterm_input_wake_up(input);
	} else if (ev->type == UTERM_MONITOR_FREE_SEAT) {
		uterm_input_unregister_cb(input, input_arrived, NULL);
		uterm_input_unref(input);
	} else if (ev->type == UTERM_MONITOR_NEW_DEV) {
		if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_add_dev(input, ev->dev_node);
	} else if (ev->type == UTERM_MONITOR_FREE_DEV) {
		if (ev->dev_type == UTERM_MONITOR_INPUT)
			uterm_input_remove_dev(input, ev->dev_node);
	}
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
		TEST_HELP
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-model <model>     [-]     Set XkbModel for input devices\n"
		"\t    --xkb-layout <layout>   [-]     Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant> [-]     Set XkbVariant for input devices\n"
		"\t    --xkb-options <options> [-]     Set XkbOptions for input devices\n"
		"\t    --xkb-keymap <FILE>     [-]     Use a predefined keymap for\n"
		"\t                                    input devices\n",
		"test_input");
	/*
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
}

struct conf_option options[] = {
	TEST_OPTIONS,
	CONF_OPTION_STRING(0, "xkb-model", &input_conf.xkb_model, ""),
	CONF_OPTION_STRING(0, "xkb-layout", &input_conf.xkb_layout, ""),
	CONF_OPTION_STRING(0, "xkb-variant", &input_conf.xkb_variant, ""),
	CONF_OPTION_STRING(0, "xkb-options", &input_conf.xkb_options, ""),
	CONF_OPTION_STRING(0, "xkb-keymap", &input_conf.xkb_keymap, ""),
};

int main(int argc, char **argv)
{
	int ret;
	struct uterm_monitor *mon;
	size_t onum;

	onum = sizeof(options) / sizeof(*options);
	ret = test_prepare(options, onum, argc, argv, &eloop);
	if (ret)
		goto err_fail;

	if (!setlocale(LC_ALL, "")) {
		log_err("Cannot set locale: %m");
		ret = -EFAULT;
		goto err_exit;
	}

	ret = uterm_monitor_new(&mon, eloop, monitor_event, NULL);
	if (ret)
		goto err_exit;

	ret = ev_eloop_register_signal_cb(eloop, SIGQUIT, sig_quit, NULL);
	if (ret)
		goto err_mon;

	system("stty -echo");
	uterm_monitor_scan(mon);
	ev_eloop_run(eloop, -1);
	system("stty echo");

	ev_eloop_unregister_signal_cb(eloop, SIGQUIT, sig_quit, NULL);
err_mon:
	uterm_monitor_unref(mon);
err_exit:
	test_exit(options, onum, eloop);
err_fail:
	if (ret != -ECANCELED)
		test_fail(ret);
	return abs(ret);
}
