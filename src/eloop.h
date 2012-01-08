/*
 * kmscon - Event Loop
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * Event Loop
 * This provides a basic event loop similar to those provided by glib etc.
 * It uses linux specific features like signalfd so it may not be easy to port
 * it to other platforms.
 */

#ifndef KMSCON_ELOOP_H
#define KMSCON_ELOOP_H

#include <stdlib.h>

struct kmscon_eloop;
struct kmscon_idle;
struct kmscon_fd;
struct kmscon_signal;

typedef void (*kmscon_idle_cb) (struct kmscon_idle *idle, void *data);
typedef void (*kmscon_fd_cb) (struct kmscon_fd *fd, int mask, void *data);
typedef void (*kmscon_signal_cb)
			(struct kmscon_signal *sig, int signum, void *data);

enum kmscon_eloop_flags {
	KMSCON_READABLE = 0x01,
	KMSCON_WRITEABLE = 0x02,
	KMSCON_HUP = 0x04,
	KMSCON_ERR = 0x08,
};

int kmscon_eloop_new(struct kmscon_eloop **out);
void kmscon_eloop_ref(struct kmscon_eloop *loop);
void kmscon_eloop_unref(struct kmscon_eloop *loop);

int kmscon_eloop_get_fd(struct kmscon_eloop *loop);
int kmscon_eloop_dispatch(struct kmscon_eloop *loop, int timeout);

/* idle sources */

int kmscon_idle_new(struct kmscon_idle **out);
void kmscon_idle_ref(struct kmscon_idle *idle);
void kmscon_idle_unref(struct kmscon_idle *idle);

int kmscon_eloop_new_idle(struct kmscon_eloop *loop, struct kmscon_idle **out,
						kmscon_idle_cb cb, void *data);
int kmscon_eloop_add_idle(struct kmscon_eloop *loop, struct kmscon_idle *idle,
						kmscon_idle_cb cb, void *data);
void kmscon_eloop_rm_idle(struct kmscon_idle *idle);

/* fd sources */

int kmscon_fd_new(struct kmscon_fd **out);
void kmscon_fd_ref(struct kmscon_fd *fd);
void kmscon_fd_unref(struct kmscon_fd *fd);

int kmscon_eloop_new_fd(struct kmscon_eloop *loop, struct kmscon_fd **out,
				int rfd, int mask, kmscon_fd_cb cb, void *data);
int kmscon_eloop_add_fd(struct kmscon_eloop *loop, struct kmscon_fd *fd,
				int rfd, int mask, kmscon_fd_cb cb, void *data);
void kmscon_eloop_rm_fd(struct kmscon_fd *fd);
int kmscon_eloop_update_fd(struct kmscon_fd *fd, int mask);

/* signal sources */

int kmscon_signal_new(struct kmscon_signal **out);
void kmscon_signal_ref(struct kmscon_signal *sig);
void kmscon_signal_unref(struct kmscon_signal *sig);

int kmscon_eloop_new_signal(struct kmscon_eloop *loop,
	struct kmscon_signal **out, int signum, kmscon_signal_cb cb,
								void *data);
int kmscon_eloop_add_signal(struct kmscon_eloop *loop,
	struct kmscon_signal *sig, int signum, kmscon_signal_cb cb, void *data);
void kmscon_eloop_rm_signal(struct kmscon_signal *sig);

#endif /* KMSCON_ELOOP_H */
