/*
 * test_input - Test the input system - hotplug and keypresses
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
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
#include "input.h"
#include "kbd.h"
#include "log.h"
#include "test_include.h"

/* Pressing Ctrl-\ should toggle the capturing. */
static void sig_quit(struct ev_eloop *p, struct signalfd_siginfo *info,
			void *data)
{
	struct kmscon_input *input = data;

	if (kmscon_input_is_asleep(input)) {
		kmscon_input_wake_up(input);
		log_info("Woke Up\n");
	} else {
		kmscon_input_sleep(input);
		log_info("Went to sleep\n");
	}
}

static void print_modifiers(unsigned int mods)
{
	if (mods & KMSCON_SHIFT_MASK)
		printf("SHIFT ");
	if (mods & KMSCON_LOCK_MASK)
		printf("LOCK ");
	if (mods & KMSCON_CONTROL_MASK)
		printf("CONTROL ");
	if (mods & KMSCON_MOD1_MASK)
		printf("MOD1 ");
	if (mods & KMSCON_MOD2_MASK)
		printf("MOD2 ");
	if (mods & KMSCON_MOD3_MASK)
		printf("MOD3 ");
	if (mods & KMSCON_MOD4_MASK)
		printf("MOD4 ");
	if (mods & KMSCON_MOD5_MASK)
		printf("MOD5 ");
	printf("\n");
}

static void input_arrived(struct kmscon_input *input,
				struct kmscon_input_event *ev, void *data)
{
	char s[16];

	if (ev->unicode == KMSCON_INPUT_INVALID) {
		kmscon_kbd_keysym_to_string(ev->keysym, s, sizeof(s));
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

int main(int argc, char **argv)
{
	int ret;
	struct ev_eloop *eloop;
	struct kmscon_input *input;

	ret = test_prepare(argc, argv, &eloop);
	if (ret)
		goto err_fail;

	if (!setlocale(LC_ALL, "")) {
		log_err("Cannot set locale: %m");
		ret = -EFAULT;
		goto err_exit;
	}

	ret = kmscon_input_new(&input);
	if (ret)
		goto err_exit;

	ret = ev_eloop_register_signal_cb(eloop, SIGQUIT, sig_quit, input);
	if (ret)
		goto err_input;

	ret = kmscon_input_connect_eloop(input, eloop);
	if (ret)
		goto err_sigquit;

	ret = kmscon_input_register_cb(input, input_arrived, NULL);
	if (ret)
		goto err_sigquit;

	kmscon_input_wake_up(input);

	system("stty -echo");
	ev_eloop_run(eloop, -1);
	system("stty echo");

	kmscon_input_unregister_cb(input, input_arrived, NULL);
err_sigquit:
	ev_eloop_unregister_signal_cb(eloop, SIGQUIT, sig_quit, input);
err_input:
	kmscon_input_unref(input);
err_exit:
	test_exit(eloop);
err_fail:
	test_fail(ret);
	return abs(ret);
}
