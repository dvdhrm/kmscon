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
#include <inttypes.h>
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
#include "pty.h"
#include "shl_log.h"
#include "shl_misc.h"
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
	char *colorterm;
	char **argv;
	char *seat;
	char *vtnr;
	bool env_reset;
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

	ret = ev_eloop_new(&pty->eloop, log_llog, NULL);
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
	free(pty->vtnr);
	free(pty->seat);
	free(pty->argv);
	free(pty->colorterm);
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

int kmscon_pty_set_colorterm(struct kmscon_pty *pty, const char *colorterm)
{
	char *t;

	if (!pty || !colorterm)
		return -EINVAL;

	t = strdup(colorterm);
	if (!t)
		return -ENOMEM;
	free(pty->colorterm);
	pty->colorterm = t;

	return 0;
}

int kmscon_pty_set_argv(struct kmscon_pty *pty, char **argv)
{
	char **t;
	int ret;

	if (!pty || !argv || !*argv || !**argv)
		return -EINVAL;

	ret = shl_dup_array(&t, argv);
	if (ret)
		return ret;

	free(pty->argv);
	pty->argv = t;
	return 0;
}

int kmscon_pty_set_seat(struct kmscon_pty *pty, const char *seat)
{
	char *t;

	if (!pty || !seat)
		return -EINVAL;

	t = strdup(seat);
	if (!t)
		return -ENOMEM;
	free(pty->seat);
	pty->seat = t;

	return 0;
}

int kmscon_pty_set_vtnr(struct kmscon_pty *pty, unsigned int vtnr)
{
	char *t;
	int ret;

	if (!pty)
		return -EINVAL;

	ret = asprintf(&t, "%u", vtnr);
	if (ret < 0)
		return -ENOMEM;
	free(pty->vtnr);
	pty->vtnr = t;

	return 0;
}

void kmscon_pty_set_env_reset(struct kmscon_pty *pty, bool do_reset)
{
	if (!pty)
		return;

	pty->env_reset = do_reset;
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

static void __attribute__((noreturn))
exec_child(const char *term, const char *colorterm, char **argv,
	   const char *seat, const char *vtnr, bool env_reset)
{
	char **env;
	char **def_argv;

	if (env_reset) {
		env = malloc(sizeof(char*));
		if (!env) {
			log_error("cannot allocate memory for environment (%d): %m",
				  errno);
			exit(EXIT_FAILURE);
		}

		memset(env, 0, sizeof(char*));
		environ = env;

		def_argv = (char*[]){ "/bin/login", "-p", NULL };
	} else {
		def_argv = (char*[]){ "/bin/login", NULL };
	}

	if (!term)
		term = "vt220";
	if (!argv)
		argv = def_argv;

	setenv("TERM", term, 1);
	if (colorterm)
		setenv("COLORTERM", colorterm, 1);
	if (seat)
		setenv("XDG_SEAT", seat, 1);
	if (vtnr)
		setenv("XDG_VTNR", vtnr, 1);

	execve(argv[0], argv, environ);

	log_err("failed to exec child %s: %m", argv[0]);

	exit(EXIT_FAILURE);
}

static void setup_child(int master, struct winsize *ws)
{
	int ret;
	sigset_t sigset;
	pid_t pid;
	char slave_name[128];
	int slave = -1, i;
	struct termios attr;

	/* The child should not inherit our signal mask. */
	sigemptyset(&sigset);
	ret = pthread_sigmask(SIG_SETMASK, &sigset, NULL);
	if (ret)
		log_warn("cannot reset blocked signals: %m");

	for (i = 1; i < SIGSYS; ++i)
		signal(i, SIG_DFL);

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

	pid = fork();
	switch (pid) {
	case -1:
		log_err("cannot fork: %m");
		return -errno;
	case 0:
		setup_child(master, &ws);
		exec_child(pty->term, pty->colorterm, pty->argv, pty->seat,
			   pty->vtnr, pty->env_reset);
		exit(EXIT_FAILURE);
	default:
		log_debug("forking child %d", pid);
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

	while ((buf = shl_ring_peek(pty->msgbuf, &len, 0))) {
		ret = write(pty->fd, buf, len);
		if (ret > 0) {
			shl_ring_drop(pty->msgbuf, ret);
			continue;
		}

		if (ret < 0 && errno != EWOULDBLOCK) {
			log_warn("cannot write to child process (%d): %m",
				 errno);
			return ret;
		}

		/* EWOULDBLOCK */
		return 0;
	}

	ev_fd_update(pty->efd, EV_READABLE | EV_ET);
	return 0;
}

static int read_buf(struct kmscon_pty *pty)
{
	ssize_t len, num;
	int mask;

	/* Use a maximum of 50 steps to avoid staying here forever.
	 * TODO: recheck where else a user might flush our queues and try to
	 * install an explicit policy. */
	num = 50;
	do {
		len = read(pty->fd, pty->io_buf, sizeof(pty->io_buf));
		if (len > 0) {
			if (pty->input_cb)
				pty->input_cb(pty, pty->io_buf, len, pty->data);
		} else if (len == 0) {
			log_debug("HUP during read on pty of child %d",
				  pty->child);
			break;
		} else if (errno != EWOULDBLOCK) {
			log_debug("cannot read from pty of child %d (%d): %m",
				  pty->child, errno);
			break;
		}
	} while (len > 0 && --num);

	if (!num) {
		log_debug("cannot read application data fast enough");

		/* We are edge-triggered so update the mask to get the
		 * EV_READABLE event again next round. */
		mask = EV_READABLE | EV_ET;
		if (!shl_ring_is_empty(pty->msgbuf))
			mask |= EV_WRITEABLE;
		ev_fd_update(pty->efd, mask);
	}

	return 0;
}

static void pty_input(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_pty *pty = data;

	/* Programs like /bin/login tend to perform a vhangup() on their TTY
	 * before running the login procedure. This also causes the pty master
	 * to get a EV_HUP event as long as no client has the TTY opened.
	 * This means, we cannot use the TTY connection as reliable way to track
	 * the client. Instead, we _must_ rely on the PID of the client to track
	 * them.
	 * However, this has the side effect that if the client forks and the
	 * parent exits, we loose them and restart the client. But this seems to
	 * be the expected behavior so we implement it here.
	 * Unfortunately, epoll always polls for EPOLLHUP so as long as the
	 * vhangup() is ongoing, we will _always_ get EPOLLHUP and cannot sleep.
	 * This gets worse if the client closes the TTY but doesn't exit.
	 * Therefore, we set the fd as edge-triggered in the epoll-set so we
	 * only get the events once they change. This has to be taken into
	 * account at all places of kmscon_pty to avoid missing events. */

	if (mask & EV_ERR)
		log_warn("error on pty socket of child %d", pty->child);
	if (mask & EV_HUP)
		log_debug("HUP on pty of child %d", pty->child);
	if (mask & EV_WRITEABLE)
		send_buf(pty);
	if (mask & EV_READABLE)
		read_buf(pty);
}

static void sig_child(struct ev_eloop *eloop, struct ev_child_data *chld,
			void *data)
{
	struct kmscon_pty *pty = data;

	if (chld->pid != pty->child)
		return;

	log_info("child exited: pid: %u status: %d",
		 chld->pid, chld->status);

	pty->input_cb(pty, NULL, 0, pty->data);
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
			      EV_ET | EV_READABLE, pty_input, pty);
	if (ret)
		goto err_master;

	ret = ev_eloop_register_child_cb(pty->eloop, sig_child, pty);
	if (ret)
		goto err_fd;

	ret = pty_spawn(pty, master, width, height);
	if (ret)
		goto err_sig;

	return 0;

err_sig:
	ev_eloop_unregister_child_cb(pty->eloop, sig_child, pty);
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

	ev_eloop_rm_fd(pty->efd);
	pty->efd = NULL;
	ev_eloop_unregister_child_cb(pty->eloop, sig_child, pty);
	close(pty->fd);
	pty->fd = -1;
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

	ev_fd_update(pty->efd, EV_READABLE | EV_WRITEABLE | EV_ET);

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
}
