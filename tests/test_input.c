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
#include "eloop.h"
#include "log.h"
#include "uterm.h"
#include "test_include.h"

extern void kbd_keysym_to_string(uint32_t keysym, char *str, size_t size);

static struct ev_eloop *eloop;
static struct uterm_input *input;

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
	if (mods & UTERM_MOD1_MASK)
		printf("MOD1 ");
	if (mods & UTERM_MOD2_MASK)
		printf("MOD2 ");
	if (mods & UTERM_MOD3_MASK)
		printf("MOD3 ");
	if (mods & UTERM_MOD4_MASK)
		printf("MOD4 ");
	if (mods & UTERM_MOD5_MASK)
		printf("MOD5 ");
	printf("\n");
}

static void input_arrived(struct uterm_input *input,
				struct uterm_input_event *ev,
				void *data)
{
	char s[16];

	if (ev->unicode == UTERM_INPUT_INVALID) {
		kbd_keysym_to_string(ev->keysym, s, sizeof(s));
		printf("sym %s ", s);
	} else {
		/*
		 * Just a proof-of-concept hack. This works because glibc uses
		 * UTF-32 (= UCS-4) as the internal wchar_t encoding.
		 */
		printf("unicode %lc ", ev->unicode);
	}
	print_modifiers(ev->mods);
}

static void monitor_event(struct uterm_monitor *mon,
				struct uterm_monitor_event *ev,
				void *data)
{
	int ret;

	if (ev->type == UTERM_MONITOR_NEW_SEAT) {
		if (strcmp(ev->seat_name, "seat0"))
			return;

		ret = uterm_input_new(&input, eloop);
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

int main(int argc, char **argv)
{
	int ret;
	struct uterm_monitor *mon;

	ret = test_prepare(argc, argv, &eloop);
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
	test_exit(eloop);
err_fail:
	test_fail(ret);
	return abs(ret);
}
