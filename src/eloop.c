/*
 * Event Loop
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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

/**
 * SECTION:eloop
 * @short_description: Event loop
 * @include: eloop.h
 *
 * The event loop allows to register event sources and poll them for events.
 * When an event occurs, the user-supplied callback is called.
 *
 * The event-loop allows the callbacks to modify _any_ data they want. They can
 * remove themselves or other sources from the event loop even in a callback.
 * This, however, means that recursive dispatch calls are not supported to
 * increase performance and avoid internal dispatch-stacks.
 *
 * Sources can be one of:
 *  - File descriptors: An fd that is watched for readable/writeable events
 *  - Timers: An event that occurs after a relative timeout
 *  - Counters: An event that occurs when the counter is non-zero
 *  - Signals: An event that occurs when a signal is caught
 *  - Idle: An event that occurs when nothing else is done
 *  - Eloop: An event loop itself can be a source of another event loop
 *
 * A source can be registered for a single event-loop only! You cannot add it
 * to multiple event loops simultaneously. Also all provided sources are based
 * on the file-descriptor source so it is guaranteed that you can get a
 * file-descriptor for every source-type. This is not exported via the public
 * API, but you can get the epoll-fd which is basically a selectable FD summary
 * of all event sources.
 *
 * For instance, if you're developing a library, you can use the eloop library
 * internally and you will have a full event-loop implementation inside of a
 * library without any side-effects. You simply export the epoll-fd of the
 * eloop-object via your public API and the outside users think you only use a
 * single file-descriptor. They include this FD in their own application event
 * loop which will then dispatch the messages to your library. Internally, you
 * simply forward this dispatching to ev_eloop_dispatch() which then calls all
 * your internal callbacks.
 * That is, you have an event loop inside your library without requiring the
 * outside-user to use the same event loop. You also have no global state or
 * thread-bound event-loops like the Qt/Gtk event loops. So you have full
 * access to the whole event loop without any side-effects.
 *
 *
 * The whole eloop library does not use any global data. Therefore, it is fully
 * re-entrant and no synchronization needed. However, a single object is not
 * thread-safe. This means, if you access a single eloop object or registered
 * sources on this eloop object in two different threads, you need to
 * synchronize them. Furthermore, all callbacks are called from the thread that
 * calls ev_eloop_dispatch() or ev_eloop_run().
 * This guarantees that you have full control over the eloop but that you also
 * have to implement additional functionality like thread-affinity yourself
 * (obviously, only if you need it).
 *
 * The philosophy behind this library is that a proper application needs only a
 * single thread that uses an event loop. Multiple threads should be used to do
 * calculations, but not to avoid learning how to do non-blocking I/O!
 * Therefore, only the application threads needs an event-loop, all other
 * threads only perform calculation and return the data to the main thread.
 * However, the library does not enforce this design-choice. On the contrary,
 * it supports all other types of application-designs, too. But as it is
 * optimized for performance, other application-designs may need to add further
 * functionality (like thread-affinity) by themselves as it would slow down the
 * event loop if it was natively implemented.
 *
 *
 * To get started simply create an eloop object with ev_eloop_new(). All
 * functions return 0 on success and a negative error code like -EFAULT on
 * failure. -EINVAL is returned if invalid parameters were passed.
 * Every object can be ref-counted. *_ref() increases the reference-count and
 * *_unref() decreases it. *_unref() also destroys the object if the ref-count
 * drops to 0.
 * To create new objects you call *_new(). It stores a pointer to the new
 * object in the location you passed as parameter. Nearly all structues are
 * opaque, that is, you cannot access member fields directly. This guarantees
 * ABI stability.
 *
 * You can create sources with ev_fd_new(), ev_timer_new(), ... and you can add
 * them to you eloop with ev_eloop_add_fd() or ev_eloop_add_timer(), ...
 * After they are added you can call ev_eloop_run() to run this eloop for the
 * given time. If you pass -1 as timeout, it runs until some callback calls
 * ev_eloop_exit() on this eloop.
 * You can perform _any_ operations on an eloop object inside of callbacks. You
 * can add new sources, remove sources, destroy sources, modify sources. You
 * also do all this on the currently active source.
 *
 * All objects are enabled by default. You can disable them with *_disable()
 * and re-enable them with *_enable(). Only when enabled, they are added to the
 * dispatcher and callbacks are called.
 *
 * Two sources are different for performance reasons:
 *   Idle sources: Idle sources can be registered with
 *   ev_eloop_register_idle_cb() and unregistered with
 *   ev_eloop_unregister_idle_cb(). They internally share a single
 *   file-descriptor to make them faster so you cannot get the same access as
 *   to other event sources (you cannot enable/disable them or similar).
 *   Idle sources are called every-time ev_eloop_dispatch() is called. That is,
 *   as long as an idle-source is registered, the event-loop will not go to
 *   sleep!
 *
 *   Signal sources: Talking about the API they are very similar to
 *   idle-sources. They same restrictions apply, however, their type is very
 *   different. A signal-callback is called when the specified signal is
 *   received. They are not called in signal-context! But rather called in the
 *   same context as every other source. They are implemented with
 *   linux-signalfd.
 *   You can register multiple callbacks for the same signal and all callbacks
 *   will be called (compared to plain signalfd where only one fd gets the
 *   signal). This is done internally by sharing the signalfd.
 *   However, there is one restriction: You cannot share a signalfd between
 *   multiple eloop-instances. That is, if you register a callback for the same
 *   signal on two different eloop-instances (which are connected themselves),
 *   then only one eloop-instance will fire the signal source. This is a
 *   restriction of signalfd that cannot be overcome. However, it is very
 *   uncommon to register multiple callbacks for a signal so this shouldn't
 *   affect common application use-cases.
 *   Also note that if you register a callback for SIGCHLD then the eloop-
 *   object will automatically reap all pending zombies _after_ your callback
 *   has been called. So if you need to check for them, then check for all of
 *   them in the callback. After you return, they will be gone.
 *   When adding a signal handler the signal is automatically added to the
 *   currently blocked signals. It is not removed when dropping the
 *   signal-source, though.
 *
 * Eloop uses several system calls which may fail. All errors (including memory
 * allocation errors via -ENOMEM) are forwarded to the caller, however, it is
 * often preferable to have a more detailed logging message. Therefore, eloop
 * takes a loggin-function as argument for each object. Pass NULL if you are
 * not interested in logging. This will disable logging entirely.
 * Otherwise, pass in a callback from your application. This callback will be
 * called when a message is to be logged. The function may be called under any
 * circumstances (out-of-memory, etc...) and should always behave well.
 * Nothing is ever logged except through this callback.
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_llog.h"
#include "shl_misc.h"

#define LLOG_SUBSYSTEM "eloop"

/**
 * ev_eloop:
 * @ref: refcnt of this object
 * @llog: llog log function
 * @llog_data: llog log function user-data
 * @efd: The epoll file descriptor.
 * @fd: Event source around \efd so you can nest event loops
 * @cnt: Counter source used for idle events
 * @sig_list: Shared signal sources
 * @idlers: List of idle sources
 * @cur_fds: Current dispatch array of fds
 * @cur_fds_cnt: current length of \cur_fds
 * @cur_fds_size: absolute size of \cur_fds
 * @exit: true if we should exit the main loop
 *
 * An event loop is an object where you can register event sources. If you then
 * sleep on the event loop, you will be woken up if a single event source is
 * firing up. An event loop itself is an event source so you can nest them.
 */
struct ev_eloop {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;
	int efd;
	struct ev_fd *fd;
	int idle_fd;

	struct shl_dlist sig_list;
	struct shl_hook *chlds;
	struct shl_hook *idlers;
	struct shl_hook *pres;
	struct shl_hook *posts;

	bool dispatching;
	struct epoll_event *cur_fds;
	size_t cur_fds_cnt;
	size_t cur_fds_size;
	bool exit;
};

/**
 * ev_fd:
 * @ref: refcnt for object
 * @llog: llog log function
 * @llog_data: llog log function user-data
 * @fd: the actual file descriptor
 * @mask: the event mask for this fd (EV_READABLE, EV_WRITEABLE, ...)
 * @cb: the user callback
 * @data: the user data
 * @enabled: true if the object is currently enabled
 * @loop: NULL or pointer to eloop if bound
 *
 * File descriptors are the most basic event source. Internally, they are used
 * to implement all other kinds of event sources.
 */
struct ev_fd {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;
	int fd;
	int mask;
	ev_fd_cb cb;
	void *data;

	bool enabled;
	struct ev_eloop *loop;
};

/**
 * ev_timer:
 * @ref: refcnt of this object
 * @llog: llog log function
 * @llog_data: llog log function user-data
 * @cb: user callback
 * @data: user data
 * @fd: the timerfd file descriptor
 * @efd: fd-source for @fd
 *
 * Based on timerfd this allows firing events based on relative timeouts.
 */
struct ev_timer {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;
	ev_timer_cb cb;
	void *data;

	int fd;
	struct ev_fd *efd;
};

/**
 * ev_counter:
 * @ref: refcnt of counter object
 * @llog: llog log function
 * @llog_data: llog log function user-data
 * @cb: user callback
 * @data: user data
 * @fd: eventfd file descriptor
 * @efd: fd-source for @fd
 *
 * Counter sources fire if they are non-zero. They are based on the eventfd
 * syscall in linux.
 */
struct ev_counter {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;
	ev_counter_cb cb;
	void *data;

	int fd;
	struct ev_fd *efd;
};

/**
 * ev_signal_shared:
 * @list: list integration into ev_eloop object
 * @fd: the signalfd file descriptor for this signal
 * @signum: the actual signal number
 * @hook: list of registered user callbacks for this signal
 *
 * A shared signal allows multiple listeners for the same signal. All listeners
 * are called if the signal is caught.
 */
struct ev_signal_shared {
	struct shl_dlist list;

	struct ev_fd *fd;
	int signum;
	struct shl_hook *hook;
};

/*
 * Shared signals
 * signalfd allows us to conveniently listen for incoming signals. However, if
 * multiple signalfds are registered for the same signal, then only one of them
 * will get signaled. To avoid this restriction, we provide shared signals.
 * That means, the user can register for a signal and if no other user is
 * registered for this signal, yet, we create a new shared signal. Otherwise,
 * we add the user to the existing shared signals.
 * If the signal is caught, we simply call all users that are registered for
 * this signal.
 * To avoid side-effects, we automatically block all signals for the current
 * thread when a signalfd is created. We never unblock the signal. However,
 * most modern linux user-space programs avoid signal handlers, anyway, so you
 * can use signalfd only.
 */

static void sig_child(struct ev_eloop *loop, struct signalfd_siginfo *info,
		      void *data)
{
	pid_t pid;
	int status;
	struct ev_child_data d;

	while (1) {
		pid = waitpid(-1, &status, WNOHANG);
		if (pid == -1) {
			if (errno != ECHILD)
				llog_warn(loop, "cannot wait on child: %m");
			break;
		} else if (pid == 0) {
			break;
		} else if (WIFEXITED(status)) {
			if (WEXITSTATUS(status) != 0)
				llog_debug(loop, "child %d exited with status %d",
					   pid, WEXITSTATUS(status));
			else
				llog_debug(loop, "child %d exited successfully",
					   pid);
		} else if (WIFSIGNALED(status)) {
			llog_debug(loop, "child %d exited by signal %d", pid,
				   WTERMSIG(status));
		}

		d.pid = pid;
		d.status = status;
		shl_hook_call(loop->chlds, loop, &d);
	}
}

static void shared_signal_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_signal_shared *sig = data;
	struct signalfd_siginfo info;
	int len;

	if (mask & EV_READABLE) {
		len = read(fd->fd, &info, sizeof(info));
		if (len != sizeof(info))
			llog_warn(fd, "cannot read signalfd (%d): %m", errno);
		else
			shl_hook_call(sig->hook, sig->fd->loop, &info);
	} else if (mask & (EV_HUP | EV_ERR)) {
		llog_warn(fd, "HUP/ERR on signal source");
	}
}

/**
 * signal_new:
 * @out: Shared signal storage where the new object is stored
 * @loop: The event loop where this shared signal is registered
 * @signum: Signal number that this shared signal is for
 *
 * This creates a new shared signal and links it into the list of shared
 * signals in @loop. It automatically adds @signum to the signal mask of the
 * current thread so the signal is blocked.
 *
 * Returns: 0 on success, otherwise negative error code
 */
static int signal_new(struct ev_signal_shared **out, struct ev_eloop *loop,
			int signum)
{
	sigset_t mask;
	int ret, fd;
	struct ev_signal_shared *sig;

	if (signum < 0)
		return llog_EINVAL(loop);

	sig = malloc(sizeof(*sig));
	if (!sig)
		return llog_ENOMEM(loop);
	memset(sig, 0, sizeof(*sig));
	sig->signum = signum;

	ret = shl_hook_new(&sig->hook);
	if (ret)
		goto err_free;

	sigemptyset(&mask);
	sigaddset(&mask, signum);

	fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
	if (fd < 0) {
		ret = -errno;
		llog_error(loop, "cannot created signalfd");
		goto err_hook;
	}

	ret = ev_eloop_new_fd(loop, &sig->fd, fd, EV_READABLE,
				shared_signal_cb, sig);
	if (ret)
		goto err_sig;

	pthread_sigmask(SIG_BLOCK, &mask, NULL);
	shl_dlist_link(&loop->sig_list, &sig->list);

	*out = sig;
	return 0;

err_sig:
	close(fd);
err_hook:
	shl_hook_free(sig->hook);
err_free:
	free(sig);
	return ret;
}

/**
 * signal_free:
 * @sig: The shared signal to be freed
 *
 * This unlinks the given shared signal from the event-loop where it was
 * registered and destroys it. This does _not_ unblock the signal number that it
 * was associated to. If you want this, you need to do this manually with
 * pthread_sigmask().
 */
static void signal_free(struct ev_signal_shared *sig)
{
	int fd;

	if (!sig)
		return;

	shl_dlist_unlink(&sig->list);
	fd = sig->fd->fd;
	ev_eloop_rm_fd(sig->fd);
	close(fd);
	shl_hook_free(sig->hook);
	free(sig);
	/*
	 * We do not unblock the signal here as there may be other subsystems
	 * which blocked this signal so we do not want to interfere. If you
	 * need a clean sigmask then do it yourself.
	 */
}

/*
 * Eloop mainloop
 * The main eloop object is responsible for correctly dispatching all events.
 * You can register fd, idle or signal sources with it. All other kinds of
 * sources are based on these. In fact, event idle and signal sources are based
 * on fd sources.
 * As special feature, you can retrieve an fd of an eloop object, too, and pass
 * it to your own event loop. If this fd is readable, then call
 * ev_eloop_dispatch() to make this loop dispatch all pending events.
 *
 * There is one restriction when nesting eloops, though. You cannot share
 * signals across eloop boundaries. That is, if you have registered for shared
 * signals in two eloops for the _same_ signal, then only one eloop will
 * receive the signal (and this is pretty random).
 * However, such a setup is most often broken in design and hence should never
 * occur. Even shared signals are quite rare.
 * Anyway, you must take this into account when nesting eloops.
 *
 * For the curious reader: We implement idle sources with counter sources. That
 * is, whenever there is an idle source we increase the counter source. Hence,
 * the next dispatch call will call the counter source and this will call all
 * registered idle source. If the idle sources do not unregister them, then we
 * directly increase the counter again and the next dispatch round will call
 * all idle sources again. This, however, has the side-effect that idle sources
 * are _not_ called before other fd events but are rather mixed in between.
 */

static void eloop_event(struct ev_fd *fd, int mask, void *data)
{
	struct ev_eloop *eloop = data;

	if (mask & EV_READABLE)
		ev_eloop_dispatch(eloop, 0);
	if (mask & (EV_HUP | EV_ERR))
		llog_warn(eloop, "HUP/ERR on eloop source");
}

static int write_eventfd(llog_submit_t llog, void *llog_data, int fd,
			 uint64_t val)
{
	int ret;

	if (!val)
		return llog_dEINVAL(llog, llog_data);

	if (val == 0xffffffffffffffffULL) {
		llog_dwarning(llog, llog_data,
			      "increasing counter with invalid value %" PRIu64,
			      val);
		return -EINVAL;;
	}

	ret = write(fd, &val, sizeof(val));
	if (ret < 0) {
		if (errno == EAGAIN)
			llog_dwarning(llog, llog_data,
				      "eventfd overflow while writing %" PRIu64,
				      val);
		else
			llog_dwarning(llog, llog_data,
				      "eventfd write error (%d): %m", errno);
		return -EFAULT;
	} else if (ret != sizeof(val)) {
		llog_dwarning(llog, llog_data,
			      "wrote %d bytes instead of 8 to eventdfd", ret);
		return -EFAULT;
	}

	return 0;
}

static void eloop_idle_event(struct ev_eloop *loop, unsigned int mask)
{
	int ret;
	uint64_t val;

	if (mask & (EV_HUP | EV_ERR)) {
		llog_warning(loop, "HUP/ERR on eventfd");
		goto err_out;
	}

	if (!(mask & EV_READABLE))
		return;

	ret = read(loop->idle_fd, &val, sizeof(val));
	if (ret < 0) {
		if (errno != EAGAIN) {
			llog_warning(loop, "reading eventfd failed (%d): %m",
				     errno);
			goto err_out;
		}
	} else if (ret == 0) {
		llog_warning(loop, "EOF on eventfd");
		goto err_out;
	} else if (ret != sizeof(val)) {
		llog_warning(loop, "read %d bytes instead of 8 on eventfd",
			     ret);
		goto err_out;
	} else if (val > 0) {
		shl_hook_call(loop->idlers, loop, NULL);
		if (shl_hook_num(loop->idlers) > 0)
			write_eventfd(loop->llog, loop->llog_data,
				      loop->idle_fd, 1);
	}

	return;

err_out:
	ret = epoll_ctl(loop->efd, EPOLL_CTL_DEL, loop->idle_fd, NULL);
	if (ret)
		llog_warning(loop, "cannot remove fd %d from epollset (%d): %m",
			     loop->idle_fd, errno);
}

/**
 * ev_eloop_new:
 * @out: Storage for the result
 * @log: logging function or NULL
 * @log_data: logging function user-data
 *
 * This creates a new event-loop with ref-count 1. The new event loop is stored
 * in @out and has no registered events.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_eloop_new(struct ev_eloop **out, ev_log_t log, void *log_data)
{
	struct ev_eloop *loop;
	int ret;
	struct epoll_event ep;

	if (!out)
		return llog_dEINVAL(log, log_data);

	loop = malloc(sizeof(*loop));
	if (!loop)
		return llog_dENOMEM(log, log_data);

	memset(loop, 0, sizeof(*loop));
	loop->ref = 1;
	loop->llog = log;
	loop->llog_data = log_data;
	shl_dlist_init(&loop->sig_list);

	loop->cur_fds_size = 32;
	loop->cur_fds = malloc(sizeof(struct epoll_event) *
			       loop->cur_fds_size);
	if (!loop->cur_fds) {
		ret = llog_ENOMEM(loop);
		goto err_free;
	}

	ret = shl_hook_new(&loop->chlds);
	if (ret)
		goto err_fds;

	ret = shl_hook_new(&loop->idlers);
	if (ret)
		goto err_childs;

	ret = shl_hook_new(&loop->pres);
	if (ret)
		goto err_idlers;

	ret = shl_hook_new(&loop->posts);
	if (ret)
		goto err_pres;

	loop->efd = epoll_create1(EPOLL_CLOEXEC);
	if (loop->efd < 0) {
		ret = -errno;
		llog_error(loop, "cannot create epoll-fd");
		goto err_posts;
	}

	ret = ev_fd_new(&loop->fd, loop->efd, EV_READABLE, eloop_event, loop,
			loop->llog, loop->llog_data);
	if (ret)
		goto err_close;

	loop->idle_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (loop->idle_fd < 0) {
		llog_error(loop, "cannot create eventfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_fd;
	}

	memset(&ep, 0, sizeof(ep));
	ep.events |= EPOLLIN;
	ep.data.ptr = loop;

	ret = epoll_ctl(loop->efd, EPOLL_CTL_ADD, loop->idle_fd, &ep);
	if (ret) {
		llog_warning(loop, "cannot add fd %d to epoll set (%d): %m",
			     loop->idle_fd, errno);
		ret = -EFAULT;
		goto err_idle_fd;
	}

	llog_debug(loop, "new eloop object %p", loop);
	*out = loop;
	return 0;

err_idle_fd:
	close(loop->idle_fd);
err_fd:
	ev_fd_unref(loop->fd);
err_close:
	close(loop->efd);
err_posts:
	shl_hook_free(loop->posts);
err_pres:
	shl_hook_free(loop->pres);
err_idlers:
	shl_hook_free(loop->idlers);
err_childs:
	shl_hook_free(loop->chlds);
err_fds:
	free(loop->cur_fds);
err_free:
	free(loop);
	return ret;
}

/**
 * ev_eloop_ref:
 * @loop: Event loop to be modified or NULL
 *
 * This increases the ref-count of @loop by 1.
 */
SHL_EXPORT
void ev_eloop_ref(struct ev_eloop *loop)
{
	if (!loop)
		return;

	++loop->ref;
}

/**
 * ev_eloop_unref:
 * @loop: Event loop to be modified or NULL
 *
 * This decreases the ref-count of @loop by 1. If it drops to zero, the event
 * loop is destroyed. Note that every registered event source takes a ref-count
 * of the event loop so this ref-count will never drop to zero while there is an
 * registered event source.
 */
SHL_EXPORT
void ev_eloop_unref(struct ev_eloop *loop)
{
	struct ev_signal_shared *sig;
	int ret;

	if (!loop)
		return;
	if (!loop->ref)
		return llog_vEINVAL(loop);
	if (--loop->ref)
		return;

	llog_debug(loop, "free eloop object %p", loop);

	if (shl_hook_num(loop->chlds))
		ev_eloop_unregister_signal_cb(loop, SIGCHLD, sig_child, loop);

	while (loop->sig_list.next != &loop->sig_list) {
		sig = shl_dlist_entry(loop->sig_list.next,
					struct ev_signal_shared,
					list);
		signal_free(sig);
	}

	ret = epoll_ctl(loop->efd, EPOLL_CTL_DEL, loop->idle_fd, NULL);
	if (ret)
		llog_warning(loop, "cannot remove fd %d from epollset (%d): %m",
			     loop->idle_fd, errno);
	close(loop->idle_fd);

	ev_fd_unref(loop->fd);
	close(loop->efd);
	shl_hook_free(loop->posts);
	shl_hook_free(loop->pres);
	shl_hook_free(loop->idlers);
	shl_hook_free(loop->chlds);
	free(loop->cur_fds);
	free(loop);
}

/**
 * ev_eloop_flush_fd:
 * @loop: The event loop where @fd is registered
 * @fd: The fd to be flushed
 *
 * If @loop is currently dispatching events, this will remove all pending events
 * of @fd from the current event-list.
 */
SHL_EXPORT
void ev_eloop_flush_fd(struct ev_eloop *loop, struct ev_fd *fd)
{
	int i;

	if (!loop)
		return;
	if (!fd)
		return llog_vEINVAL(loop);

	if (loop->dispatching) {
		for (i = 0; i < loop->cur_fds_cnt; ++i) {
			if (loop->cur_fds[i].data.ptr == fd)
				loop->cur_fds[i].data.ptr = NULL;
		}
	}
}

static unsigned int convert_mask(uint32_t mask)
{
	unsigned int res = 0;

	if (mask & EPOLLIN)
		res |= EV_READABLE;
	if (mask & EPOLLOUT)
		res |= EV_WRITEABLE;
	if (mask & EPOLLERR)
		res |= EV_ERR;
	if (mask & EPOLLHUP)
		res |= EV_HUP;

	return res;
}

/**
 * ev_eloop_dispatch:
 * @loop: Event loop to be dispatched
 * @timeout: Timeout in milliseconds
 *
 * This listens on @loop for incoming events and handles all events that
 * occurred. This waits at most @timeout milliseconds until returning. If
 * @timeout is -1, this waits until the first event arrives. If @timeout is 0,
 * then this returns directly if no event is currently pending.
 *
 * This performs only a single dispatch round. That is, if all sources where
 * checked for events and there are no more pending events, this will return. If
 * it handled events and the timeout has not elapsed, this will still return.
 *
 * If ev_eloop_exit() was called on @loop, then this will return immediately.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_eloop_dispatch(struct ev_eloop *loop, int timeout)
{
	struct epoll_event *ep;
	struct ev_fd *fd;
	int i, count, mask, ret;

	if (!loop)
		return -EINVAL;
	if (loop->exit)
		return llog_EINVAL(loop);
	if (loop->dispatching) {
		llog_warn(loop, "recursive dispatching not allowed");
		return -EOPNOTSUPP;
	}

	loop->dispatching = true;

	shl_hook_call(loop->pres, loop, NULL);

	count = epoll_wait(loop->efd,
			   loop->cur_fds,
			   loop->cur_fds_size,
			   timeout);
	if (count < 0) {
		if (errno == EINTR) {
			ret = 0;
			goto out_dispatch;
		} else {
			llog_warn(loop, "epoll_wait dispatching failed: %m");
			ret = -errno;
			goto out_dispatch;
		}
	} else if (count > loop->cur_fds_size) {
		count = loop->cur_fds_size;
	}

	ep = loop->cur_fds;
	loop->cur_fds_cnt = count;

	for (i = 0; i < count; ++i) {
		if (ep[i].data.ptr == loop) {
			mask = convert_mask(ep[i].events);
			eloop_idle_event(loop, mask);
		} else {
			fd = ep[i].data.ptr;
			if (!fd || !fd->cb || !fd->enabled)
				continue;

			mask = convert_mask(ep[i].events);
			fd->cb(fd, mask, fd->data);
		}
	}

	if (count == loop->cur_fds_size) {
		ep = realloc(loop->cur_fds, sizeof(struct epoll_event) *
			     loop->cur_fds_size * 2);
		if (!ep) {
			llog_warning(loop, "cannot reallocate dispatch cache to size %zu",
				    loop->cur_fds_size * 2);
		} else {
			loop->cur_fds = ep;
			loop->cur_fds_size *= 2;
		}
	}

	ret = 0;

out_dispatch:
	shl_hook_call(loop->posts, loop, NULL);
	loop->dispatching = false;
	return ret;
}

/**
 * ev_eloop_run:
 * @loop: The event loop to be run
 * @timeout: Timeout for this operation
 *
 * This is similar to ev_eloop_dispatch() but runs _exactly_ for @timeout
 * milliseconds. It calls ev_eloop_dispatch() as often as it can until the
 * timeout has elapsed. If @timeout is -1 this will run until you call
 * ev_eloop_exit(). If @timeout is 0 this is equal to calling
 * ev_eloop_dispatch() with a timeout of 0.
 *
 * Calling ev_eloop_exit() will always interrupt this function and make it
 * return.
 *
 * Returns: 0 on success, otherwise a negative error code
 */
SHL_EXPORT
int ev_eloop_run(struct ev_eloop *loop, int timeout)
{
	int ret;
	struct timeval tv, start;
	int64_t off, msec;

	if (!loop)
		return -EINVAL;
	loop->exit = false;

	llog_debug(loop, "run for %d msecs", timeout);
	gettimeofday(&start, NULL);

	while (!loop->exit) {
		ret = ev_eloop_dispatch(loop, timeout);
		if (ret)
			return ret;

		if (!timeout) {
			break;
		} else if (timeout > 0) {
			gettimeofday(&tv, NULL);
			off = tv.tv_sec - start.tv_sec;
			msec = (int64_t)tv.tv_usec - (int64_t)start.tv_usec;
			if (msec < 0) {
				off -= 1;
				msec = 1000000 + msec;
			}
			off *= 1000;
			off += msec / 1000;
			if (off >= timeout)
				break;
		}
	}

	return 0;
}

/**
 * ev_eloop_exit:
 * @loop: Event loop that should exit
 *
 * This makes a call to ev_eloop_run() stop.
 */
SHL_EXPORT
void ev_eloop_exit(struct ev_eloop *loop)
{
	if (!loop)
		return;

	llog_debug(loop, "exiting %p", loop);

	loop->exit = true;
	if (loop->fd->loop)
		ev_eloop_exit(loop->fd->loop);
}

/**
 * ev_eloop_get_fd:
 * @loop: Event loop
 *
 * Returns a single file descriptor for the whole event-loop. If that FD is
 * readable, then one of the event-sources is active and you should call
 * ev_eloop_dispatch(loop, 0); to dispatch these events.
 * If the fd is not readable, then ev_eloop_dispatch() would sleep as there are
 * no active events.
 *
 * Returns: A file descriptor for the event loop or negative error code
 */
SHL_EXPORT
int ev_eloop_get_fd(struct ev_eloop *loop)
{
	if (!loop)
		return -EINVAL;

	return loop->efd;
}

/**
 * ev_eloop_new_eloop:
 * @loop: The parent event-loop where the new event loop is registered
 * @out: Storage for new event loop
 *
 * This creates a new event loop and directly registers it as event source on
 * the parent event loop \loop.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_eloop_new_eloop(struct ev_eloop *loop, struct ev_eloop **out)
{
	struct ev_eloop *el;
	int ret;

	if (!loop)
		return -EINVAL;
	if (!out)
		return llog_EINVAL(loop);

	ret = ev_eloop_new(&el, loop->llog, loop->llog_data);
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

/**
 * ev_eloop_add_eloop:
 * @loop: Parent event loop
 * @add: The event loop that is registered as event source on @loop
 *
 * This registers the existing event loop @add as event source on the parent
 * event loop @loop.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_eloop_add_eloop(struct ev_eloop *loop, struct ev_eloop *add)
{
	int ret;

	if (!loop)
		return -EINVAL;
	if (!add)
		return llog_EINVAL(loop);

	if (add->fd->loop)
		return -EALREADY;

	/* This adds the epoll-fd into the parent epoll-set. This works
	 * perfectly well with registered FDs, timers, etc. However, we use
	 * shared signals in this event-loop so if the parent and child have
	 * overlapping shared-signals, then the signal will be randomly
	 * delivered to either the parent-hook or child-hook but never both.
	 * TODO:
	 * We may fix this by linking the childs-sig_list into the parent's
	 * siglist but we didn't need this, yet, so ignore it here.
	 */

	ret = ev_eloop_add_fd(loop, add->fd);
	if (ret)
		return ret;

	ev_eloop_ref(add);
	return 0;
}

/**
 * ev_eloop_rm_eloop:
 * @rm: Event loop to be unregistered from its parent
 *
 * This unregisters the event loop @rm as event source from its parent. If this
 * event loop was not registered on any other event loop, then this call does
 * nothing.
 */
SHL_EXPORT
void ev_eloop_rm_eloop(struct ev_eloop *rm)
{
	if (!rm || !rm->fd->loop)
		return;

	ev_eloop_rm_fd(rm->fd);
	ev_eloop_unref(rm);
}

/*
 * FD sources
 * This allows adding file descriptors to an eloop. A file descriptor is the
 * most basic kind of source and used for all other source types.
 * By default a source is always enabled but you can easily disable the source
 * by calling ev_fd_disable(). This will have the effect, that the source is
 * still registered with the eloop but will not wake up the thread or get
 * called until you enable it again.
 */

/**
 * ev_fd_new:
 * @out: Storage for result
 * @rfd: The actual file descriptor
 * @mask: Bitmask of %EV_READABLE and %EV_WRITEABLE flags
 * @cb: User callback
 * @data: User data
 * @log: llog function or NULL
 * @log_data: logging function user-data
 *
 * This creates a new file descriptor source that is watched for the events set
 * in @mask. @rfd is the system filedescriptor. The resulting object is stored
 * in @out. @cb and @data are the user callback and the user-supplied data that
 * is passed to the callback on events.
 * The FD is automatically watched for EV_HUP and EV_ERR events, too.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_fd_new(struct ev_fd **out, int rfd, int mask, ev_fd_cb cb, void *data,
	      ev_log_t log, void *log_data)
{
	struct ev_fd *fd;

	if (!out || rfd < 0)
		return llog_dEINVAL(log, log_data);

	fd = malloc(sizeof(*fd));
	if (!fd)
		return llog_dEINVAL(log, log_data);

	memset(fd, 0, sizeof(*fd));
	fd->ref = 1;
	fd->llog = log;
	fd->llog_data = log_data;
	fd->fd = rfd;
	fd->mask = mask;
	fd->cb = cb;
	fd->data = data;
	fd->enabled = true;

	*out = fd;
	return 0;
}

/**
 * ev_fd_ref:
 * @fd: FD object
 *
 * Increases the ref-count of @fd by 1.
 */
SHL_EXPORT
void ev_fd_ref(struct ev_fd *fd)
{
	if (!fd)
		return;
	if (!fd->ref)
		return llog_vEINVAL(fd);

	++fd->ref;
}

/**
 * ev_fd_unref:
 * @fd: FD object
 *
 * Decreases the ref-count of @fd by 1. Destroys the object if the ref-count
 * drops to zero.
 */
SHL_EXPORT
void ev_fd_unref(struct ev_fd *fd)
{
	if (!fd)
		return;
	if (!fd->ref)
		return llog_vEINVAL(fd);
	if (--fd->ref)
		return;

	free(fd);
}

static int fd_epoll_add(struct ev_fd *fd)
{
	struct epoll_event ep;
	int ret;

	if (!fd->loop)
		return 0;

	memset(&ep, 0, sizeof(ep));
	if (fd->mask & EV_READABLE)
		ep.events |= EPOLLIN;
	if (fd->mask & EV_WRITEABLE)
		ep.events |= EPOLLOUT;
	if (fd->mask & EV_ET)
		ep.events |= EPOLLET;
	ep.data.ptr = fd;

	ret = epoll_ctl(fd->loop->efd, EPOLL_CTL_ADD, fd->fd, &ep);
	if (ret) {
		llog_warning(fd, "cannot add fd %d to epoll set (%d): %m",
			     fd->fd, errno);
		return -EFAULT;
	}

	return 0;
}

static void fd_epoll_remove(struct ev_fd *fd)
{
	int ret;

	if (!fd->loop)
		return;

	ret = epoll_ctl(fd->loop->efd, EPOLL_CTL_DEL, fd->fd, NULL);
	if (ret && errno != EBADF)
		llog_warning(fd, "cannot remove fd %d from epoll set (%d): %m",
			     fd->fd, errno);
}

static int fd_epoll_update(struct ev_fd *fd)
{
	struct epoll_event ep;
	int ret;

	if (!fd->loop)
		return 0;

	memset(&ep, 0, sizeof(ep));
	if (fd->mask & EV_READABLE)
		ep.events |= EPOLLIN;
	if (fd->mask & EV_WRITEABLE)
		ep.events |= EPOLLOUT;
	if (fd->mask & EV_ET)
		ep.events |= EPOLLET;
	ep.data.ptr = fd;

	ret = epoll_ctl(fd->loop->efd,  EPOLL_CTL_MOD, fd->fd, &ep);
	if (ret) {
		llog_warning(fd, "cannot update epoll fd %d (%d): %m",
			     fd->fd, errno);
		return -EFAULT;
	}

	return 0;
}

/**
 * ev_fd_enable:
 * @fd: FD object
 *
 * This enables @fd. By default every fd object is enabled. If you disabled it
 * you can re-enable it with this call.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_fd_enable(struct ev_fd *fd)
{
	int ret;

	if (!fd)
		return -EINVAL;
	if (fd->enabled)
		return 0;

	ret = fd_epoll_add(fd);
	if (ret)
		return ret;

	fd->enabled = true;
	return 0;
}

/**
 * ev_fd_disable:
 * @fd: FD object
 *
 * Disables @fd. That means, no more events are handled for @fd until you
 * re-enable it with ev_fd_enable().
 */
SHL_EXPORT
void ev_fd_disable(struct ev_fd *fd)
{
	if (!fd || !fd->enabled)
		return;

	fd->enabled = false;
	fd_epoll_remove(fd);
}

/**
 * ev_fd_is_enabled:
 * @fd: FD object
 *
 * Returns whether the fd object is enabled or disabled.
 *
 * Returns: true if @fd is enabled, otherwise false.
 */
SHL_EXPORT
bool ev_fd_is_enabled(struct ev_fd *fd)
{
	return fd && fd->enabled;
}

/**
 * ev_fd_is_bound:
 * @fd: FD object
 *
 * Returns true if the fd object is bound to an event loop.
 *
 * Returns: true if @fd is bound, otherwise false
 */
SHL_EXPORT
bool ev_fd_is_bound(struct ev_fd *fd)
{
	return fd && fd->loop;
}

/**
 * ev_fd_set_cb_data:
 * @fd: FD object
 * @cb: New user callback
 * @data: New user data
 *
 * This changes the user callback and user data that were set in ev_fd_new().
 * Both can be set to NULL. If @cb is NULL, then the callback will not be called
 * anymore.
 */
SHL_EXPORT
void ev_fd_set_cb_data(struct ev_fd *fd, ev_fd_cb cb, void *data)
{
	if (!fd)
		return;

	fd->cb = cb;
	fd->data = data;
}

/**
 * ev_fd_update:
 * @fd: FD object
 * @mask: Bitmask of %EV_READABLE and %EV_WRITEABLE
 *
 * This resets the event mask of @fd to @mask.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_fd_update(struct ev_fd *fd, int mask)
{
	int ret;
	int omask;

	if (!fd)
		return -EINVAL;
	if (fd->mask == mask && !(mask & EV_ET))
		return 0;

	omask = fd->mask;
	fd->mask = mask;

	if (!fd->enabled)
		return 0;

	ret = fd_epoll_update(fd);
	if (ret) {
		fd->mask = omask;
		return ret;
	}

	return 0;
}

/**
 * ev_eloop_new_fd:
 * @loop: Event loop
 * @out: Storage for result
 * @rfd: File descriptor
 * @mask: Bitmask of %EV_READABLE and %EV_WRITEABLE
 * @cb: User callback
 * @data: User data
 *
 * This creates a new fd object like ev_fd_new() and directly registers it in
 * the event loop @loop. See ev_fd_new() and ev_eloop_add_fd() for more
 * information.
 * The ref-count of @out is 1 so you must call ev_eloop_rm_fd() to destroy the
 * fd. You must not call ev_fd_unref() unless you called ev_fd_ref() before.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_eloop_new_fd(struct ev_eloop *loop, struct ev_fd **out, int rfd,
			int mask, ev_fd_cb cb, void *data)
{
	struct ev_fd *fd;
	int ret;

	if (!loop)
		return -EINVAL;
	if (!out || rfd < 0)
		return llog_EINVAL(loop);

	ret = ev_fd_new(&fd, rfd, mask, cb, data, loop->llog, loop->llog_data);
	if (ret)
		return ret;

	ret = ev_eloop_add_fd(loop, fd);
	if (ret) {
		ev_fd_unref(fd);
		return ret;
	}

	ev_fd_unref(fd);
	*out = fd;
	return 0;
}

/**
 * ev_eloop_add_fd:
 * @loop: Event loop
 * @fd: FD Object
 *
 * Registers @fd in the event loop @loop. This increases the ref-count of both
 * @loop and @fd. From now on the user callback of @fd may get called during
 * dispatching.
 *
 * Returns: 0 on success, otherwise negative error code
 */
SHL_EXPORT
int ev_eloop_add_fd(struct ev_eloop *loop, struct ev_fd *fd)
{
	int ret;

	if (!loop)
		return -EINVAL;
	if (!fd || fd->loop)
		return llog_EINVAL(loop);

	fd->loop = loop;

	if (fd->enabled) {
		ret = fd_epoll_add(fd);
		if (ret) {
			fd->loop = NULL;
			return ret;
		}
	}

	ev_fd_ref(fd);
	ev_eloop_ref(loop);
	return 0;
}

/**
 * ev_eloop_rm_fd:
 * @fd: FD object
 *
 * Removes the fd object @fd from its event loop. If you did not call
 * ev_eloop_add_fd() before, this will do nothing.
 * This decreases the refcount of @fd and the event loop by 1.
 * It is safe to call this in any callback. This makes sure that the current
 * dispatcher will not get confused or read invalid memory.
 */
SHL_EXPORT
void ev_eloop_rm_fd(struct ev_fd *fd)
{
	struct ev_eloop *loop;
	size_t i;

	if (!fd || !fd->loop)
		return;

	loop = fd->loop;
	if (fd->enabled)
		fd_epoll_remove(fd);

	/*
	 * If we are currently dispatching events, we need to remove ourself
	 * from the temporary event list.
	 */
	if (loop->dispatching) {
		for (i = 0; i < loop->cur_fds_cnt; ++i) {
			if (fd == loop->cur_fds[i].data.ptr)
				loop->cur_fds[i].data.ptr = NULL;
		}
	}

	fd->loop = NULL;
	ev_fd_unref(fd);
	ev_eloop_unref(loop);
}

/*
 * Timer sources
 * Timer sources allow delaying a specific event by an relative timeout. The
 * timeout can be set to trigger after a specific time. Optionally, you can
 * also make the timeout trigger every next time the timeout elapses so you
 * basically get a pulse that reliably calls the callback.
 * The callback gets as parameter the number of timeouts that elapsed since it
 * was last called (in case the application couldn't call the callback fast
 * enough). The timeout can be specified with nano-seconds precision. However,
 * real precision depends on the operating-system and hardware.
 */

static int timer_drain(struct ev_timer *timer, uint64_t *out)
{
	int len;
	uint64_t expirations;

	if (out)
		*out = 0;

	len = read(timer->fd, &expirations, sizeof(expirations));
	if (len < 0) {
		if (errno == EAGAIN) {
			return 0;
		} else {
			llog_warning(timer, "cannot read timerfd (%d): %m",
				     errno);
			return errno;
		}
	} else if (len == 0) {
		llog_warning(timer, "EOF on timer source");
		return -EFAULT;
	} else if (len != sizeof(expirations)) {
		llog_warn(timer, "invalid size %d read on timerfd", len);
		return -EFAULT;
	} else {
		if (out)
			*out = expirations;
		return 0;
	}
}

static void timer_cb(struct ev_fd *fd, int mask, void *data)
{
	struct ev_timer *timer = data;
	uint64_t expirations;
	int ret;

	if (mask & (EV_HUP | EV_ERR)) {
		llog_warn(fd, "HUP/ERR on timer source");
		goto err_cb;
	}

	if (mask & EV_READABLE) {
		ret = timer_drain(timer, &expirations);
		if (ret)
			goto err_cb;
		if (expirations > 0) {
			if (timer->cb)
				timer->cb(timer, expirations, timer->data);
		}
	}

	return;

err_cb:
	ev_timer_disable(timer);
	if (timer->cb)
		timer->cb(timer, 0, timer->data);
}

static const struct itimerspec ev_timer_zero;

/**
 * ev_timer_new:
 * @out: Timer pointer where to store the new timer
 * @spec: Timespan
 * @cb: callback to use for this event-source
 * @data: user-specified data
 * @log: logging function or NULL
 * @log_data: logging function user-data
 *
 * This creates a new timer-source. See "man timerfd_create" for information on
 * the @spec argument. The timer is always relative and uses the
 * monotonic-kernel clock.
 *
 * Returns: 0 on success, negative error on failure
 */
SHL_EXPORT
int ev_timer_new(struct ev_timer **out, const struct itimerspec *spec,
		 ev_timer_cb cb, void *data, ev_log_t log, void *log_data)
{
	struct ev_timer *timer;
	int ret;

	if (!out)
		return llog_dEINVAL(log, log_data);

	if (!spec)
		spec = &ev_timer_zero;

	timer = malloc(sizeof(*timer));
	if (!timer)
		return llog_dENOMEM(log, log_data);

	memset(timer, 0, sizeof(*timer));
	timer->ref = 1;
	timer->llog = log;
	timer->llog_data = log_data;
	timer->cb = cb;
	timer->data = data;

	timer->fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (timer->fd < 0) {
		llog_error(timer, "cannot create timerfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_free;
	}

	ret = timerfd_settime(timer->fd, 0, spec, NULL);
	if (ret) {
		llog_warn(timer, "cannot set timerfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_close;
	}

	ret = ev_fd_new(&timer->efd, timer->fd, EV_READABLE, timer_cb, timer,
			timer->llog, timer->llog_data);
	if (ret)
		goto err_close;

	*out = timer;
	return 0;

err_close:
	close(timer->fd);
err_free:
	free(timer);
	return ret;
}

/**
 * ev_timer_ref:
 * @timer: Timer object
 *
 * Increase reference count by 1.
 */
SHL_EXPORT
void ev_timer_ref(struct ev_timer *timer)
{
	if (!timer)
		return;
	if (!timer->ref)
		return llog_vEINVAL(timer);

	++timer->ref;
}

/**
 * ev_timer_unref:
 * @timer: Timer object
 *
 * Decrease reference-count by 1 and destroy timer if it drops to 0.
 */
SHL_EXPORT
void ev_timer_unref(struct ev_timer *timer)
{
	if (!timer)
		return;
	if (!timer->ref)
		return llog_vEINVAL(timer);
	if (--timer->ref)
		return;

	ev_fd_unref(timer->efd);
	close(timer->fd);
	free(timer);
}

/**
 * ev_timer_enable:
 * @timer: Timer object
 *
 * Enable the timer. This calls ev_fd_enable() on the fd that implements this
 * timer.
 *
 * Returns: 0 on success negative error code on failure
 */
SHL_EXPORT
int ev_timer_enable(struct ev_timer *timer)
{
	if (!timer)
		return -EINVAL;

	return ev_fd_enable(timer->efd);
}

/**
 * ev_timer_disable:
 * @timer: Timer object
 *
 * Disable the timer. This calls ev_fd_disable() on the fd that implements this
 * timer.
 *
 * Returns: 0 on success and negative error code on failure
 */
SHL_EXPORT
void ev_timer_disable(struct ev_timer *timer)
{
	if (!timer)
		return;

	ev_fd_disable(timer->efd);
}

/**
 * ev_timer_is_enabled:
 * @timer: Timer object
 *
 * Checks whether the timer is enabled.
 *
 * Returns: true if timer is enabled, false otherwise
 */
SHL_EXPORT
bool ev_timer_is_enabled(struct ev_timer *timer)
{
	return timer && ev_fd_is_enabled(timer->efd);
}

/**
 * ev_timer_is_bound:
 * @timer: Timer object
 *
 * Checks whether the timer is bound to an event loop.
 *
 * Returns: true if the timer is bound, false otherwise.
 */
SHL_EXPORT
bool ev_timer_is_bound(struct ev_timer *timer)
{
	return timer && ev_fd_is_bound(timer->efd);
}

/**
 * ev_timer_set_cb_data:
 * @timer: Timer object
 * @cb: User callback or NULL
 * @data: User data or NULL
 *
 * This changes the user-supplied callback and data that is used for this timer
 * object.
 */
SHL_EXPORT
void ev_timer_set_cb_data(struct ev_timer *timer, ev_timer_cb cb, void *data)
{
	if (!timer)
		return;

	timer->cb = cb;
	timer->data = data;
}

/**
 * ev_timer_update:
 * @timer: Timer object
 * @spec: timespan
 *
 * This changes the timer timespan. See "man timerfd_settime" for information
 * on the @spec parameter.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_timer_update(struct ev_timer *timer, const struct itimerspec *spec)
{
	int ret;

	if (!timer)
		return -EINVAL;

	if (!spec)
		spec = &ev_timer_zero;

	ret = timerfd_settime(timer->fd, 0, spec, NULL);
	if (ret) {
		llog_warn(timer, "cannot set timerfd (%d): %m", errno);
		return -EFAULT;
	}

	return 0;
}

/**
 * ev_timer_drain:
 * @timer: valid timer object
 * @expirations: destination to save result or NULL
 *
 * This reads the current expiration-count from the timer object @timer and
 * saves it in @expirations (if it is non-NULL). This can be used to clear the
 * timer after an idle-period or similar.
 * Note that the timer_cb() callback function automatically calls this before
 * calling the user-supplied callback.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_timer_drain(struct ev_timer *timer, uint64_t *expirations)
{
	if (!timer)
		return -EINVAL;

	return timer_drain(timer, expirations);
}

/**
 * ev_eloop_new_timer:
 * @loop: event loop
 * @out: output where to store the new timer
 * @spec: timespan
 * @cb: user callback
 * @data: user-supplied data
 *
 * This is a combination of ev_timer_new() and ev_eloop_add_timer(). See both
 * for more information.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_new_timer(struct ev_eloop *loop, struct ev_timer **out,
			const struct itimerspec *spec, ev_timer_cb cb,
			void *data)
{
	struct ev_timer *timer;
	int ret;

	if (!loop)
		return -EINVAL;
	if (!out)
		return llog_EINVAL(loop);

	ret = ev_timer_new(&timer, spec, cb, data, loop->llog, loop->llog_data);
	if (ret)
		return ret;

	ret = ev_eloop_add_timer(loop, timer);
	if (ret) {
		ev_timer_unref(timer);
		return ret;
	}

	ev_timer_unref(timer);
	*out = timer;
	return 0;
}

/**
 * ev_eloop_add_timer:
 * @loop: event loop
 * @timer: Timer source
 *
 * This adds @timer as source to @loop. @timer must be currently unbound,
 * otherwise, this will fail with -EALREADY.
 *
 * Returns: 0 on success, negative error code on failure
 */
SHL_EXPORT
int ev_eloop_add_timer(struct ev_eloop *loop, struct ev_timer *timer)
{
	int ret;

	if (!loop)
		return -EINVAL;
	if (!timer)
		return llog_EINVAL(loop);

	if (ev_fd_is_bound(timer->efd))
		return -EALREADY;

	ret = ev_eloop_add_fd(loop, timer->efd);
	if (ret)
		return ret;

	ev_timer_ref(timer);
	return 0;
}

/**
 * ev_eloop_rm_timer:
 * @timer: Timer object
 *
 * If @timer is currently bound to an event loop, this will remove this bondage
 * again.
 */
SHL_EXPORT
void ev_eloop_rm_timer(struct ev_timer *timer)
{
	if (!timer || !ev_fd_is_bound(timer->efd))
		return;

	ev_eloop_rm_fd(timer->efd);
	ev_timer_unref(timer);
}

/*
 * Counter Sources
 * Counter sources are a very basic event notification mechanism. It is based
 * around the eventfd() system call on linux machines. Internally, there is a
 * 64bit unsigned integer that can be increased by the caller. By default it is
 * set to 0. If it is non-zero, the event-fd will be notified and the
 * user-defined callback is called. The callback gets as argument the current
 * state of the counter and the counter is reset to 0.
 *
 * If the internal counter would overflow, an increase() fails silently so an
 * overflow will never occur, however, you may loose events this way. This can
 * be ignored when increasing with small values, only.
 */

static void counter_event(struct ev_fd *fd, int mask, void *data)
{
	struct ev_counter *cnt = data;
	int ret;
	uint64_t val;

	if (mask & (EV_HUP | EV_ERR)) {
		llog_warning(fd, "HUP/ERR on eventfd");
		if (cnt->cb)
			cnt->cb(cnt, 0, cnt->data);
		return;
	}

	if (!(mask & EV_READABLE))
		return;

	ret = read(cnt->fd, &val, sizeof(val));
	if (ret < 0) {
		if (errno != EAGAIN) {
			llog_warning(fd, "reading eventfd failed (%d): %m", errno);
			ev_counter_disable(cnt);
			if (cnt->cb)
				cnt->cb(cnt, 0, cnt->data);
		}
	} else if (ret == 0) {
		llog_warning(fd, "EOF on eventfd");
		ev_counter_disable(cnt);
		if (cnt->cb)
			cnt->cb(cnt, 0, cnt->data);
	} else if (ret != sizeof(val)) {
		llog_warning(fd, "read %d bytes instead of 8 on eventfd", ret);
		ev_counter_disable(cnt);
		if (cnt->cb)
			cnt->cb(cnt, 0, cnt->data);
	} else if (val > 0) {
		if (cnt->cb)
			cnt->cb(cnt, val, cnt->data);
	}
}

/**
 * ev_counter_new:
 * @out: Where to store the new counter
 * @cb: user-supplied callback
 * @data: user-supplied data
 * @log: logging function or NULL
 * @log_data: logging function user-data
 *
 * This creates a new counter object and stores it in @out.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_counter_new(struct ev_counter **out, ev_counter_cb cb, void *data,
		   ev_log_t log, void *log_data)
{
	struct ev_counter *cnt;
	int ret;

	if (!out)
		return llog_dEINVAL(log, log_data);

	cnt = malloc(sizeof(*cnt));
	if (!cnt)
		return llog_dENOMEM(log, log_data);
	memset(cnt, 0, sizeof(*cnt));
	cnt->ref = 1;
	cnt->llog = log;
	cnt->llog_data = log_data;
	cnt->cb = cb;
	cnt->data = data;

	cnt->fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (cnt->fd < 0) {
		llog_error(cnt, "cannot create eventfd (%d): %m", errno);
		ret = -EFAULT;
		goto err_free;
	}

	ret = ev_fd_new(&cnt->efd, cnt->fd, EV_READABLE, counter_event, cnt,
			cnt->llog, cnt->llog_data);
	if (ret)
		goto err_close;

	*out = cnt;
	return 0;

err_close:
	close(cnt->fd);
err_free:
	free(cnt);
	return ret;
}

/**
 * ev_counter_ref:
 * @cnt: Counter object
 *
 * This increases the reference-count of @cnt by 1.
 */
SHL_EXPORT
void ev_counter_ref(struct ev_counter *cnt)
{
	if (!cnt)
		return;
	if (!cnt->ref)
		return llog_vEINVAL(cnt);

	++cnt->ref;
}

/**
 * ev_counter_unref:
 * @cnt: Counter object
 *
 * This decreases the reference-count of @cnt by 1 and destroys the object if
 * it drops to 0.
 */
SHL_EXPORT
void ev_counter_unref(struct ev_counter *cnt)
{
	if (!cnt)
		return;
	if (!cnt->ref)
		return llog_vEINVAL(cnt);
	if (--cnt->ref)
		return;

	ev_fd_unref(cnt->efd);
	close(cnt->fd);
	free(cnt);
}

/**
 * ev_counter_enable:
 * @cnt: Counter object
 *
 * This enables the counter object. It calls ev_fd_enable() on the underlying
 * file-descriptor.
 *
 * Returns: 0 on success, negative error code on failure
 */
SHL_EXPORT
int ev_counter_enable(struct ev_counter *cnt)
{
	if (!cnt)
		return -EINVAL;

	return ev_fd_enable(cnt->efd);
}

/**
 * ev_counter_disable:
 * @cnt: Counter object
 *
 * This disables the counter. It calls ev_fd_disable() on the underlying
 * file-descriptor.
 */
SHL_EXPORT
void ev_counter_disable(struct ev_counter *cnt)
{
	if (!cnt)
		return;

	ev_fd_disable(cnt->efd);
}

/**
 * ev_counter_is_enabled:
 * @cnt: counter object
 *
 * Checks whether the counter is enabled.
 *
 * Returns: true if the counter is enabled, otherwise returns false.
 */
SHL_EXPORT
bool ev_counter_is_enabled(struct ev_counter *cnt)
{
	return cnt && ev_fd_is_enabled(cnt->efd);
}

/**
 * ev_counter_is_bound:
 * @cnt: Counter object
 *
 * Checks whether the counter is bound to an event loop.
 *
 * Returns: true if the counter is bound, otherwise false is returned.
 */
SHL_EXPORT
bool ev_counter_is_bound(struct ev_counter *cnt)
{
	return cnt && ev_fd_is_bound(cnt->efd);
}

/**
 * ev_counter_set_cb_data:
 * @cnt: Counter object
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This changes the user-supplied callback and data for the given counter
 * object.
 */
SHL_EXPORT
void ev_counter_set_cb_data(struct ev_counter *cnt, ev_counter_cb cb,
			    void *data)
{
	if (!cnt)
		return;

	cnt->cb = cb;
	cnt->data = data;
}

/**
 * ev_counter_inc:
 * @cnt: Counter object
 * @val: Counter increase amount
 *
 * This increases the counter @cnt by @val.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_counter_inc(struct ev_counter *cnt, uint64_t val)
{
	if (!cnt)
		return -EINVAL;

	return write_eventfd(cnt->llog, cnt->llog_data, cnt->fd, val);
}

/**
 * ev_eloop_new_counter:
 * @eloop: event loop
 * @out: output storage for new counter
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This combines ev_counter_new() and ev_eloop_add_counter() in one call.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_new_counter(struct ev_eloop *eloop, struct ev_counter **out,
			 ev_counter_cb cb, void *data)
{
	int ret;
	struct ev_counter *cnt;

	if (!eloop)
		return -EINVAL;
	if (!out)
		return llog_EINVAL(eloop);

	ret = ev_counter_new(&cnt, cb, data, eloop->llog, eloop->llog_data);
	if (ret)
		return ret;

	ret = ev_eloop_add_counter(eloop, cnt);
	if (ret) {
		ev_counter_unref(cnt);
		return ret;
	}

	ev_counter_unref(cnt);
	*out = cnt;
	return 0;
}

/**
 * ev_eloop_add_counter:
 * @eloop: Event loop
 * @cnt: Counter object
 *
 * This adds @cnt to the given event loop @eloop. If @cnt is already bound,
 * this will fail with -EALREADY.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_add_counter(struct ev_eloop *eloop, struct ev_counter *cnt)
{
	int ret;

	if (!eloop)
		return -EINVAL;
	if (!cnt)
		return llog_EINVAL(eloop);

	if (ev_fd_is_bound(cnt->efd))
		return -EALREADY;

	ret = ev_eloop_add_fd(eloop, cnt->efd);
	if (ret)
		return ret;

	ev_counter_ref(cnt);
	return 0;
}

/**
 * ev_eloop_rm_counter:
 * @cnt: Counter object
 *
 * If @cnt is bound to an event-loop, then this will remove this bondage again.
 */
SHL_EXPORT
void ev_eloop_rm_counter(struct ev_counter *cnt)
{
	if (!cnt || !ev_fd_is_bound(cnt->efd))
		return;

	ev_eloop_rm_fd(cnt->efd);
	ev_counter_unref(cnt);
}

/*
 * Shared signals
 * This allows registering for shared signal events. See description of the
 * shared signal object above for more information how this works. Also see the
 * eloop description to see some drawbacks when nesting eloop objects with the
 * same shared signal sources.
 */

/**
 * ev_eloop_register_signal_cb:
 * @loop: event loop
 * @signum: Signal number
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This register a new callback for the given signal @signum. @cb must not be
 * NULL!
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_register_signal_cb(struct ev_eloop *loop, int signum,
				ev_signal_shared_cb cb, void *data)
{
	struct ev_signal_shared *sig = NULL;
	int ret;
	struct shl_dlist *iter;

	if (!loop)
		return -EINVAL;
	if (signum < 0 || !cb)
		return llog_EINVAL(loop);

	shl_dlist_for_each(iter, &loop->sig_list) {
		sig = shl_dlist_entry(iter, struct ev_signal_shared, list);
		if (sig->signum == signum)
			break;
		sig = NULL;
	}

	if (!sig) {
		ret = signal_new(&sig, loop, signum);
		if (ret)
			return ret;
	}

	ret = shl_hook_add_cast(sig->hook, cb, data, false);
	if (ret) {
		signal_free(sig);
		return ret;
	}

	return 0;
}

/**
 * ev_eloop_unregister_signal_cb:
 * @loop: event loop
 * @signum: signal number
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This removes a previously registered signal-callback again. The arguments
 * must be the same as for the ev_eloop_register_signal_cb() call. If multiple
 * callbacks with the same arguments are registered, then only one callback is
 * removed. It doesn't matter which callback is removed as both are identical.
 */
SHL_EXPORT
void ev_eloop_unregister_signal_cb(struct ev_eloop *loop, int signum,
					ev_signal_shared_cb cb, void *data)
{
	struct ev_signal_shared *sig;
	struct shl_dlist *iter;

	if (!loop)
		return;

	shl_dlist_for_each(iter, &loop->sig_list) {
		sig = shl_dlist_entry(iter, struct ev_signal_shared, list);
		if (sig->signum == signum) {
			shl_hook_rm_cast(sig->hook, cb, data);
			if (!shl_hook_num(sig->hook))
				signal_free(sig);
			return;
		}
	}
}

/*
 * Child reaper sources
 * If at least one child-reaper callback is registered, then the eloop object
 * listens for SIGCHLD and waits for all exiting children. The callbacks are
 * then notified for each PID that signaled an event.
 * Note that this cannot be done via the shared-signal sources as the waitpid()
 * call must not be done in callbacks. Otherwise, only one callback would see
 * the events while others will call waitpid() and get EAGAIN.
 */

SHL_EXPORT
int ev_eloop_register_child_cb(struct ev_eloop *loop, ev_child_cb cb,
			       void *data)
{
	int ret;
	bool empty;

	if (!loop)
		return -EINVAL;

	empty = !shl_hook_num(loop->chlds);
	ret = shl_hook_add_cast(loop->chlds, cb, data, false);
	if (ret)
		return ret;

	if (empty) {
		ret = ev_eloop_register_signal_cb(loop, SIGCHLD, sig_child,
						  loop);
		if (ret) {
			shl_hook_rm_cast(loop->chlds, cb, data);
			return ret;
		}
	}

	return 0;
}

SHL_EXPORT
void ev_eloop_unregister_child_cb(struct ev_eloop *loop, ev_child_cb cb,
				  void *data)
{
	if (!loop || !shl_hook_num(loop->chlds))
		return;

	shl_hook_rm_cast(loop->chlds, cb, data);
	if (!shl_hook_num(loop->chlds))
		ev_eloop_unregister_signal_cb(loop, SIGCHLD, sig_child, loop);
}

/*
 * Idle sources
 * Idle sources are called every time when a next dispatch round is started.
 * That means, unless there is no idle source registered, the thread will
 * _never_ go to sleep. So please unregister your idle source if no longer
 * needed.
 */

/**
 * ev_eloop_register_idle_cb:
 * @eloop: event loop
 * @cb: user-supplied callback
 * @data: user-supplied data
 * @flags: flags
 *
 * This register a new idle-source with the given callback and data. @cb must
 * not be NULL!.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_register_idle_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			      void *data, unsigned int flags)
{
	int ret;
	bool os = flags & EV_ONESHOT;

	if (!eloop || (flags & ~EV_IDLE_ALL))
		return -EINVAL;

	if ((flags & EV_SINGLE))
		ret = shl_hook_add_single_cast(eloop->idlers, cb, data, os);
	else
		ret = shl_hook_add_cast(eloop->idlers, cb, data, os);

	if (ret)
		return ret;

	ret = write_eventfd(eloop->llog, eloop->llog_data, eloop->idle_fd, 1);
	if (ret) {
		llog_warning(eloop, "cannot increase eloop idle-counter");
		shl_hook_rm_cast(eloop->idlers, cb, data);
		return ret;
	}

	return 0;
}

/**
 * ev_eloop_unregister_idle_cb:
 * @eloop: event loop
 * @cb: user-supplied callback
 * @data: user-supplied data
 * @flags: flags
 *
 * This removes an idle-source. The arguments must be the same as for the
 * ev_eloop_register_idle_cb() call. If two identical callbacks are registered,
 * then only one is removed. It doesn't matter which one is removed, because
 * they are identical.
 */
SHL_EXPORT
void ev_eloop_unregister_idle_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				 void *data, unsigned int flags)
{
	if (!eloop || (flags & ~EV_IDLE_ALL))
		return;

	if (flags & EV_SINGLE)
		shl_hook_rm_all_cast(eloop->idlers, cb, data);
	else
		shl_hook_rm_cast(eloop->idlers, cb, data);
}

/*
 * Pre-Dispatch Callbacks
 * A pre-dispatch cb is called before a single dispatch round is started.
 * You should avoid using them and instead not rely on any specific
 * dispatch-behavior but expect every event to be received asynchronously.
 * However, this hook is useful to integrate other limited APIs into this event
 * loop if they do not provide proper FD-abstractions.
 */

/**
 * ev_eloop_register_pre_cb:
 * @eloop: event loop
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This register a new pre-cb with the given callback and data. @cb must
 * not be NULL!.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_register_pre_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			     void *data)
{
	if (!eloop)
		return -EINVAL;

	return shl_hook_add_cast(eloop->pres, cb, data, false);
}

/**
 * ev_eloop_unregister_pre_cb:
 * @eloop: event loop
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This removes a pre-cb. The arguments must be the same as for the
 * ev_eloop_register_pre_cb() call. If two identical callbacks are registered,
 * then only one is removed. It doesn't matter which one is removed, because
 * they are identical.
 */
SHL_EXPORT
void ev_eloop_unregister_pre_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				void *data)
{
	if (!eloop)
		return;

	shl_hook_rm_cast(eloop->pres, cb, data);
}

/*
 * Post-Dispatch Callbacks
 * A post-dispatch cb is called whenever a single dispatch round is complete.
 * You should avoid using them and instead not rely on any specific
 * dispatch-behavior but expect every event to be received asynchronously.
 * However, this hook is useful to integrate other limited APIs into this event
 * loop if they do not provide proper FD-abstractions.
 */

/**
 * ev_eloop_register_post_cb:
 * @eloop: event loop
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This register a new post-cb with the given callback and data. @cb must
 * not be NULL!.
 *
 * Returns: 0 on success, negative error code on failure.
 */
SHL_EXPORT
int ev_eloop_register_post_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			      void *data)
{
	if (!eloop)
		return -EINVAL;

	return shl_hook_add_cast(eloop->posts, cb, data, false);
}

/**
 * ev_eloop_unregister_post_cb:
 * @eloop: event loop
 * @cb: user-supplied callback
 * @data: user-supplied data
 *
 * This removes a post-cb. The arguments must be the same as for the
 * ev_eloop_register_post_cb() call. If two identical callbacks are registered,
 * then only one is removed. It doesn't matter which one is removed, because
 * they are identical.
 */
SHL_EXPORT
void ev_eloop_unregister_post_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				 void *data)
{
	if (!eloop)
		return;

	shl_hook_rm_cast(eloop->posts, cb, data);
}
