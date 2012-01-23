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

/* for pty functions */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "log.h"
#include "pty.h"

struct kmscon_pty {
	unsigned long ref;
	struct kmscon_eloop *eloop;

	int fd;
	struct kmscon_fd *efd;

	kmscon_pty_output_cb output_cb;
	void *output_data;

	kmscon_pty_closed_cb closed_cb;
	void *closed_data;
};

int kmscon_pty_new(struct kmscon_pty **out,
				kmscon_pty_output_cb output_cb, void *data)
{
	struct kmscon_pty *pty;

	if (!out)
		return -EINVAL;

	log_debug("pty: new pty object\n");

	pty = malloc(sizeof(*pty));
	if (!pty)
		return -ENOMEM;

	memset(pty, 0, sizeof(*pty));
	pty->fd = -1;
	pty->ref = 1;

	pty->output_cb = output_cb;
	pty->output_data = data;
	*out = pty;
	return 0;
}

void kmscon_pty_ref(struct kmscon_pty *pty)
{
	if (!pty)
		return;

	pty->ref++;
}

void kmscon_pty_unref(struct kmscon_pty *pty)
{
	if (!pty || !pty->ref)
		return;

	if (--pty->ref)
		return;

	kmscon_pty_close(pty);
	free(pty);
	log_debug("pty: destroying pty object\n");
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
	const char *sh;

	setenv("TERM", "linux", 1);

	sh = getenv("SHELL") ?: _PATH_BSHELL;
	execlp(sh, sh, "-i", NULL);

	log_err("pty: failed to exec child: %m\n");

	_exit(EXIT_FAILURE);
}

static int fork_pty_child(int master, struct winsize *ws)
{
	int ret, saved_errno;
	sigset_t sigset;
	pid_t pid;
	const char *slave_name;
	int slave = -1;

	/* The child should not inherit our signal mask. */
	sigemptyset(&sigset);
	ret = sigprocmask(SIG_SETMASK, &sigset, NULL);
	if (ret)
		log_warn("pty: cannot reset blocked signals: %m\n");

	/* This doesn't actually do anything on linux. */
	ret = grantpt(master);
	if (ret < 0) {
		log_err("pty: grantpt failed: %m");
		goto err_out;
	}

	ret = unlockpt(master);
	if (ret < 0) {
		log_err("pty: cannot unlock pty: %m");
		goto err_out;
	}

	slave_name = ptsname(master);
	if (!slave_name) {
		log_err("pty: cannot find slave name: %m");
		goto err_out;
	}

	/* This also loses our controlling tty. */
	pid = setsid();
	if (pid < 0) {
		log_err("pty: cannot start a new session: %m");
		goto err_out;
	}

	/* And the slave pty becomes our controlling tty. */
	slave = open(slave_name, O_RDWR | O_CLOEXEC);
	if (slave < 0) {
		log_err("pty: cannot open slave: %m");
		goto err_out;
	}

	if (ws) {
		ret = ioctl(slave, TIOCSWINSZ, ws);
		if (ret)
			log_warn("pty: cannot set slave window size: %m");
	}

	if (dup2(slave, STDIN_FILENO) != STDIN_FILENO ||
			dup2(slave, STDOUT_FILENO) != STDOUT_FILENO ||
			dup2(slave, STDERR_FILENO) != STDERR_FILENO) {
		log_err("pty: cannot duplicate slave: %m");
		goto err_out;
	}

	close(master);
	close(slave);
	return 0;

err_out:
	saved_errno = errno;
	if (slave > 0)
		close(slave);
	close(master);
	return -saved_errno;
}

/*
 * This is functionally equivalent to forkpty(3). We do it manually to obtain
 * a little bit more control of the process, and as a bonus avoid linking to
 * the libutil library in glibc.
 */
static pid_t fork_pty(int *pty_out, struct winsize *ws)
{
	int ret;
	pid_t pid;
	int master;

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
	if (master < 0) {
		ret = -errno;
		log_err("pty: cannot open master: %m");
		goto err_out;
	}

	pid = fork();
	switch (pid) {
	case -1:
		log_err("pty: cannot fork: %m");
		ret = -errno;
		goto err_master;
	case 0:
		ret = fork_pty_child(master, ws);
		if (ret)
			goto err_master;
		*pty_out = -1;
		return 0;
	default:
		*pty_out = master;
		return pid;
	}

err_master:
	close(master);
err_out:
	*pty_out = -1;
	errno = -ret;
	return -1;
}

static int pty_spawn(struct kmscon_pty *pty,
			unsigned short width, unsigned short height)
{
	struct winsize ws;
	pid_t pid;

	if (pty->fd >= 0)
		return -EALREADY;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = width;
	ws.ws_row = height;

	pid = fork_pty(&pty->fd, &ws);
	switch (pid) {
	case -1:
		log_err("pty: cannot fork or open pty pair: %m");
		return -errno;
	case 0:
		exec_child(pty->fd);
	default:
		break;
	}

	return 0;
}

static void pty_output(struct kmscon_fd *fd, int mask, void *data)
{
	int ret, nread;
	ssize_t len;
	struct kmscon_pty *pty = data;

	if (!pty || pty->fd < 0)
		return;

	/*
	 * If we get a hangup or an error, but the pty is still readable, we
	 * read what's left and deal with the rest on the next dispatch.
	 */
	if (!(mask & KMSCON_READABLE)) {
		if (mask & KMSCON_ERR)
			log_warn("pty: error condition happened on pty\n");
		kmscon_pty_close(pty);
		return;
	}

	ret = ioctl(pty->fd, FIONREAD, &nread);
	if (ret) {
		log_warn("pty: cannot peek into pty input buffer: %m");
		return;
	} else if (nread <= 0) {
		return;
	}

	char u8[nread];
	len = read(pty->fd, u8, nread);
	if (len == -1) {
		if (errno == EWOULDBLOCK)
			return;
		/* EIO is hangup, although we should have caught it above. */
		if (errno != EIO)
			log_err("pty: cannot read from pty: %m");
		kmscon_pty_close(pty);
		return;
	} else if (len == 0) {
		kmscon_pty_close(pty);
		return;
	}

	if (pty->output_cb)
		pty->output_cb(pty, u8, len, pty->output_data);
}

static int connect_eloop(struct kmscon_pty *pty, struct kmscon_eloop *eloop)
{
	int ret;

	if (pty->eloop)
		return -EALREADY;

	ret = kmscon_eloop_new_fd(eloop, &pty->efd, pty->fd,
					KMSCON_READABLE, pty_output, pty);
	if (ret)
		return ret;

	kmscon_eloop_ref(eloop);
	pty->eloop = eloop;
	return 0;
}

static void disconnect_eloop(struct kmscon_pty *pty)
{
	kmscon_eloop_rm_fd(pty->efd);
	kmscon_eloop_unref(pty->eloop);
	pty->efd = NULL;
	pty->eloop = NULL;
}

int kmscon_pty_open(struct kmscon_pty *pty, struct kmscon_eloop *eloop,
				unsigned short width, unsigned short height,
				kmscon_pty_closed_cb closed_cb, void *data)
{
	int ret;

	if (!pty || !eloop)
		return -EINVAL;

	if (pty->fd >= 0)
		return -EALREADY;

	ret = pty_spawn(pty, width, height);
	if (ret)
		return ret;

	ret = connect_eloop(pty, eloop);
	if (ret == -EALREADY) {
		disconnect_eloop(pty);
		ret = connect_eloop(pty, eloop);
	}
	if (ret) {
		close(pty->fd);
		pty->fd = -1;
		return ret;
	}

	pty->closed_cb = closed_cb;
	pty->closed_data = data;
	return 0;
}

void kmscon_pty_close(struct kmscon_pty *pty)
{
	kmscon_pty_closed_cb cb;
	void *data;

	if (!pty || pty->fd < 0)
		return;

	disconnect_eloop(pty);

	close(pty->fd);
	pty->fd = -1;

	cb = pty->closed_cb;
	data = pty->closed_data;
	pty->closed_cb = NULL;
	pty->closed_data = NULL;

	if (cb)
		cb(pty, data);
}

void kmscon_pty_input(struct kmscon_pty *pty, const char *u8, size_t len)
{
	if (!pty || pty->fd < 0)
		return;

	/* FIXME: In EWOULDBLOCK we would lose input! Need to buffer. */
	len = write(pty->fd, u8, len);
	if (len <= 0) {
		if (errno != EWOULDBLOCK)
			kmscon_pty_close(pty);
		return;
	}
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
		log_warn("pty: cannot set window size\n");
		return;
	}

	log_debug("pty: window size set to %hdx%hd\n", ws.ws_col, ws.ws_row);
}
