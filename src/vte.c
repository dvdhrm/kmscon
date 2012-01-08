/*
 * kmscon - VT Emulator
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
 * Virtual Terminal Emulator
 * This is a vt100 implementation. It is written from scratch. It uses the
 * console subsystem as output and is tightly bound to it.
 */

/* for pty functions */
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <pty.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/keysym.h>

#include "console.h"
#include "eloop.h"
#include "input.h"
#include "log.h"
#include "unicode.h"
#include "vte.h"

struct kmscon_vte {
	unsigned long ref;
	struct kmscon_console *con;
	struct kmscon_eloop *eloop;

	int pty;
	struct kmscon_fd *pty_fd;

	kmscon_vte_changed_cb changed_cb;
	void *changed_data;

	kmscon_vte_closed_cb closed_cb;
	void *closed_data;
};

int kmscon_vte_new(struct kmscon_vte **out,
				kmscon_vte_changed_cb changed_cb, void *data)
{
	struct kmscon_vte *vte;

	if (!out)
		return -EINVAL;

	log_debug("vte: new vte object\n");

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->pty = -1;
	vte->ref = 1;

	vte->changed_cb = changed_cb;
	vte->changed_data = data;
	*out = vte;
	return 0;
}

void kmscon_vte_ref(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

void kmscon_vte_unref(struct kmscon_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	kmscon_console_unref(vte->con);
	kmscon_vte_close(vte);
	free(vte);
	log_debug("vte: destroying vte object\n");
}

void kmscon_vte_bind(struct kmscon_vte *vte, struct kmscon_console *con)
{
	if (!vte)
		return;

	kmscon_console_unref(vte->con);
	vte->con = con;
	kmscon_console_ref(vte->con);
}

/* FIXME: this is just temporary. */
void kmscon_vte_input(struct kmscon_vte *vte, struct kmscon_input_event *ev)
{
	kmscon_symbol_t ch;
	ssize_t len;

	if (!vte || !vte->con || vte->pty < 0)
		return;

	if (ev->keysym == XK_Return)
		ch = '\n';
	else if (ev->unicode == KMSCON_INPUT_INVALID)
		return;
	else
		ch = kmscon_symbol_make(ev->unicode);

	if (ch > 127)
		return;

	if (ev->mods & KMSCON_CONTROL_MASK)
		if (iscntrl(toupper(ch) ^ 64))
			ch = toupper(ch) ^ 64;

	len = write(vte->pty, (char *)&ch, 1);
	if (len <= 0) {
		kmscon_vte_close(vte);
		return;
	}
}

void kmscon_vte_putc(struct kmscon_vte *vte, kmscon_symbol_t ch)
{
	if (!vte || !vte->con)
		return;

	if (ch == '\n')
		kmscon_console_newline(vte->con);
	else
		kmscon_console_write(vte->con, ch);
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

	_exit(EXIT_FAILURE);
}

static int fork_pty_child(int master, struct winsize *ws)
{
	int ret, saved_errno;
	pid_t pid;
	const char *slave_name;
	int slave = -1;

	/* This doesn't actually do anything on linux. */
	ret = grantpt(master);
	if (ret < 0) {
		log_err("vte: grantpt failed: %m");
		goto err_out;
	}

	ret = unlockpt(master);
	if (ret < 0) {
		log_err("vte: cannot unlock pty: %m");
		goto err_out;
	}

	slave_name = ptsname(master);
	if (!slave_name) {
		log_err("vte: cannot find pty slave name: %m");
		goto err_out;
	}

	/* This also loses our controlling tty. */
	pid = setsid();
	if (pid < 0) {
		log_err("vte: cannot start a new session: %m");
		goto err_out;
	}

	/* And the slave pty becomes our controlling tty. */
	slave = open(slave_name, O_RDWR | O_CLOEXEC);
	if (slave < 0) {
		log_err("vte: cannot open pty slave: %m");
		goto err_out;
	}

	ret = ioctl(slave, TIOCSWINSZ, ws);
	if (ret)
		log_warning("vte: cannot set slave pty window size: %m");

	if (dup2(slave, STDIN_FILENO) != STDIN_FILENO ||
			dup2(slave, STDOUT_FILENO) != STDOUT_FILENO ||
			dup2(slave, STDERR_FILENO) != STDERR_FILENO) {
		log_err("vte: cannot duplicate slave pty: %m");
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
		log_err("vte: cannot open pty master: %m");
		goto err_out;
	}

	pid = fork();
	switch (pid) {
	case -1:
		log_err("vte: failed to fork pty slave: %m");
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

static int pty_spawn(struct kmscon_vte *vte)
{
	pid_t pid;

	if (vte->pty >= 0)
		return -EALREADY;

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = kmscon_console_get_width(vte->con) ?:
							KMSCON_DEFAULT_WIDTH;
	ws.ws_row = kmscon_console_get_height(vte->con) ?:
							KMSCON_DEFAULT_HEIGHT;

	pid = fork_pty(&vte->pty, &ws);
	switch (pid) {
	case -1:
		log_err("vte: cannot fork or open pty pair: %m");
		return -errno;
	case 0:
		exec_child(vte->pty);
	default:
		break;
	}

	return 0;
}

void pty_input(struct kmscon_fd *fd, int mask, void *data)
{
	int ret, nread;
	ssize_t len, i;
	struct kmscon_vte *vte = data;

	if (!vte || vte->pty < 0)
		return;

	/*
	 * If we get a hangup or an error, but the pty is still readable, we
	 * read what's left and deal with the rest on the next dispatch.
	 */
	if (!(mask & KMSCON_READABLE)) {
		if (mask & KMSCON_ERR)
			log_warning("vte: error condition happened on pty\n");
		kmscon_vte_close(vte);
		return;
	}

	ret = ioctl(vte->pty, FIONREAD, &nread);
	if (ret) {
		log_warning("vte: cannot peek into pty input buffer: %m");
		return;
	} else if (nread <= 0) {
		return;
	}

	char buf[nread];
	len = read(vte->pty, buf, nread);
	if (len == -1) {
		/* EIO is hangup, although we should have caught it above. */
		if (errno != EIO)
			log_err("vte: cannot read from pty: %m");
		kmscon_vte_close(vte);
		return;
	} else if (len == 0) {
		kmscon_vte_close(vte);
		return;
	}

	for (i=0; i < len; i++)
		kmscon_vte_putc(vte, buf[i]);

	if (vte->changed_cb)
		vte->changed_cb(vte, vte->changed_data);
}

static int connect_eloop(struct kmscon_vte *vte, struct kmscon_eloop *eloop)
{
	int ret;

	if (vte->eloop)
		return -EALREADY;

	ret = kmscon_eloop_new_fd(eloop, &vte->pty_fd, vte->pty,
					KMSCON_READABLE, pty_input, vte);
	if (ret)
		return ret;

	kmscon_eloop_ref(eloop);
	vte->eloop = eloop;
	return 0;
}

static void disconnect_eloop(struct kmscon_vte *vte)
{
	kmscon_eloop_rm_fd(vte->pty_fd);
	kmscon_eloop_unref(vte->eloop);
	vte->pty_fd = NULL;
	vte->eloop = NULL;
}

int kmscon_vte_open(struct kmscon_vte *vte, struct kmscon_eloop *eloop,
				kmscon_vte_closed_cb closed_cb, void *data)
{
	int ret;

	if (!vte || !eloop)
		return -EINVAL;

	if (vte->pty >= 0)
		return -EALREADY;

	ret = pty_spawn(vte);
	if (ret)
		return ret;

	ret = connect_eloop(vte, eloop);
	if (ret == -EALREADY) {
		disconnect_eloop(vte);
		ret = connect_eloop(vte, eloop);
	}
	if (ret) {
		close(vte->pty);
		vte->pty = -1;
		return ret;
	}

	vte->closed_cb = closed_cb;
	vte->closed_data = data;
	return 0;
}

void kmscon_vte_close(struct kmscon_vte *vte)
{
	kmscon_vte_closed_cb cb;
	void *data;

	if (!vte || vte->pty < 0)
		return;

	disconnect_eloop(vte);

	close(vte->pty);
	vte->pty = -1;

	cb = vte->closed_cb;
	data = vte->closed_data;
	vte->closed_cb = NULL;
	vte->closed_data = NULL;

	if (cb)
		cb(vte, data);
}

void kmscon_vte_resize(struct kmscon_vte *vte)
{
	int ret;
	struct winsize ws;

	if (!vte || !vte->con || vte->pty < 0)
		return;

	memset(&ws, 0, sizeof(ws));
	ws.ws_col = kmscon_console_get_width(vte->con);
	ws.ws_row = kmscon_console_get_height(vte->con);

	/*
	 * This will send SIGWINCH to the pty slave foreground process group.
	 * We will also get one, but we don't need it.
	 */
	ret = ioctl(vte->pty, TIOCSWINSZ, &ws);
	if (ret) {
		log_warning("vte: cannot set window size\n");
		return;
	}

	log_debug("vte: window size set to %hdx%hd\n", ws.ws_col, ws.ws_row);
}
