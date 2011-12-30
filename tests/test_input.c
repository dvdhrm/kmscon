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
#include <unistd.h>
#include <wchar.h>

#include <X11/extensions/XKBcommon.h>
#include <X11/keysym.h>

#include "eloop.h"
#include "input.h"
#include "log.h"

static bool terminate;

static void sig_term(struct kmscon_signal *sig, int signum, void *data)
{
	terminate = true;
}

/* Pressing Ctrl-\ should toggle the capturing. */
static void sig_quit(struct kmscon_signal *sig, int signum, void *data)
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

static void print_modifiers(uint8_t mods)
{
	if (mods & XKB_COMMON_SHIFT_MASK)
		printf("SHIFT ");
	if (mods & XKB_COMMON_LOCK_MASK)
		printf("LOCK ");
	if (mods & XKB_COMMON_CONTROL_MASK)
		printf("CONTROL ");
	if (mods & XKB_COMMON_MOD1_MASK)
		printf("MOD1 ");
	if (mods & XKB_COMMON_MOD2_MASK)
		printf("MOD2 ");
	if (mods & XKB_COMMON_MOD3_MASK)
		printf("MOD3 ");
	if (mods & XKB_COMMON_MOD4_MASK)
		printf("MOD4 ");
	if (mods & XKB_COMMON_MOD5_MASK)
		printf("MOD5 ");
	printf("\n");
}

static void input_arrived(struct kmscon_input *input,
				struct kmscon_input_event *ev, void *data)
{
	int len;
	char s[16];
	char utf8[MB_CUR_MAX + 1];

	if (ev->unicode == KMSCON_INPUT_INVALID) {
		xkb_keysym_to_string(ev->keysym, s, sizeof(s));
		printf("sym %s ", s);
	} else {
		/*
		 * Just a proof-of-concept hack. This works because glibc uses
		 * UTF-32 (= UCS-4) as the internal wchar_t encoding.
		 */
		len = wctomb(utf8, (wchar_t)ev->unicode);
		if (len <= 0) {
			log_info("Bad unicode char\n");
			return;
		} else {
			utf8[len] = '\0';
		}

		printf("utf8 %s ", utf8);
	}
	print_modifiers(ev->modifiers);
}

int main(int argc, char **argv)
{
	int ret;
	struct kmscon_eloop *loop;
	struct kmscon_input *input;
	struct kmscon_signal *sigint, *sigquit;

	if (!setlocale(LC_ALL, "en_US.UTF-8")) {
		log_err("Cannot set locale: %m\n");
		ret = -EFAULT;
		goto err_out;
	}

	ret = kmscon_eloop_new(&loop);
	if (ret) {
		log_err("Cannot create eloop\n");
		goto err_out;
	}

	ret = kmscon_input_new(&input);
	if (ret) {
		log_err("Cannot create input\n");
		goto err_loop;
	}

	ret = kmscon_eloop_new_signal(loop, &sigint, SIGINT, sig_term, NULL);
	if (ret) {
		log_err("Cannot add INT signal\n");
		goto err_input;
	}

	ret = kmscon_eloop_new_signal(loop, &sigquit, SIGQUIT, sig_quit, input);
	if (ret) {
		log_err("Cannot add quit signal\n");
		goto err_sigint;
	}

	ret = kmscon_input_connect_eloop(input, loop, input_arrived, NULL);
	if (ret) {
		log_err("Cannot connect input\n");
		goto err_sigquit;
	}

	kmscon_input_wake_up(input);

	system("stty -echo");

	while (!terminate) {
		ret = kmscon_eloop_dispatch(loop, -1);
		if (ret) {
			log_err("Dispatcher failed\n");
			break;
		}
	}

	system("stty echo");

err_sigquit:
	kmscon_eloop_rm_signal(sigquit);
err_sigint:
	kmscon_eloop_rm_signal(sigint);
err_input:
	kmscon_input_unref(input);
err_loop:
	kmscon_eloop_unref(loop);
err_out:
	return abs(ret);
}
