/*
 * Event Loop
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
#include <time.h>

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "eloop.h"
#include "log.h"

struct ev_eloop {
	int efd;
	unsigned long ref;
	struct ev_fd *fd;

	struct ev_idle *idle_list;
	struct ev_idle *cur_idle;

	struct epoll_event *cur_fds;
	size_t cur_fds_cnt;
};

struct ev_idle {
	unsigned long ref;
	struct ev_eloop *loop;
	struct ev_idle *next;
	struct ev_idle *prev;

	ev_idle_cb cb;
	void *data;
};

struct ev_fd {
	unsigned long ref;
	struct ev_eloop *loop;

	ev_fd_cb cb;
	void *data;
	int fd;
};

struct ev_signal {
	unsigned long ref;

	struct ev_fd *fd;
	ev_signal_cb cb;
	void *data;
};

struct ev_timer {
	unsigned long ref;

	struct ev_fd *fd;
	ev_timer_cb cb;
	void *data;
};

int ev_eloop_new_eloop(struct ev_eloop *loop, struct ev_eloop **out)
{
	struct ev_eloop *el;
	int ret;

	if (!out || !loop)
		return -EINVAL;

	ret = ev_eloop_new(&el);
	if (ret)
		return ret;

	ret = ev_eloop_add_eloop(loop, el);
	if (ret) {
		ev_eloop_unref(el);
		return ret;
	}

	ev_eloop_unref(el);
	*out = el;
	return 0;
}

static void eloop_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_eloop *eloop = data;

	if (mask & EV_READABLE)
		ev_eloop_dispatch(eloop, 0);
}

int ev_eloop_add_eloop(struct ev_eloop *loop, struct ev_eloop *add)
{
	int ret;

	if (!loop || !add)
		return -EINVAL;

	if (add->fd->loop)
		return -EALREADY;

	ret = ev_eloop_add_fd(loop, add->fd, add->efd, EV_READABLE,
							eloop_cb, add);
	if (ret)
		return ret;

	ev_eloop_ref(add);
	return 0;
}

void ev_eloop_rm_eloop(struct ev_eloop *rm)
{
	if (!rm || !rm->fd->loop)
		return;

	ev_eloop_rm_fd(rm->fd);
	ev_eloop_unref(rm);
}

int ev_idle_new(struct ev_idle **out)
{
	struct ev_idle *idle;

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

void ev_idle_ref(struct ev_idle *idle)
{
	if (!idle)
		return;

	++idle->ref;
}

void ev_idle_unref(struct ev_idle *idle)
{
	if (!idle || !idle->ref)
		return;

	if (--idle->ref)
		return;

	free(idle);
}

int ev_eloop_new_idle(struct ev_eloop *loop, struct ev_idle **out,
						ev_idle_cb cb, void *data)
{
	struct ev_idle *idle;
	int ret;

	if (!out)
		return -EINVAL;

	ret = ev_idle_new(&idle);
	if (ret)
		return ret;

	ret = ev_eloop_add_idle(loop, idle, cb, data);
	if (ret) {
		ev_idle_unref(idle);
		return ret;
	}

	ev_idle_unref(idle);
	*out = idle;
	return 0;
}

int ev_eloop_add_idle(struct ev_eloop *loop, struct ev_idle *idle,
						ev_idle_cb cb, void *data)
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

	ev_idle_ref(idle);
	ev_eloop_ref(loop);

	return 0;
}

void ev_eloop_rm_idle(struct ev_idle *idle)
{
	struct ev_eloop *loop;

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

	ev_idle_unref(idle);
	ev_eloop_unref(loop);
}

int ev_fd_new(struct ev_fd **out)
{
	struct ev_fd *fd;

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

void ev_fd_ref(struct ev_fd *fd)
{
	if (!fd)
		return;

	++fd->ref;
}

void ev_fd_unref(struct ev_fd *fd)
{
	if (!fd || !fd->ref)
		return;

	if (--fd->ref)
		return;

	free(fd);
}

int ev_eloop_new_fd(struct ev_eloop *loop, struct ev_fd **out,
				int rfd, int mask, ev_fd_cb cb, void *data)
{
	struct ev_fd *fd;
	int ret;

	if (!out)
		return -EINVAL;

	ret = ev_fd_new(&fd);
	if (ret)
		return ret;

	ret = ev_eloop_add_fd(loop, fd, rfd, mask, cb, data);
	if (ret) {
		ev_fd_unref(fd);
		return ret;
	}

	ev_fd_unref(fd);
	*out = fd;
	return 0;
}

int ev_eloop_add_fd(struct ev_eloop *loop, struct ev_fd *fd,
				int rfd, int mask, ev_fd_cb cb, void *data)
{
	struct epoll_event ep;

	if (!loop || !fd || !cb)
		return -EINVAL;

	if (fd->loop)
		return -EALREADY;

	memset(&ep, 0, sizeof(ep));
	if (mask & EV_READABLE)
		ep.events |= EPOLLIN;
	if (mask & EV_WRITEABLE)
		ep.events |= EPOLLOUT;
	ep.data.ptr = fd;

	if (epoll_ctl(loop->efd, EPOLL_CTL_ADD, rfd, &ep) < 0)
		return -errno;

	fd->loop = loop;
	fd->cb = cb;
	fd->data = data;
	fd->fd = rfd;

	ev_fd_ref(fd);
	ev_eloop_ref(loop);

	return 0;
}

void ev_eloop_rm_fd(struct ev_fd *fd)
{
	struct ev_eloop *loop;
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
	ev_fd_unref(fd);
	ev_eloop_unref(loop);
}

int ev_eloop_update_fd(struct ev_fd *fd, int mask)
{
	struct epoll_event ep;

	if (!fd || !fd->loop)
		return -EINVAL;

	memset(&ep, 0, sizeof(ep));
	if (mask & EV_READABLE)
		ep.events |= EPOLLIN;
	if (mask & EV_WRITEABLE)
		ep.events |= EPOLLOUT;
	ep.data.ptr = fd;

	if (epoll_ctl(fd->loop->efd,  EPOLL_CTL_MOD, fd->fd, &ep))
		return -errno;

	return 0;
}

int ev_signal_new(struct ev_signal **out)
{
	struct ev_signal *sig;
	int ret;

	if (!out)
		return -EINVAL;

	sig = malloc(sizeof(*sig));
	if (!sig)
		return -ENOMEM;

	memset(sig, 0, sizeof(*sig));
	sig->ref = 1;

	ret = ev_fd_new(&sig->fd);
	if (ret) {
		free(sig);
		return ret;
	}

	*out = sig;
	return 0;
}

void ev_signal_ref(struct ev_signal *sig)
{
	if (!sig)
		return;

	++sig->ref;
}

void ev_signal_unref(struct ev_signal *sig)
{
	if (!sig || !sig->ref)
		return;

	if (--sig->ref)
		return;

	ev_fd_unref(sig->fd);
	free(sig);
}

int ev_eloop_new_signal(struct ev_eloop *loop,
	struct ev_signal **out, int signum, ev_signal_cb cb,
								void *data)
{
	struct ev_signal *sig;
	int ret;

	if (!out)
		return -EINVAL;

	ret = ev_signal_new(&sig);
	if (ret)
		return ret;

	ret = ev_eloop_add_signal(loop, sig, signum, cb, data);
	if (ret) {
		ev_signal_unref(sig);
		return ret;
	}

	ev_signal_unref(sig);
	*out = sig;
	return 0;
}

static void signal_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_signal *sig = data;
	struct signalfd_siginfo signal_info;
	int len;

	if (mask & EV_READABLE) {
		len = read(fd->fd, &signal_info, sizeof(signal_info));
		if (len != sizeof(signal_info))
			log_warn("eloop: cannot read signalfd\n");
		else
			sig->cb(sig, signal_info.ssi_signo, sig->data);
	}
}

int ev_eloop_add_signal(struct ev_eloop *loop,
	struct ev_signal *sig, int signum, ev_signal_cb cb, void *data)
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

	ret = ev_eloop_add_fd(loop, sig->fd, fd, EV_READABLE,
							signal_cb, sig);
	if (ret) {
		close(fd);
		return ret;
	}

	sigprocmask(SIG_BLOCK, &mask, NULL);
	sig->cb = cb;
	sig->data = data;
	ev_signal_ref(sig);

	return 0;
}

void ev_eloop_rm_signal(struct ev_signal *sig)
{
	int fd;

	if (!sig || !sig->fd->loop)
		return;

	fd = sig->fd->fd;
	ev_eloop_rm_fd(sig->fd);
	close(fd);
	ev_signal_unref(sig);

	/*
	 * We cannot unblock the signal here because we do not know whether some
	 * other subsystem also added the signal to the sigprocmask.
	 */
}

int ev_timer_new(struct ev_timer **out)
{
	struct ev_timer *timer;
	int ret;

	if (!out)
		return -EINVAL;

	timer = malloc(sizeof(*timer));
	if (!timer)
		return -ENOMEM;

	memset(timer, 0, sizeof(*timer));
	timer->ref = 1;

	ret = ev_fd_new(&timer->fd);
	if (ret) {
		free(timer);
		return ret;
	}

	*out = timer;
	return 0;
}

void ev_timer_ref(struct ev_timer *timer)
{
	if (!timer)
		return;

	++timer->ref;
}

void ev_timer_unref(struct ev_timer *timer)
{
	if (!timer || !timer->ref)
		return;

	if (--timer->ref)
		return;

	ev_fd_unref(timer->fd);
	free(timer);
}

int ev_eloop_new_timer(struct ev_eloop *loop, struct ev_timer **out,
		const struct itimerspec *spec, ev_timer_cb cb, void *data)
{
	struct ev_timer *timer;
	int ret;

	if (!out)
		return -EINVAL;

	ret = ev_timer_new(&timer);
	if (ret)
		return ret;

	ret = ev_eloop_add_timer(loop, timer, spec, cb, data);
	if (ret) {
		ev_timer_unref(timer);
		return ret;
	}

	ev_timer_unref(timer);
	*out = timer;
	return 0;
}

static void timer_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_timer *timer = data;
	uint64_t expirations;
	int len;

	if (mask & EV_READABLE) {
		len = read(fd->fd, &expirations, sizeof(expirations));
		if (len != sizeof(expirations))
			log_warn("eloop: cannot read timerfd\n");
		else
			timer->cb(timer, expirations, timer->data);
	}
}

int ev_eloop_add_timer(struct ev_eloop *loop,
		struct ev_timer *timer, const struct itimerspec *spec,
						ev_timer_cb cb, void *data)
{
	int ret, fd;

	if (!loop || !timer || !spec || !cb)
		return -EINVAL;

	if (timer->fd->loop)
		return -EALREADY;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (fd < 0)
		return -errno;

	ret = timerfd_settime(fd, 0, spec, NULL);
	if (ret) {
		ret = -errno;
		log_warn("eloop: cannot set timerfd: %m\n");
		goto err_fd;
	}

	ret = ev_eloop_add_fd(loop, timer->fd, fd, EV_READABLE,
							timer_cb, timer);
	if (ret)
		goto err_fd;

	timer->cb = cb;
	timer->data = data;
	ev_timer_ref(timer);

	return 0;

err_fd:
	close(fd);
	return ret;
}

void ev_eloop_rm_timer(struct ev_timer *timer)
{
	int fd;

	if (!timer || !timer->fd->loop)
		return;

	fd = timer->fd->fd;
	ev_eloop_rm_fd(timer->fd);
	close(fd);
	ev_timer_unref(timer);
}

int ev_eloop_update_timer(struct ev_timer *timer,
						const struct itimerspec *spec)
{
	int ret;

	if (!timer || !timer->fd->loop)
		return -EINVAL;

	ret = timerfd_settime(timer->fd->fd, 0, spec, NULL);
	if (ret) {
		ret = -errno;
		log_warn("eloop: cannot set timerfd: %m\n");
		return ret;
	}

	return 0;
}

int ev_eloop_new(struct ev_eloop **out)
{
	struct ev_eloop *loop;
	int ret;

	if (!out)
		return -EINVAL;

	loop = malloc(sizeof(*loop));
	if (!loop)
		return -ENOMEM;

	memset(loop, 0, sizeof(*loop));
	loop->ref = 1;

	loop->efd = epoll_create1(EPOLL_CLOEXEC);
	if (loop->efd < 0) {
		ret = -errno;
		goto err_free;
	}

	ret = ev_fd_new(&loop->fd);
	if (ret)
		goto err_close;

	log_debug("eloop: create eloop object\n");
	*out = loop;
	return 0;

err_close:
	close(loop->efd);
err_free:
	free(loop);
	return ret;
}

void ev_eloop_ref(struct ev_eloop *loop)
{
	if (!loop)
		return;

	++loop->ref;
}

void ev_eloop_unref(struct ev_eloop *loop)
{
	if (!loop || !loop->ref)
		return;

	if (--loop->ref)
		return;

	log_debug("eloop: destroy eloop object\n");
	ev_fd_unref(loop->fd);
	close(loop->efd);
	free(loop);
}

int ev_eloop_dispatch(struct ev_eloop *loop, int timeout)
{
	struct epoll_event ep[32];
	struct ev_fd *fd;
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
			mask |= EV_READABLE;
		if (ep[i].events & EPOLLOUT)
			mask |= EV_WRITEABLE;
		if (ep[i].events & EPOLLHUP)
			mask |= EV_HUP;
		if (ep[i].events & EPOLLERR)
			mask |= EV_ERR;

		fd->cb(fd, mask, fd->data);
	}

	loop->cur_fds = NULL;
	loop->cur_fds_cnt = 0;

	return 0;
}
