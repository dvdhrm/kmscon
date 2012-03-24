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

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "conf.h"
#include "eloop.h"
#include "log.h"
#include "misc.h"
#include "pty.h"

#define LOG_SUBSYSTEM "pty"

/* Match N_TTY_BUF_SIZE from the kernel to read as much as we can. */
#define KMSCON_NREAD 4096

struct kmscon_pty {
	unsigned long ref;
	struct ev_eloop *eloop;

	int fd;
	struct ev_fd *efd;
	struct kmscon_ring *msgbuf;
	char io_buf[KMSCON_NREAD];

	kmscon_pty_input_cb input_cb;
	void *data;
};

int kmscon_pty_new(struct kmscon_pty **out, struct ev_eloop *loop,
				kmscon_pty_input_cb input_cb, void *data)
{
	struct kmscon_pty *pty;
	int ret;

	if (!out || !loop || !input_cb)
		return -EINVAL;

	pty = malloc(sizeof(*pty));
	if (!pty)
		return -ENOMEM;

	memset(pty, 0, sizeof(*pty));
	pty->fd = -1;
	pty->ref = 1;
	pty->eloop = loop;
	pty->input_cb = input_cb;
	pty->data = data;

	ret = kmscon_ring_new(&pty->msgbuf);
	if (ret)
		goto err_free;

	log_debug("new pty object");
	ev_eloop_ref(pty->eloop);
	*out = pty;
	return 0;

err_free:
	free(pty);
	return ret;
}

void kmscon_pty_ref(struct kmscon_pty *pty)
{
	if (!pty)
		return;

	pty->ref++;
}

void kmscon_pty_unref(struct kmscon_pty *pty)
{
	if (!pty || !pty->ref || --pty->ref)
		return;

	log_debug("free pty object");
	kmscon_pty_close(pty);
	kmscon_ring_free(pty->msgbuf);
	ev_eloop_unref(pty->eloop);
	free(pty);
}

/*
 * TODO:
 *   - Decide which terminal we're emulating and set TERM accordingly.
 *   - Decide what to exec here: login, some getty equivalent, a shell...
 *   - Might also need to update some details in utmp wtmp and friends.
 */
static void __attribute__((noreturn))
exec_child(int pty_master)
{
	setenv("TERM", "linux", 1);
	execvp(conf_global.login, conf_global.argv);

	log_err("failed to exec child: %m");

	_exit(EXIT_FAILURE);
}

static int setup_child(int master, struct winsize *ws)
{
	int ret;
	sigset_t sigset;
	pid_t pid;
	const char *slave_name;
	int slave = -1;

	/* The child should not inherit our signal mask. */
	sigemptyset(&sigset);
	ret = sigprocmask(SIG_SETMASK, &sigset, NULL);
	if (ret)
		log_warn("cannot reset blocked signals: %m");

	ret = grantpt(master);
	if (ret < 0) {
		log_err("grantpt failed: %m");
		goto err_out;
	}

	ret = unlockpt(master);
	if (ret < 0) {
		log_err("cannot unlock pty: %m");
		goto err_out;
	}

	slave_name = ptsname(master);
	if (!slave_name) {
		log_err("cannot find slave name: %m");
		goto err_out;
	}

	/* This also loses our controlling tty. */
	pid = setsid();
	if (pid < 0) {
		log_err("cannot start a new session: %m");
		goto err_out;
	}

	/* And the slave pty becomes our controlling tty. */
	slave = open(slave_name, O_RDWR | O_CLOEXEC);
	if (slave < 0) {
		log_err("cannot open slave: %m");
		goto err_out;
	}

	if (ws) {
		ret = ioctl(slave, TIOCSWINSZ, ws);
		if (ret)
			log_warn("cannot set slave window size: %m");
	}

	if (dup2(slave, STDIN_FILENO) != STDIN_FILENO ||
			dup2(slave, STDOUT_FILENO) != STDOUT_FILENO ||
			dup2(slave, STDERR_FILENO) != STDERR_FILENO) {
		log_err("cannot duplicate slave: %m");
		goto err_out;
	}

	close(master);
	close(slave);
	return 0;

err_out:
	ret = -errno;
	if (slave > 0)
		close(slave);
	close(master);
	return ret;
}

/*
 * This is functionally equivalent to forkpty(3). We do it manually to obtain
 * a little bit more control of the process, and as a bonus avoid linking to
 * the libutil library in glibc.
 */
static int pty_spawn(struct kmscon_pty *pty, unsigned short width,
							unsigned short height)
{
	int ret;
	pid_t pid;
	int master;
	struct winsize ws;

	if (pty->fd >= 0)
		return -EALREADY;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = width;
	ws.ws_row = height;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	if (master < 0) {
		log_err("cannot open master: %m");
		return -errno;
	}

	log_debug("forking child");
	pid = fork();
	switch (pid) {
	case -1:
		log_err("cannot fork: %m");
		ret = -errno;
		goto err_master;
	case 0:
		ret = setup_child(master, &ws);
		if (ret)
			goto err_master;
		exec_child(pty->fd);
		abort();
	default:
		pty->fd = master;
		break;
	}

	return 0;

err_master:
	close(master);
	return ret;
}

static int send_buf(struct kmscon_pty *pty)
{
	const char *buf;
	size_t len;
	int ret;

	while ((buf = kmscon_ring_peek(pty->msgbuf, &len))) {
		ret = write(pty->fd, buf, len);
		if (ret > 0) {
			kmscon_ring_drop(pty->msgbuf, ret);
			continue;
		}

		if (ret < 0 && errno != EWOULDBLOCK) {
			log_warn("cannot write to child process");
			return ret;
		}

		/* EWOULDBLOCK */
		return 0;
	}

	ev_eloop_update_fd(pty->efd, EV_READABLE);
	return 0;
}

static void pty_input(struct ev_fd *fd, int mask, void *data)
{
	int ret;
	ssize_t len;
	struct kmscon_pty *pty = data;

	if (!pty || pty->fd < 0)
		return;

	if (mask & (EV_ERR | EV_HUP)) {
		if (mask & EV_ERR)
			log_warn("error on child pty socket");
		else
			log_debug("child closed remote end");

		goto err;
	}

	if (mask & EV_WRITEABLE) {
		ret = send_buf(pty);
		if (ret)
			goto err;
	}

	if (mask & EV_READABLE) {
		len = read(pty->fd, pty->io_buf, sizeof(pty->io_buf));
		if (len > 0) {
			if (pty->input_cb)
				pty->input_cb(pty, pty->io_buf, len, pty->data);
		} else if (len == 0) {
			log_debug("child closed remote end");
			goto err;
		} else if (errno != EWOULDBLOCK) {
			log_err("cannot read from pty: %m");
			goto err;
		}
	}

	return;

err:
	ev_eloop_rm_fd(pty->efd);
	pty->efd = NULL;
	if (pty->input_cb)
		pty->input_cb(pty, NULL, 0, pty->data);
}

int kmscon_pty_open(struct kmscon_pty *pty, unsigned short width,
							unsigned short height)
{
	int ret;

	if (!pty)
		return -EINVAL;

	if (pty->fd >= 0)
		return -EALREADY;

	ret = pty_spawn(pty, width, height);
	if (ret)
		return ret;

	ret = ev_eloop_new_fd(pty->eloop, &pty->efd, pty->fd,
					EV_READABLE, pty_input, pty);
	if (ret) {
		close(pty->fd);
		pty->fd = -1;
		return ret;
	}

	return 0;
}

void kmscon_pty_close(struct kmscon_pty *pty)
{
	if (!pty || pty->fd < 0)
		return;

	ev_eloop_rm_fd(pty->efd);
	pty->efd = NULL;
	close(pty->fd);
	pty->fd = -1;
}

int kmscon_pty_write(struct kmscon_pty *pty, const char *u8, size_t len)
{
	int ret;

	if (!pty || pty->fd < 0 || !u8 || !len)
		return -EINVAL;

	if (!kmscon_ring_is_empty(pty->msgbuf))
		goto buf;

	ret = write(pty->fd, u8, len);
	if (ret < 0) {
		if (errno != EWOULDBLOCK) {
			log_warn("cannot write to child process");
			return ret;
		}
	} else if (ret == len) {
		return 0;
	} else if (ret > 0) {
		len -= ret;
		u8 = &u8[ret];
	}

	ev_eloop_update_fd(pty->efd, EV_READABLE | EV_WRITEABLE);

buf:
	ret = kmscon_ring_write(pty->msgbuf, u8, len);
	if (ret)
		log_warn("cannot allocate buffer; dropping input");

	return 0;
}

void kmscon_pty_resize(struct kmscon_pty *pty,
			unsigned short width, unsigned short height)
{
	int ret;
	struct winsize ws;

	if (!pty || pty->fd < 0)
		return;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = width;
	ws.ws_row = height;

	/*
	 * This will send SIGWINCH to the pty slave foreground process group.
	 * We will also get one, but we don't need it.
	 */
	ret = ioctl(pty->fd, TIOCSWINSZ, &ws);
	if (ret) {
		log_warn("cannot set window size");
		return;
	}

	log_debug("window size set to %hdx%hd", ws.ws_col, ws.ws_row);
}
