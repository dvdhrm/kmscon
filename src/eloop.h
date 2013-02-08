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

/*
 * Event Loop
 * This provides a basic event loop similar to those provided by glib etc.
 * It uses linux specific features like signalfd so it may not be easy to port
 * it to other platforms.
 */

#ifndef EV_ELOOP_H
#define EV_ELOOP_H

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <time.h>

struct ev_eloop;
struct ev_fd;
struct ev_timer;
struct ev_counter;

/**
 * ev_log_t:
 * @data: User provided data field
 * @file: Source code file where the log message originated or NULL
 * @line: Line number in source code or 0
 * @func: C function name or NULL
 * @subs: Subsystem where the message came from or NULL
 * @sev: Kernel-style severity between 0=FATAL and 7=DEBUG
 * @format: printf-formatted message
 * @args: arguments for printf-style @format
 *
 * This is the type of a logging callback function. You can always pass NULL
 * instead of such a function to disable logging.
 */
typedef void (*ev_log_t) (void *data,
			  const char *file,
			  int line,
			  const char *func,
			  const char *subs,
			  unsigned int sev,
			  const char *format,
			  va_list args);

/**
 * ev_fd_cb:
 * @fd: File descriptor event source
 * @mask: Mask of @EV_READABLE, @EV_WRITEABLE, etc.
 * @data: user-supplied data
 *
 * This is the callback-type for file-descriptor event sources.
 */
typedef void (*ev_fd_cb) (struct ev_fd *fd, int mask, void *data);

/**
 * ev_timer_cb:
 * @timer: Timer source
 * @num: Number of times the timer fired since last wakeup
 * @data: user-supplied data
 *
 * This is the callback-type for timer event sources. If the process was too
 * busy to be woken up as fast as possible, then @num may be bigger than 1.
 */
typedef void (*ev_timer_cb)
			(struct ev_timer *timer, uint64_t num, void *data);

/**
 * ev_counter_cb:
 * @cnt: Counter source
 * @num: Current counter state
 * @data: user-supplied data
 *
 * This is the callback-type for counter event sources. @num is at least 1 but
 * may be bigger if the timer was increased multiple times or by bigger values.
 * The counter is reset to 0 before this callback is called.
 */
typedef void (*ev_counter_cb)
			(struct ev_counter *cnt, uint64_t num, void *data);

/**
 * ev_signal_shared_cb:
 * @eloop: event loop object
 * @info: signalfd info for this event, see "man signalfd"
 * @data: user-supplied data
 *
 * This is the callback-type for shared signal events.
 */
typedef void (*ev_signal_shared_cb)
	(struct ev_eloop *eloop, struct signalfd_siginfo *info, void *data);

/**
 * ev_child_data:
 * @pid: pid of child
 * @status: status of child
 *
 * This contains the payload for @ev_child_cb events. The data is simply copied
 * from the waitpid() system-call.
 */
struct ev_child_data {
	pid_t pid;
	int status;
};

/**
 * ev_child_cb:
 * @eloop: event loop object
 * @chld: chld pid/status payload
 * @data: user-supplied data
 *
 * This is the callback-type for child-reaper events.
 */
typedef void (*ev_child_cb)
	(struct ev_eloop *eloop, struct ev_child_data *chld, void *data);

/**
 * ev_idle_cb:
 * @eloop: event loop object
 * @unused: Unused parameter which is required internally
 * @data: user-supplied data
 *
 * This is the callback-type for idle-source events.
 */
typedef void (*ev_idle_cb) (struct ev_eloop *eloop, void *unused, void *data);

/**
 * ev_eloop_flags:
 * @EV_READABLE: file-descriptor is readable
 * @EV_WRITEABLE: file-desciprotr is writeable
 * @EV_HUP: Hang-up on file-descriptor
 * @EV_ERR: I/O error on file-descriptor
 * @EV_ET: Edge-triggered mode
 *
 * These flags are used for events on file-descriptors. You can combine them
 * with binary-operators like @EV_READABLE | @EV_WRITEABLE.
 * @EV_HUP and @EV_ERR are always raised for file-descriptors, even if not
 * requested explicitly.
 * @EV_ET enables edge-triggered mode for the operation
 */
enum ev_eloop_flags {
	EV_READABLE = 0x01,
	EV_WRITEABLE = 0x02,
	EV_HUP = 0x04,
	EV_ERR = 0x08,
	EV_ET = 0x10,
};

int ev_eloop_new(struct ev_eloop **out, ev_log_t log, void *log_data);
void ev_eloop_ref(struct ev_eloop *loop);
void ev_eloop_unref(struct ev_eloop *loop);

void ev_eloop_flush_fd(struct ev_eloop *loop, struct ev_fd *fd);
int ev_eloop_dispatch(struct ev_eloop *loop, int timeout);
int ev_eloop_run(struct ev_eloop *loop, int timeout);
void ev_eloop_exit(struct ev_eloop *loop);
int ev_eloop_get_fd(struct ev_eloop *loop);

/* eloop sources */

int ev_eloop_new_eloop(struct ev_eloop *loop, struct ev_eloop **out);
int ev_eloop_add_eloop(struct ev_eloop *loop, struct ev_eloop *add);
void ev_eloop_rm_eloop(struct ev_eloop *rm);

/* fd sources */

int ev_fd_new(struct ev_fd **out, int fd, int mask, ev_fd_cb cb, void *data,
	      ev_log_t log, void *log_data);
void ev_fd_ref(struct ev_fd *fd);
void ev_fd_unref(struct ev_fd *fd);

int ev_fd_enable(struct ev_fd *fd);
void ev_fd_disable(struct ev_fd *fd);
bool ev_fd_is_enabled(struct ev_fd *fd);
bool ev_fd_is_bound(struct ev_fd *fd);
void ev_fd_set_cb_data(struct ev_fd *fd, ev_fd_cb cb, void *data);
int ev_fd_update(struct ev_fd *fd, int mask);

int ev_eloop_new_fd(struct ev_eloop *loop, struct ev_fd **out, int rfd,
			int mask, ev_fd_cb cb, void *data);
int ev_eloop_add_fd(struct ev_eloop *loop, struct ev_fd *fd);
void ev_eloop_rm_fd(struct ev_fd *fd);

/* timer sources */

int ev_timer_new(struct ev_timer **out, const struct itimerspec *spec,
		 ev_timer_cb cb, void *data, ev_log_t log, void *log_data);
void ev_timer_ref(struct ev_timer *timer);
void ev_timer_unref(struct ev_timer *timer);

int ev_timer_enable(struct ev_timer *timer);
void ev_timer_disable(struct ev_timer *timer);
bool ev_timer_is_enabled(struct ev_timer *timer);
bool ev_timer_is_bound(struct ev_timer *timer);
void ev_timer_set_cb_data(struct ev_timer *timer, ev_timer_cb cb, void *data);
int ev_timer_update(struct ev_timer *timer, const struct itimerspec *spec);
int ev_timer_drain(struct ev_timer *timer, uint64_t *expirations);

int ev_eloop_new_timer(struct ev_eloop *loop, struct ev_timer **out,
			const struct itimerspec *spec, ev_timer_cb cb,
			void *data);
int ev_eloop_add_timer(struct ev_eloop *loop, struct ev_timer *timer);
void ev_eloop_rm_timer(struct ev_timer *timer);

/* counter sources */

int ev_counter_new(struct ev_counter **out, ev_counter_cb, void *data,
		   ev_log_t log, void *log_data);
void ev_counter_ref(struct ev_counter *cnt);
void ev_counter_unref(struct ev_counter *cnt);

int ev_counter_enable(struct ev_counter *cnt);
void ev_counter_disable(struct ev_counter *cnt);
bool ev_counter_is_enabled(struct ev_counter *cnt);
bool ev_counter_is_bound(struct ev_counter *cnt);
void ev_counter_set_cb_data(struct ev_counter *cnt, ev_counter_cb cb,
			    void *data);
int ev_counter_inc(struct ev_counter *cnt, uint64_t val);

int ev_eloop_new_counter(struct ev_eloop *eloop, struct ev_counter **out,
			 ev_counter_cb cb, void *data);
int ev_eloop_add_counter(struct ev_eloop *eloop, struct ev_counter *cnt);
void ev_eloop_rm_counter(struct ev_counter *cnt);

/* signal sources */

int ev_eloop_register_signal_cb(struct ev_eloop *loop, int signum,
				ev_signal_shared_cb cb, void *data);
void ev_eloop_unregister_signal_cb(struct ev_eloop *loop, int signum,
					ev_signal_shared_cb cb, void *data);

/* child reaper sources */

int ev_eloop_register_child_cb(struct ev_eloop *loop, ev_child_cb cb,
			       void *data);
void ev_eloop_unregister_child_cb(struct ev_eloop *loop, ev_child_cb cb,
				  void *data);

/* idle sources */

enum ev_idle_flags {
	EV_NORMAL	= 0x00,
	EV_ONESHOT	= 0x01,
	EV_SINGLE	= 0x02,
	EV_IDLE_ALL	= EV_ONESHOT | EV_SINGLE,
};

int ev_eloop_register_idle_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			      void *data, unsigned int flags);
void ev_eloop_unregister_idle_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				 void *data, unsigned int flags);

/* pre dispatch callbacks */

int ev_eloop_register_pre_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			     void *data);
void ev_eloop_unregister_pre_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				void *data);

/* post dispatch callbacks */

int ev_eloop_register_post_cb(struct ev_eloop *eloop, ev_idle_cb cb,
			      void *data);
void ev_eloop_unregister_post_cb(struct ev_eloop *eloop, ev_idle_cb cb,
				 void *data);

#endif /* EV_ELOOP_H */
