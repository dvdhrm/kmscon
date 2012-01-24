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

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "eloop.h"
#include "log.h"

struct kmscon_eloop {
	int efd;
	unsigned long ref;

	struct kmscon_idle *idle_list;
	struct kmscon_idle *cur_idle;

	struct epoll_event *cur_fds;
	size_t cur_fds_cnt;
};

struct kmscon_idle {
	unsigned long ref;
	struct kmscon_eloop *loop;
	struct kmscon_idle *next;
	struct kmscon_idle *prev;

	kmscon_idle_cb cb;
	void *data;
};

struct kmscon_fd {
	unsigned long ref;
	struct kmscon_eloop *loop;

	kmscon_fd_cb cb;
	void *data;
	int fd;
};

struct kmscon_signal {
	unsigned long ref;

	struct kmscon_fd *fd;
	kmscon_signal_cb cb;
	void *data;
};

int kmscon_idle_new(struct kmscon_idle **out)
{
	struct kmscon_idle *idle;

	if (!out)
		return -EINVAL;

	idle = malloc(sizeof(*idle));
	if (!idle)
		return -ENOMEM;

	memset(idle, 0, sizeof(*idle));
	idle->ref = 1;

	*out = idle;
	return 0;
}

void kmscon_idle_ref(struct kmscon_idle *idle)
{
	if (!idle)
		return;

	++idle->ref;
}

void kmscon_idle_unref(struct kmscon_idle *idle)
{
	if (!idle || !idle->ref)
		return;

	if (--idle->ref)
		return;

	free(idle);
}

int kmscon_eloop_new_idle(struct kmscon_eloop *loop, struct kmscon_idle **out,
						kmscon_idle_cb cb, void *data)
{
	struct kmscon_idle *idle;
	int ret;

	if (!out)
		return -EINVAL;

	ret = kmscon_idle_new(&idle);
	if (ret)
		return ret;

	ret = kmscon_eloop_add_idle(loop, idle, cb, data);
	if (ret) {
		kmscon_idle_unref(idle);
		return ret;
	}

	kmscon_idle_unref(idle);
	*out = idle;
	return 0;
}

int kmscon_eloop_add_idle(struct kmscon_eloop *loop, struct kmscon_idle *idle,
						kmscon_idle_cb cb, void *data)
{
	if (!loop || !idle || !cb)
		return -EINVAL;

	if (idle->next || idle->prev || idle->loop)
		return -EALREADY;

	idle->next = loop->idle_list;
	if (idle->next)
		idle->next->prev = idle;
	loop->idle_list = idle;

	idle->loop = loop;
	idle->cb = cb;
	idle->data = data;

	kmscon_idle_ref(idle);
	kmscon_eloop_ref(loop);

	return 0;
}

void kmscon_eloop_rm_idle(struct kmscon_idle *idle)
{
	struct kmscon_eloop *loop;

	if (!idle || !idle->loop)
		return;

	loop = idle->loop;

	/*
	 * If the loop is currently dispatching, we need to check whether we are
	 * the current element and correctly set it to the next element.
	 */
	if (loop->cur_idle == idle)
		loop->cur_idle = idle->next;

	if (idle->prev)
		idle->prev->next = idle->next;
	if (idle->next)
		idle->next->prev = idle->prev;
	if (loop->idle_list == idle)
		loop->idle_list = idle->next;

	idle->next = NULL;
	idle->prev = NULL;
	idle->loop = NULL;
	idle->cb = NULL;
	idle->data = NULL;

	kmscon_idle_unref(idle);
	kmscon_eloop_unref(loop);
}

int kmscon_fd_new(struct kmscon_fd **out)
{
	struct kmscon_fd *fd;

	if (!out)
		return -EINVAL;

	fd = malloc(sizeof(*fd));
	if (!fd)
		return -ENOMEM;

	memset(fd, 0, sizeof(*fd));
	fd->ref = 1;
	fd->fd = -1;

	*out = fd;
	return 0;
}

void kmscon_fd_ref(struct kmscon_fd *fd)
{
	if (!fd)
		return;

	++fd->ref;
}

void kmscon_fd_unref(struct kmscon_fd *fd)
{
	if (!fd || !fd->ref)
		return;

	if (--fd->ref)
		return;

	free(fd);
}

int kmscon_eloop_new_fd(struct kmscon_eloop *loop, struct kmscon_fd **out,
				int rfd, int mask, kmscon_fd_cb cb, void *data)
{
	struct kmscon_fd *fd;
	int ret;

	if (!out)
		return -EINVAL;

	ret = kmscon_fd_new(&fd);
	if (ret)
		return ret;

	ret = kmscon_eloop_add_fd(loop, fd, rfd, mask, cb, data);
	if (ret) {
		kmscon_fd_unref(fd);
		return ret;
	}

	kmscon_fd_unref(fd);
	*out = fd;
	return 0;
}

int kmscon_eloop_add_fd(struct kmscon_eloop *loop, struct kmscon_fd *fd,
				int rfd, int mask, kmscon_fd_cb cb, void *data)
{
	struct epoll_event ep;

	if (!loop || !fd || !cb)
		return -EINVAL;

	if (fd->loop)
		return -EALREADY;

	memset(&ep, 0, sizeof(ep));
	if (mask & KMSCON_READABLE)
		ep.events |= EPOLLIN;
	if (mask & KMSCON_WRITEABLE)
		ep.events |= EPOLLOUT;
	ep.data.ptr = fd;

	if (epoll_ctl(loop->efd, EPOLL_CTL_ADD, rfd, &ep) < 0)
		return -errno;

	fd->loop = loop;
	fd->cb = cb;
	fd->data = data;
	fd->fd = rfd;

	kmscon_fd_ref(fd);
	kmscon_eloop_ref(loop);

	return 0;
}

void kmscon_eloop_rm_fd(struct kmscon_fd *fd)
{
	struct kmscon_eloop *loop;
	size_t i;

	if (!fd || !fd->loop)
		return;

	loop = fd->loop;

	epoll_ctl(loop->efd, EPOLL_CTL_DEL, fd->fd, NULL);

	/*
	 * If we are currently dispatching events, we need to remove ourself
	 * from the temporary event list.
	 */
	for (i = 0; i < loop->cur_fds_cnt; ++i) {
		if (fd == loop->cur_fds[i].data.ptr)
			loop->cur_fds[i].data.ptr = NULL;
	}

	fd->loop = NULL;
	fd->cb = NULL;
	fd->data = NULL;
	fd->fd = -1;
	kmscon_fd_unref(fd);
	kmscon_eloop_unref(loop);
}

int kmscon_eloop_update_fd(struct kmscon_fd *fd, int mask)
{
	struct epoll_event ep;

	if (!fd || !fd->loop)
		return -EINVAL;

	memset(&ep, 0, sizeof(ep));
	if (mask & KMSCON_READABLE)
		ep.events |= EPOLLIN;
	if (mask & KMSCON_WRITEABLE)
		ep.events |= EPOLLOUT;
	ep.data.ptr = fd;

	if (epoll_ctl(fd->loop->efd,  EPOLL_CTL_MOD, fd->fd, &ep))
		return -errno;

	return 0;
}

int kmscon_signal_new(struct kmscon_signal **out)
{
	struct kmscon_signal *sig;
	int ret;

	if (!out)
		return -EINVAL;

	sig = malloc(sizeof(*sig));
	if (!sig)
		return -ENOMEM;

	memset(sig, 0, sizeof(*sig));
	sig->ref = 1;

	ret = kmscon_fd_new(&sig->fd);
	if (ret) {
		free(sig);
		return ret;
	}

	*out = sig;
	return 0;
}

void kmscon_signal_ref(struct kmscon_signal *sig)
{
	if (!sig)
		return;

	++sig->ref;
}

void kmscon_signal_unref(struct kmscon_signal *sig)
{
	if (!sig || !sig->ref)
		return;

	if (--sig->ref)
		return;

	kmscon_fd_unref(sig->fd);
	free(sig);
}

int kmscon_eloop_new_signal(struct kmscon_eloop *loop,
	struct kmscon_signal **out, int signum, kmscon_signal_cb cb,
								void *data)
{
	struct kmscon_signal *sig;
	int ret;

	if (!out)
		return -EINVAL;

	ret = kmscon_signal_new(&sig);
	if (ret)
		return ret;

	ret = kmscon_eloop_add_signal(loop, sig, signum, cb, data);
	if (ret) {
		kmscon_signal_unref(sig);
		return ret;
	}

	kmscon_signal_unref(sig);
	*out = sig;
	return 0;
}

static void signal_cb(struct kmscon_fd *fd, int mask, void *data)
{
	struct kmscon_signal *sig = data;
	struct signalfd_siginfo signal_info;
	int len;

	if (mask & KMSCON_READABLE) {
		len = read(fd->fd, &signal_info, sizeof(signal_info));
		if (len != sizeof(signal_info))
			log_warn("eloop: cannot read signalfd\n");
		else
			sig->cb(sig, signal_info.ssi_signo, sig->data);
	}
}

int kmscon_eloop_add_signal(struct kmscon_eloop *loop,
	struct kmscon_signal *sig, int signum, kmscon_signal_cb cb, void *data)
{
	sigset_t mask;
	int ret, fd;

	if (!loop || !sig)
		return -EINVAL;

	if (sig->fd->loop)
		return -EALREADY;

	sigemptyset(&mask);
	sigaddset(&mask, signum);

	fd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (fd < 0)
		return -errno;

	ret = kmscon_eloop_add_fd(loop, sig->fd, fd, KMSCON_READABLE,
							signal_cb, sig);
	if (ret) {
		close(fd);
		return ret;
	}

	sigprocmask(SIG_BLOCK, &mask, NULL);
	sig->cb = cb;
	sig->data = data;
	kmscon_signal_ref(sig);

	return 0;
}

void kmscon_eloop_rm_signal(struct kmscon_signal *sig)
{
	int fd;

	if (!sig || !sig->fd->loop)
		return;

	fd = sig->fd->fd;
	kmscon_eloop_rm_fd(sig->fd);
	close(fd);
	kmscon_signal_unref(sig);

	/*
	 * We cannot unblock the signal here because we do not know whether some
	 * other subsystem also added the signal to the sigprocmask.
	 */
}

int kmscon_eloop_new(struct kmscon_eloop **out)
{
	struct kmscon_eloop *loop;

	if (!out)
		return -EINVAL;

	loop = malloc(sizeof(*loop));
	if (!loop)
		return -ENOMEM;

	memset(loop, 0, sizeof(*loop));
	loop->ref = 1;

	loop->efd = epoll_create1(EPOLL_CLOEXEC);
	if (loop->efd < 0) {
		free(loop);
		return -errno;
	}

	log_debug("eloop: create eloop object\n");
	*out = loop;
	return 0;
}

void kmscon_eloop_ref(struct kmscon_eloop *loop)
{
	if (!loop)
		return;

	++loop->ref;
}

void kmscon_eloop_unref(struct kmscon_eloop *loop)
{
	if (!loop || !loop->ref)
		return;

	if (--loop->ref)
		return;

	log_debug("eloop: destroy eloop object\n");
	close(loop->efd);
	free(loop);
}

int kmscon_eloop_dispatch(struct kmscon_eloop *loop, int timeout)
{
	struct epoll_event ep[32];
	struct kmscon_fd *fd;
	int i, count, mask;

	if (!loop)
		return -EINVAL;

	/* dispatch idle events */
	loop->cur_idle = loop->idle_list;
	while (loop->cur_idle) {
		loop->cur_idle->cb(loop->cur_idle, loop->cur_idle->data);
		if (loop->cur_idle)
			loop->cur_idle = loop->cur_idle->next;
	}

	/* dispatch fd events */
	count = epoll_wait(loop->efd, ep, 32, timeout);
	if (count < 0) {
		log_warn("eloop: epoll_wait dispatching failed: %m\n");
		return -errno;
	}

	loop->cur_fds = ep;
	loop->cur_fds_cnt = count;

	for (i = 0; i < count; ++i) {
		fd = ep[i].data.ptr;
		if (!fd || !fd->cb)
			continue;

		mask = 0;
		if (ep[i].events & EPOLLIN)
			mask |= KMSCON_READABLE;
		if (ep[i].events & EPOLLOUT)
			mask |= KMSCON_WRITEABLE;
		if (ep[i].events & EPOLLHUP)
			mask |= KMSCON_HUP;
		if (ep[i].events & EPOLLERR)
			mask |= KMSCON_ERR;

		fd->cb(fd, mask, fd->data);
	}

	loop->cur_fds = NULL;
	loop->cur_fds_cnt = 0;

	return 0;
}

int kmscon_eloop_get_fd(struct kmscon_eloop *loop)
{
	if (!loop)
		return -EINVAL;

	return loop->efd;
}
