/*
 * kmscon - VT compatibility layer
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
 * This kmscon-VT API is based on the wayland-compositor demo:
 *
 * Copyright © 2010 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * VT compatibility
 * Although we do not use the VT for drawing or anything, we set it to graphical
 * mode to avoid side effects.
 * This behavior is copied from wayland. If someone has more insight in VT
 * handling, they may adjust this code.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>
#include "eloop.h"
#include "log.h"
#include "vt.h"

#define LOG_SUBSYSTEM "vt"

struct kmscon_vt {
	unsigned long ref;

	int fd;
	int num;
	int saved_num;
	struct termios saved_attribs;
	kmscon_vt_cb cb;
	void *data;

	struct ev_eloop *eloop;
	struct ev_fd *efd;
};

bool kmscon_vt_supported(void)
{
	if (access("/dev/tty", F_OK))
		return false;
	else
		return true;
}

int kmscon_vt_new(struct kmscon_vt **out, kmscon_vt_cb cb, void *data)
{
	struct kmscon_vt *vt;

	if (!out)
		return -EINVAL;

	vt = malloc(sizeof(*vt));
	if (!vt)
		return -ENOMEM;

	memset(vt, 0, sizeof(*vt));
	vt->ref = 1;
	vt->fd = -1;
	vt->num = -1;
	vt->saved_num = -1;
	vt->cb = cb;
	vt->data = data;

	log_debug("new vt object %p", vt);
	*out = vt;
	return 0;
}

void kmscon_vt_ref(struct kmscon_vt *vt)
{
	if (!vt)
		return;

	++vt->ref;
}

void kmscon_vt_unref(struct kmscon_vt *vt)
{
	if (!vt || !vt->ref || --vt->ref)
		return;

	log_debug("free vt object %p", vt);
	kmscon_vt_close(vt);
	free(vt);
}

static void vt_enter(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data)
{
	struct kmscon_vt *vt = data;
	struct vt_stat vts;
	int ret;

	if (!vt || vt->fd < 0)
		return;

	ret = ioctl(vt->fd, VT_GETSTATE, &vts);
	if (ret || vts.v_active != vt->num)
		return;

	log_debug("enter VT %d %p", vt->num, vt);

	ioctl(vt->fd, VT_RELDISP, VT_ACKACQ);

	if (ioctl(vt->fd, KDSETMODE, KD_GRAPHICS))
		log_warn("cannot set graphics mode on vt %p", vt);

	if (vt->cb)
		vt->cb(vt, KMSCON_VT_ENTER, vt->data);
}

static void vt_leave(struct ev_eloop *eloop, struct signalfd_siginfo *info,
			void *data)
{
	struct kmscon_vt *vt = data;
	struct vt_stat vts;
	int ret;

	if (!vt || vt->fd < 0)
		return;

	ret = ioctl(vt->fd, VT_GETSTATE, &vts);
	if (ret || vts.v_active != vt->num)
		return;

	if (vt->cb && !vt->cb(vt, KMSCON_VT_LEAVE, vt->data)) {
		log_debug("leaving VT %d %p denied", vt->num, vt);
		ioctl(vt->fd, VT_RELDISP, 0);
	} else {
		log_debug("leaving VT %d %p", vt->num, vt);
		ioctl(vt->fd, VT_RELDISP, 1);
		if (ioctl(vt->fd, KDSETMODE, KD_TEXT))
			log_warn("cannot set text mode on vt %p", vt);
	}
}

static void vt_input(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_vt *vt = data;

	if (!vt || vt->fd < 0)
		return;

	/* we ignore input from the VT because we get it from evdev */
	tcflush(vt->fd, TCIFLUSH);
}

static int connect_eloop(struct kmscon_vt *vt, struct ev_eloop *eloop)
{
	int ret;

	if (!vt || !eloop || vt->fd < 0)
		return -EINVAL;

	ret = ev_eloop_register_signal_cb(eloop, SIGUSR1, vt_leave, vt);
	if (ret)
		return ret;

	ret = ev_eloop_register_signal_cb(eloop, SIGUSR2, vt_enter, vt);
	if (ret)
		goto err_sig1;

	ret = ev_eloop_new_fd(eloop, &vt->efd, vt->fd, EV_READABLE,
				vt_input, vt);
	if (ret)
		goto err_sig2;

	vt->eloop = eloop;
	ev_eloop_ref(vt->eloop);
	return 0;

err_sig2:
	ev_eloop_unregister_signal_cb(vt->eloop, SIGUSR2, vt_enter, vt);
err_sig1:
	ev_eloop_unregister_signal_cb(vt->eloop, SIGUSR1, vt_leave, vt);
	return ret;
}

static void disconnect_eloop(struct kmscon_vt *vt)
{
	if (!vt)
		return;

	ev_eloop_rm_fd(vt->efd);
	ev_eloop_unregister_signal_cb(vt->eloop, SIGUSR2, vt_enter, vt);
	ev_eloop_unregister_signal_cb(vt->eloop, SIGUSR1, vt_leave, vt);
	ev_eloop_unref(vt->eloop);
	vt->efd = NULL;
	vt->eloop = NULL;
}

static int open_tty(int id, int *tty_fd, int *tty_num)
{
	int fd;
	char filename[16];

	if (!tty_fd || !tty_num)
		return -EINVAL;

	if (id == KMSCON_VT_NEW) {
		fd = open("/dev/tty0", O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
		if (fd < 0) {
			fd = open("/dev/tty1",
					O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
			if (fd < 0) {
				log_err("cannot find parent tty");
				return -errno;
			}
		}

		if (ioctl(fd, VT_OPENQRY, &id) || id <= 0) {
			close(fd);
			log_err("cannot get unused tty");
			return -EINVAL;
		}
		close(fd);
	}

	snprintf(filename, sizeof(filename), "/dev/tty%d", id);
	filename[sizeof(filename) - 1] = 0;
	log_notice("using tty %s", filename);

	fd = open(filename, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		log_err("cannot open tty %s", filename);
		return -errno;
	}

	*tty_fd = fd;
	*tty_num = id;
	return 0;
}

int kmscon_vt_open(struct kmscon_vt *vt, int id, struct ev_eloop *eloop)
{
	struct termios raw_attribs;
	struct vt_mode mode;
	struct vt_stat vts;
	int ret;
	sigset_t mask;

	if (vt->fd >= 0)
		return -EALREADY;

	log_debug("open vt %p", vt);

	ret = open_tty(id, &vt->fd, &vt->num);
	if (ret)
		return ret;

	ret = connect_eloop(vt, eloop);
	if (ret)
		goto err_fd;

	/*
	 * Get the number of the VT which is active now, so we have something
	 * to switch back to in kmscon_vt_switch_leave.
	 */
	ret = ioctl(vt->fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find the currently active VT");
		vt->saved_num = -1;
	} else {
		vt->saved_num = vts.v_active;
	}

	if (tcgetattr(vt->fd, &vt->saved_attribs) < 0) {
		log_err("cannot get terminal attributes");
		ret = -EFAULT;
		goto err_eloop;
	}

	/* Ignore control characters and disable echo */
	raw_attribs = vt->saved_attribs;
	cfmakeraw(&raw_attribs);

	/* Fix up line endings to be normal (cfmakeraw hoses them) */
	raw_attribs.c_oflag |= OPOST | OCRNL;

	if (tcsetattr(vt->fd, TCSANOW, &raw_attribs) < 0)
		log_warn("cannot put terminal into raw mode");

	if (ioctl(vt->fd, KDSETMODE, KD_GRAPHICS)) {
		log_err("vt: cannot set graphics mode\n");
		ret = -errno;
		goto err_reset;
	}

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_PROCESS;
	mode.relsig = SIGUSR1;
	mode.acqsig = SIGUSR2;

	if (ioctl(vt->fd, VT_SETMODE, &mode)) {
		log_err("cannot take control of vt handling");
		ret = -errno;
		goto err_text;
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	return 0;

err_text:
	ioctl(vt->fd, KDSETMODE, KD_TEXT);
err_reset:
	tcsetattr(vt->fd, TCSANOW, &vt->saved_attribs);
err_eloop:
	disconnect_eloop(vt);
err_fd:
	close(vt->fd);
	vt->fd = -1;
	return ret;
}

void kmscon_vt_close(struct kmscon_vt *vt)
{
	if (!vt || vt->fd < 0)
		return;

	log_debug("closing vt %p", vt);
	ioctl(vt->fd, KDSETMODE, KD_TEXT);
	tcsetattr(vt->fd, TCSANOW, &vt->saved_attribs);
	disconnect_eloop(vt);
	close(vt->fd);

	vt->fd = -1;
	vt->num = -1;
	vt->saved_num = -1;
}

/* Switch to this VT and make it the active VT. */
int kmscon_vt_enter(struct kmscon_vt *vt)
{
	int ret;

	if (!vt || vt->fd < 0 || vt->num < 0)
		return -EINVAL;

	ret = ioctl(vt->fd, VT_ACTIVATE, vt->num);
	if (ret) {
		log_warn("cannot enter VT %p", vt);
		return -EFAULT;
	}

	log_debug("entering VT %p on demand", vt);
	return 0;
}

/*
 * Switch back to the VT from which we started.
 * Note: The VT switch needs to be acknowledged by us so we need to react on
 * SIGUSR. This function returns -EINPROGRESS if we started the VT switch but
 * still needs to react on SIGUSR. Make sure you call the eloop dispatcher again
 * if you get -EINPROGRESS here.
 *
 * Returns 0 if we don't know the previous VT or if the previous VT is already
 * active. Returns -EINPROGRESS if we started the VT switch. Returns <0 on
 * failure.
 */
int kmscon_vt_leave(struct kmscon_vt *vt)
{
	int ret;
	struct vt_stat vts;

	if (!vt || vt->fd < 0)
		return -EINVAL;

	if (vt->saved_num < 0)
		return 0;

	ret = ioctl(vt->fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT");
		return -EFAULT;
	}

	if (vts.v_active != vt->num)
		return 0;

	ret = ioctl(vt->fd, VT_ACTIVATE, vt->saved_num);
	if (ret) {
		log_warn("cannot leave VT %p", vt);
		return -EFAULT;
	}

	log_debug("leaving VT %p on demand", vt);
	return -EINPROGRESS;
}
