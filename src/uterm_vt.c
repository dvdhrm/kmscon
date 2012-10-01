/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Virtual Terminals
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
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "eloop.h"
#include "log.h"
#include "shl_dlist.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "vt"

struct uterm_vt {
	unsigned long ref;
	struct shl_dlist list;
	struct uterm_vt_master *vtm;
	struct uterm_input *input;
	unsigned int mode;

	uterm_vt_cb cb;
	void *data;

	bool active;

	/* this is for *real* linux kernel VTs */
	int real_fd;
	int real_num;
	int real_saved_num;
	struct termios real_saved_attribs;
	struct ev_fd *real_efd;
};

struct uterm_vt_master {
	unsigned long ref;
	struct ev_eloop *eloop;

	bool vt_support;
	struct shl_dlist vts;
};

static int vt_call(struct uterm_vt *vt, unsigned int event)
{
	int ret;

	switch (event) {
	case UTERM_VT_ACTIVATE:
		if (!vt->active) {
			if (vt->cb) {
				ret = vt->cb(vt, event, vt->data);
				if (ret)
					log_warning("vt event handler returned %d instead of 0 on activation", ret);
			}
			vt->active = true;
		}
		break;
	case UTERM_VT_DEACTIVATE:
		if (vt->active) {
			if (vt->cb) {
				ret = vt->cb(vt, event, vt->data);
				if (ret)
					return ret;
			}
			vt->active = false;
		}
		break;
	}

	return 0;
}

static void real_enter(struct uterm_vt *vt, struct signalfd_siginfo *info)
{
	struct vt_stat vts;
	int ret;

	if (info->ssi_code != SI_KERNEL)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret || vts.v_active != vt->real_num)
		return;

	log_debug("enter VT %d %p", vt->real_num, vt);

	ioctl(vt->real_fd, VT_RELDISP, VT_ACKACQ);

	if (ioctl(vt->real_fd, KDSETMODE, KD_GRAPHICS))
		log_warn("cannot set graphics mode on vt %p", vt);

	vt_call(vt, UTERM_VT_ACTIVATE);
}

static void real_leave(struct uterm_vt *vt, struct signalfd_siginfo *info)
{
	struct vt_stat vts;
	int ret;

	if (info->ssi_code != SI_KERNEL)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret || vts.v_active != vt->real_num)
		return;

	if (vt_call(vt, UTERM_VT_DEACTIVATE)) {
		log_debug("leaving VT %d %p denied", vt->real_num, vt);
		ioctl(vt->real_fd, VT_RELDISP, 0);
	} else {
		log_debug("leaving VT %d %p", vt->real_num, vt);
		ioctl(vt->real_fd, VT_RELDISP, 1);
		if (ioctl(vt->real_fd, KDSETMODE, KD_TEXT))
			log_warn("cannot set text mode on vt %p", vt);
	}
}

static void real_input(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_vt *vt = data;

	/* we ignore input from the VT because we get it from evdev */
	tcflush(vt->real_fd, TCIFLUSH);
}

static int open_tty(const char *dev, int *tty_fd, int *tty_num)
{
	int fd, err1, id, ret;
	char filename[16];
	struct stat st;

	if (!tty_fd || !tty_num)
		return -EINVAL;

	if (!dev) {
		fd = open("/dev/tty0", O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
		if (fd < 0) {
			err1 = errno;
			fd = open("/dev/tty1",
					O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
			if (fd < 0) {
				log_err("cannot find parent tty (%d, %d): %m",
					err1, errno);
				return -errno;
			}
		}

		if (ioctl(fd, VT_OPENQRY, &id) || id <= 0) {
			close(fd);
			log_err("cannot get unused tty");
			return -EINVAL;
		}
		close(fd);

		snprintf(filename, sizeof(filename), "/dev/tty%d", id);
		filename[sizeof(filename) - 1] = 0;
		dev = filename;
	}

	log_notice("using tty %s", dev);

	fd = open(dev, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		log_err("cannot open tty %s", dev);
		return -errno;
	}

	ret = fstat(fd, &st);
	if (ret) {
		log_error("cannot introspect tty %s (%d): %m", dev, errno);
		close(fd);
		return -errno;
	}
	id = minor(st.st_rdev);

	*tty_fd = fd;
	*tty_num = id;
	return 0;
}

static int real_open(struct uterm_vt *vt, const char *vt_for_seat0)
{
	struct termios raw_attribs;
	struct vt_mode mode;
	struct vt_stat vts;
	int ret;
	sigset_t mask;

	log_debug("open vt %p", vt);

	ret = open_tty(vt_for_seat0, &vt->real_fd, &vt->real_num);
	if (ret)
		return ret;

	ret = ev_eloop_new_fd(vt->vtm->eloop, &vt->real_efd, vt->real_fd,
			      EV_READABLE, real_input, vt);
	if (ret)
		goto err_fd;

	/*
	 * Get the number of the VT which is active now, so we have something
	 * to switch back to in kmscon_vt_switch_leave.
	 */
	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find the currently active VT");
		vt->real_saved_num = -1;
	} else {
		vt->real_saved_num = vts.v_active;
	}

	if (tcgetattr(vt->real_fd, &vt->real_saved_attribs) < 0) {
		log_err("cannot get terminal attributes");
		ret = -EFAULT;
		goto err_eloop;
	}

	/* Ignore control characters and disable echo */
	raw_attribs = vt->real_saved_attribs;
	cfmakeraw(&raw_attribs);

	/* Fix up line endings to be normal (cfmakeraw hoses them) */
	raw_attribs.c_oflag |= OPOST | OCRNL;

	if (tcsetattr(vt->real_fd, TCSANOW, &raw_attribs) < 0)
		log_warn("cannot put terminal into raw mode");

	if (ioctl(vt->real_fd, KDSETMODE, KD_GRAPHICS)) {
		log_err("vt: cannot set graphics mode");
		ret = -errno;
		goto err_reset;
	}

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_PROCESS;
	mode.acqsig = SIGUSR1;
	mode.relsig = SIGUSR2;

	if (ioctl(vt->real_fd, VT_SETMODE, &mode)) {
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
	ioctl(vt->real_fd, KDSETMODE, KD_TEXT);
err_reset:
	tcsetattr(vt->real_fd, TCSANOW, &vt->real_saved_attribs);
err_eloop:
	ev_eloop_rm_fd(vt->real_efd);
	vt->real_efd = NULL;
err_fd:
	close(vt->real_fd);
	return ret;
}

static void real_close(struct uterm_vt *vt)
{
	struct vt_mode mode;

	log_debug("closing vt %p", vt);

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_AUTO;
	ioctl(vt->real_fd, VT_SETMODE, &mode);

	ioctl(vt->real_fd, KDSETMODE, KD_TEXT);
	tcsetattr(vt->real_fd, TCSANOW, &vt->real_saved_attribs);
	ev_eloop_rm_fd(vt->real_efd);
	vt->real_efd = NULL;
	close(vt->real_fd);

	vt->real_fd = -1;
	vt->real_num = -1;
	vt->real_saved_num = -1;
}

/* Switch to this VT and make it the active VT. If we are already the active
 * VT, then 0 is returned, if the VT_ACTIVATE ioctl is called to activate this
 * VT, then -EINPROGRESS is returned and we will be activated when receiving the
 * VT switch signal. The currently active VT may prevent this, though.
 * On error a negative error code is returned other than -EINPROGRESS */
static int real_activate(struct uterm_vt *vt)
{
	int ret;
	struct vt_stat vts;

	if (vt->real_num < 0)
		return -EINVAL;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT");
		return -EFAULT;
	}

	if (vts.v_active != vt->real_num)
		return 0;

	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_num);
	if (ret) {
		log_warn("cannot enter VT %p", vt);
		return -EFAULT;
	}

	log_debug("entering VT %p on demand", vt);
	return -EINPROGRESS;
}

/*
 * Switch back to the VT from which we started.
 * Note: The VT switch needs to be acknowledged by us so we need to react on
 * SIGUSR. This function returns -EINPROGRESS if we started the VT switch but
 * still needs to react on SIGUSR. Make sure you call the eloop dispatcher again
 * if you get -EINPROGRESS here.
 *
 * Returns 0 if the previous VT is already active.
 * Returns -EINPROGRESS if we started the VT switch. Returns <0 on failure.
 */
static int real_deactivate(struct uterm_vt *vt)
{
	int ret;
	struct vt_stat vts;

	if (vt->real_saved_num < 0)
		return -EINVAL;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT");
		return -EFAULT;
	}

	if (vts.v_active != vt->real_num)
		return 0;

	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_saved_num);
	if (ret) {
		log_warn("cannot leave VT %p", vt);
		return -EFAULT;
	}

	log_debug("leaving VT %p on demand", vt);
	return -EINPROGRESS;
}

/*
 * Fake VT:
 * For systems without CONFIG_VT or for all seats that have no real VTs (which
 * is all seats except seat0), we support a fake-VT mechanism. This machanism is
 * only used for debugging and should not be used in production.
 * The Fake-VT reacts on SIGUSR1 and SIGUSR2 similar to the real-vt and
 * activates or deactivates the VT. However, this is a global mechanism as all
 * fake VTs listen to the same signals. Therefore, this is not really multi-seat
 * safe.
 * TODO: Replace this with a proper multi-seat-capable fake-VT mechanism.
 */

static void fake_enter(struct uterm_vt *vt, struct signalfd_siginfo *info)
{
	if (info->ssi_code != SI_USER)
		return;

	log_debug("activating fake VT due to SIGUSR1");
	vt_call(vt, UTERM_VT_ACTIVATE);
}

static void fake_leave(struct uterm_vt *vt, struct signalfd_siginfo *info)
{
	if (info->ssi_code != SI_USER)
		return;

	log_debug("deactivating fake VT due to SIGUSR2");
	vt_call(vt, UTERM_VT_DEACTIVATE);
}

static int fake_activate(struct uterm_vt *vt)
{
	log_debug("activating fake VT due to user request");
	vt_call(vt, UTERM_VT_ACTIVATE);
	return 0;
}

static int fake_deactivate(struct uterm_vt *vt)
{
	log_debug("deactivating fake VT due to user request");
	vt_call(vt, UTERM_VT_DEACTIVATE);
	return 0;
}

static bool check_vt_support(void)
{
	if (!access("/dev/tty0", F_OK))
		return true;
	else
		return false;
}

static void vt_input(struct uterm_input *input,
		     struct uterm_input_event *ev,
		     void *data)
{
	struct uterm_vt *vt = data;

	if (UTERM_INPUT_HAS_MODS(ev, UTERM_LOGO_MASK | UTERM_CONTROL_MASK)) {
		if (ev->keysym == XKB_KEY_F12) {
			if (vt->active) {
				log_debug("deactivating fake VT due to user input");
				vt_call(vt, UTERM_VT_DEACTIVATE);
			} else {
				log_debug("activating fake VT due to user input");
				vt_call(vt, UTERM_VT_ACTIVATE);
			}
		}
	}
}

static void vt_sigusr1(struct ev_eloop *eloop, struct signalfd_siginfo *info,
		       void *data)
{
	struct uterm_vt *vt = data;

	if (vt->mode == UTERM_VT_REAL)
		real_enter(vt, info);
	else if (vt->mode == UTERM_VT_FAKE)
		fake_enter(vt, info);
}

static void vt_sigusr2(struct ev_eloop *eloop, struct signalfd_siginfo *info,
		       void *data)
{
	struct uterm_vt *vt = data;

	if (vt->mode == UTERM_VT_REAL)
		real_leave(vt, info);
	else if (vt->mode == UTERM_VT_FAKE)
		fake_leave(vt, info);
}

int uterm_vt_allocate(struct uterm_vt_master *vtm,
		      struct uterm_vt **out,
		      const char *seat,
		      struct uterm_input *input,
		      const char *vt_for_seat0,
		      uterm_vt_cb cb,
		      void *data)
{
	struct uterm_vt *vt;
	int ret;

	if (!vtm || !out)
		return -EINVAL;
	if (!seat)
		seat = "seat0";

	vt = malloc(sizeof(*vt));
	if (!vt)
		return -ENOMEM;
	memset(vt, 0, sizeof(*vt));
	vt->ref = 1;
	vt->vtm = vtm;
	vt->cb = cb;
	vt->data = data;

	vt->real_fd = -1;
	vt->real_num = -1;
	vt->real_saved_num = -1;

	ret = ev_eloop_register_signal_cb(vtm->eloop, SIGUSR1, vt_sigusr1, vt);
	if (ret)
		goto err_free;

	ret = ev_eloop_register_signal_cb(vtm->eloop, SIGUSR2, vt_sigusr2, vt);
	if (ret)
		goto err_sig1;

	if (!strcmp(seat, "seat0") && vtm->vt_support) {
		vt->mode = UTERM_VT_REAL;
		ret = real_open(vt, vt_for_seat0);
		if (ret)
			goto err_sig2;
	} else {
		vt->mode = UTERM_VT_FAKE;
		vt->input = input;

		ret = uterm_input_register_cb(vt->input, vt_input, vt);
		if (ret)
			goto err_sig2;

		uterm_input_ref(vt->input);
		uterm_input_wake_up(vt->input);
	}

	shl_dlist_link(&vtm->vts, &vt->list);
	*out = vt;
	return 0;

err_sig2:
	ev_eloop_unregister_signal_cb(vtm->eloop, SIGUSR2, vt_sigusr2, vt);
err_sig1:
	ev_eloop_unregister_signal_cb(vtm->eloop, SIGUSR1, vt_sigusr1, vt);
err_free:
	free(vt);
	return ret;
}

void uterm_vt_deallocate(struct uterm_vt *vt)
{
	unsigned int mode;

	if (!vt || !vt->vtm || vt->mode == UTERM_VT_DEAD)
		return;

	mode = vt->mode;
	vt->mode = UTERM_VT_DEAD;

	if (mode == UTERM_VT_REAL) {
		real_close(vt);
	} else if (mode == UTERM_VT_FAKE) {
		vt_call(vt, UTERM_VT_DEACTIVATE);
	}
	ev_eloop_unregister_signal_cb(vt->vtm->eloop, SIGUSR2, vt_sigusr2, vt);
	ev_eloop_unregister_signal_cb(vt->vtm->eloop, SIGUSR1, vt_sigusr1, vt);
	shl_dlist_unlink(&vt->list);
	uterm_input_sleep(vt->input);
	uterm_input_unref(vt->input);
	vt->vtm = NULL;
	uterm_vt_unref(vt);
}

void uterm_vt_ref(struct uterm_vt *vt)
{
	if (!vt || !vt->ref)
		return;

	++vt->ref;
}

void uterm_vt_unref(struct uterm_vt *vt)
{
	if (!vt || !vt->ref || --vt->ref)
		return;

	uterm_vt_deallocate(vt);
	free(vt);
}

int uterm_vt_activate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return -EINVAL;

	if (vt->mode == UTERM_VT_REAL)
		return real_activate(vt);
	else
		return fake_activate(vt);
}

int uterm_vt_deactivate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return -EINVAL;

	if (vt->mode == UTERM_VT_REAL)
		return real_deactivate(vt);
	else
		return fake_deactivate(vt);
}

int uterm_vt_master_new(struct uterm_vt_master **out,
			struct ev_eloop *eloop)
{
	struct uterm_vt_master *vtm;

	if (!out || !eloop)
		return -EINVAL;

	vtm = malloc(sizeof(*vtm));
	if (!vtm)
		return -ENOMEM;
	memset(vtm, 0, sizeof(*vtm));
	vtm->ref = 1;
	vtm->eloop = eloop;
	shl_dlist_init(&vtm->vts);
	vtm->vt_support = check_vt_support();

	ev_eloop_ref(vtm->eloop);
	*out = vtm;
	return 0;
}

void uterm_vt_master_ref(struct uterm_vt_master *vtm)
{
	if (!vtm || !vtm->ref)
		return;

	++vtm->ref;
}

void uterm_vt_master_unref(struct uterm_vt_master *vtm)
{
	struct uterm_vt *vt;

	if (!vtm || !vtm->ref || --vtm->ref)
		return;

	while (vtm->vts.next != &vtm->vts) {
		vt = shl_dlist_entry(vtm->vts.next,
					struct uterm_vt,
					list);
		uterm_vt_deallocate(vt);
	}

	ev_eloop_unref(vtm->eloop);
	free(vtm);
}
