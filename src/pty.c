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
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>
#include "eloop.h"
#include "log.h"
#include "pty.h"
#include "shl_ring.h"

#define LOG_SUBSYSTEM "pty"

#define KMSCON_NREAD 16384

struct kmscon_pty {
	unsigned long ref;
	struct ev_eloop *eloop;

	int fd;
	pid_t child;
	struct ev_fd *efd;
	struct shl_ring *msgbuf;
	char io_buf[KMSCON_NREAD];

	kmscon_pty_input_cb input_cb;
	void *data;

	char *term;
	char **argv;
};

int kmscon_pty_new(struct kmscon_pty **out, kmscon_pty_input_cb input_cb,
		   void *data)
{
	struct kmscon_pty *pty;
	int ret;

	if (!out || !input_cb)
		return -EINVAL;

	pty = malloc(sizeof(*pty));
	if (!pty)
		return -ENOMEM;

	memset(pty, 0, sizeof(*pty));
	pty->fd = -1;
	pty->ref = 1;
	pty->input_cb = input_cb;
	pty->data = data;

	ret = ev_eloop_new(&pty->eloop, log_llog);
	if (ret)
		goto err_free;

	ret = shl_ring_new(&pty->msgbuf);
	if (ret)
		goto err_eloop;

	log_debug("new pty object");
	*out = pty;
	return 0;

err_eloop:
	ev_eloop_unref(pty->eloop);
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
	free(pty->argv);
	free(pty->term);
	shl_ring_free(pty->msgbuf);
	ev_eloop_unref(pty->eloop);
	free(pty);
}

int kmscon_pty_set_term(struct kmscon_pty *pty, const char *term)
{
	char *t;

	if (!pty || !term)
		return -EINVAL;

	t = strdup(term);
	if (!t)
		return -ENOMEM;
	free(pty->term);
	pty->term = t;

	return 0;
}

int kmscon_pty_set_argv(struct kmscon_pty *pty, char **argv)
{
	char **t, *off;
	unsigned int size, i;

	if (!pty || !argv || !*argv || !**argv)
		return -EINVAL;

	size = 0;
	for (i = 0; argv[i]; ++i)
		size += strlen(argv[i]) + 1;
	++i;

	size += i * sizeof(char*);

	t = malloc(size);
	if (!t)
		return -ENOMEM;
	free(pty->argv);
	pty->argv = t;

	off = (char*)t + i * sizeof(char*);
	while (*argv) {
		*t++ = off;
		for (i = 0; argv[0][i]; ++i)
			*off++ = argv[0][i];
		*off++ = 0;
		argv++;
	}
	*t = NULL;

	return 0;
}

int kmscon_pty_get_fd(struct kmscon_pty *pty)
{
	if (!pty)
		return -EINVAL;

	return ev_eloop_get_fd(pty->eloop);
}

void kmscon_pty_dispatch(struct kmscon_pty *pty)
{
	if (!pty)
		return;

	ev_eloop_dispatch(pty->eloop, 0);
}

static bool pty_is_open(struct kmscon_pty *pty)
{
	return pty->fd >= 0;
}

static void sig_child(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data);

static void pty_close(struct kmscon_pty *pty, bool user)
{
	bool called = true;

	if (!pty || !pty_is_open(pty))
		return;

	if (pty->efd) {
		called = false;
		ev_eloop_rm_fd(pty->efd);
		pty->efd = NULL;
	}

	if (!user) {
		if (!called)
			pty->input_cb(pty, NULL, 0, pty->data);

		return;
	}

	ev_eloop_unregister_signal_cb(pty->eloop, SIGCHLD, sig_child, pty);
	close(pty->fd);
	pty->fd = -1;
}

static void __attribute__((noreturn))
exec_child(int pty_master, const char *term, char **argv)
{
	if (!term)
		term = "vt220";
	if (!argv)
		argv = (char*[]){ "/bin/sh", "-l", NULL };

	setenv("TERM", term, 1);
	execvp(argv[0], argv);

	log_err("failed to exec child %s: %m", argv[0]);

	exit(EXIT_FAILURE);
}

static void setup_child(int master, struct winsize *ws)
{
	int ret;
	sigset_t sigset;
	pid_t pid;
	char slave_name[128];
	int slave = -1;
	struct termios attr;

	/* The child should not inherit our signal mask. */
	sigemptyset(&sigset);
	ret = pthread_sigmask(SIG_SETMASK, &sigset, NULL);
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

	ret = ptsname_r(master, slave_name, sizeof(slave_name));
	if (ret) {
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

	/* get terminal attributes */
	if (tcgetattr(slave, &attr) < 0) {
		log_err("cannot get terminal attributes: %m");
		goto err_out;
	}

	/* erase character should be normal backspace */
	attr.c_cc[VERASE] = 010;

	/* set changed terminal attributes */
	if (tcsetattr(slave, TCSANOW, &attr) < 0) {
		log_warn("cannot set terminal attributes: %m");
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
	return;

err_out:
	ret = -errno;
	if (slave >= 0)
		close(slave);
	close(master);
	exit(EXIT_FAILURE);
}

/*
 * This is functionally equivalent to forkpty(3). We do it manually to obtain
 * a little bit more control of the process, and as a bonus avoid linking to
 * the libutil library in glibc.
 */
static int pty_spawn(struct kmscon_pty *pty, int master,
			unsigned short width, unsigned short height)
{
	pid_t pid;
	struct winsize ws;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = width;
	ws.ws_row = height;

	log_debug("forking child");
	pid = fork();
	switch (pid) {
	case -1:
		log_err("cannot fork: %m");
		return -errno;
	case 0:
		setup_child(master, &ws);
		exec_child(pty->fd, pty->term, pty->argv);
		exit(EXIT_FAILURE);
	default:
		pty->fd = master;
		pty->child = pid;
		break;
	}

	return 0;
}

static int send_buf(struct kmscon_pty *pty)
{
	const char *buf;
	size_t len;
	int ret;

	while ((buf = shl_ring_peek(pty->msgbuf, &len))) {
		ret = write(pty->fd, buf, len);
		if (ret > 0) {
			shl_ring_drop(pty->msgbuf, ret);
			continue;
		}

		if (ret < 0 && errno != EWOULDBLOCK) {
			log_warn("cannot write to child process");
			return ret;
		}

		/* EWOULDBLOCK */
		return 0;
	}

	ev_fd_update(pty->efd, EV_READABLE);
	return 0;
}

static void pty_input(struct ev_fd *fd, int mask, void *data)
{
	int ret;
	ssize_t len, num;
	struct kmscon_pty *pty = data;

	if (mask & EV_ERR) {
		log_warn("error on child pty socket");
		goto err;
	} else if (mask & EV_HUP) {
		log_debug("child closed remote end");
		goto err;
	}

	if (mask & EV_WRITEABLE) {
		ret = send_buf(pty);
		if (ret)
			goto err;
	}

	if (mask & EV_READABLE) {
		/* use a maximum of 50 steps to avoid staying here forever */
		num = 50;
		do {
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
		} while (--num && len > 0);

		if (!num)
			log_debug("cannot read application data fast enough");
	}

	return;

err:
	pty_close(pty, false);
}

static void sig_child(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data)
{
	struct kmscon_pty *pty = data;

	if (info->ssi_pid != pty->child)
		return;

	log_info("child exited: pid: %u status: %d utime: %llu stime: %llu",
			info->ssi_pid, info->ssi_status,
			info->ssi_utime, info->ssi_stime);

	pty_close(pty, false);
}

int kmscon_pty_open(struct kmscon_pty *pty, unsigned short width,
							unsigned short height)
{
	int ret;
	int master;

	if (!pty)
		return -EINVAL;

	if (pty_is_open(pty))
		return -EALREADY;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	if (master < 0) {
		log_err("cannot open master: %m");
		return -errno;
	}

	ret = ev_eloop_new_fd(pty->eloop, &pty->efd, master,
					EV_READABLE, pty_input, pty);
	if (ret)
		goto err_master;

	ret = ev_eloop_register_signal_cb(pty->eloop, SIGCHLD, sig_child, pty);
	if (ret)
		goto err_fd;

	ret = pty_spawn(pty, master, width, height);
	if (ret)
		goto err_sig;

	return 0;

err_sig:
	ev_eloop_unregister_signal_cb(pty->eloop, SIGCHLD, sig_child, pty);
err_fd:
	ev_eloop_rm_fd(pty->efd);
	pty->efd = NULL;
err_master:
	close(master);
	return ret;
}

void kmscon_pty_close(struct kmscon_pty *pty)
{
	if (!pty || !pty_is_open(pty))
		return;

	pty_close(pty, true);
}

int kmscon_pty_write(struct kmscon_pty *pty, const char *u8, size_t len)
{
	int ret;

	if (!pty || !pty_is_open(pty) || !u8 || !len)
		return -EINVAL;

	if (!shl_ring_is_empty(pty->msgbuf))
		goto buf;

	ret = write(pty->fd, u8, len);
	if (ret < 0) {
		if (errno != EWOULDBLOCK) {
			log_warn("cannot write to child process");
			return ret;
		}
	} else if (ret >= len) {
		return 0;
	} else if (ret > 0) {
		len -= ret;
		u8 = &u8[ret];
	}

	ev_fd_update(pty->efd, EV_READABLE | EV_WRITEABLE);

buf:
	ret = shl_ring_write(pty->msgbuf, u8, len);
	if (ret)
		log_warn("cannot allocate buffer; dropping output");

	return 0;
}

void kmscon_pty_signal(struct kmscon_pty *pty, int signum)
{
	int ret;

	if (!pty || !pty_is_open(pty) || signum < 0)
		return;

	ret = ioctl(pty->fd, TIOCSIG, signum);
	if (ret) {
		log_warn("cannot send signal %d to child", signum);
		return;
	}

	log_debug("send signal %d to child", signum);
}

void kmscon_pty_resize(struct kmscon_pty *pty,
			unsigned short width, unsigned short height)
{
	int ret;
	struct winsize ws;

	if (!pty || !pty_is_open(pty))
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
