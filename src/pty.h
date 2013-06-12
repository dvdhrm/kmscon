/*
 * kmscon - Pseudo Terminal Handling
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
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
 * The pty object provides an interface for communicating with a child process
 * over a pseudo terminal. The child is the host, we act as the TTY terminal,
 * and the kernel is the driver.
 *
 * To use this, create a new pty object and open it. You will start receiving
 * output notifications through the output_cb callback. To communicate with
 * the other end of the terminal, use the kmscon_pty_input method. All
 * communication is done using byte streams (presumably UTF-8).
 *
 * The pty can be closed voluntarily using the kmson_pty_close method. The
 * child process can also exit at will; this will be communicated through the
 * input callback. The pty object does not wait on the child processes it
 * spawns; this is the responsibility of the object's user.
 */

#ifndef KMSCON_PTY_H
#define KMSCON_PTY_H

#include <stdbool.h>
#include <stdlib.h>

struct kmscon_pty;

typedef void (*kmscon_pty_input_cb)
	(struct kmscon_pty *pty, const char *u8, size_t len, void *data);

int kmscon_pty_new(struct kmscon_pty **out, kmscon_pty_input_cb input_cb,
		   void *data);
void kmscon_pty_ref(struct kmscon_pty *pty);
void kmscon_pty_unref(struct kmscon_pty *pty);
int kmscon_pty_set_term(struct kmscon_pty *pty, const char *term);
int kmscon_pty_set_colorterm(struct kmscon_pty *pty, const char *colorterm);
int kmscon_pty_set_argv(struct kmscon_pty *pty, char **argv);
int kmscon_pty_set_seat(struct kmscon_pty *pty, const char *seat);
int kmscon_pty_set_vtnr(struct kmscon_pty *pty, unsigned int vtnr);
void kmscon_pty_set_env_reset(struct kmscon_pty *pty, bool do_reset);

int kmscon_pty_get_fd(struct kmscon_pty *pty);
void kmscon_pty_dispatch(struct kmscon_pty *pty);

int kmscon_pty_open(struct kmscon_pty *pty, unsigned short width,
						unsigned short height);
void kmscon_pty_close(struct kmscon_pty *pty);

int kmscon_pty_write(struct kmscon_pty *pty, const char *u8, size_t len);
void kmscon_pty_signal(struct kmscon_pty *pty, int signum);
void kmscon_pty_resize(struct kmscon_pty *pty,
			unsigned short width, unsigned short height);

#endif /* KMSCON_PTY_H */
