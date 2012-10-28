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
#include "shl_misc.h"
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
	int real_kbmode;
	struct termios real_saved_attribs;
	struct ev_fd *real_efd;
};

struct uterm_vt_master {
	unsigned long ref;
	struct ev_eloop *eloop;

	bool vt_support;
	struct shl_dlist vts;
};

static void vt_call(struct uterm_vt *vt, unsigned int event)
{
	int ret;

	switch (event) {
	case UTERM_VT_ACTIVATE:
		if (!vt->active) {
			if (vt->cb) {
				ret = vt->cb(vt, event, vt->data);
				if (ret)
					log_warning("vt event handler returned %d instead of 0 on activation",
						    ret);
			}
			vt->active = true;
		}
		break;
	case UTERM_VT_DEACTIVATE:
		if (vt->active) {
			if (vt->cb) {
				ret = vt->cb(vt, event, vt->data);
				if (ret)
					log_warning("vt event handler returned %d instead of 0 on deactivation",
						    ret);
			}
			vt->active = false;
		}
		break;
	}
}

/*
 * 'Real' VTs
 * The linux kernel (used) to provide VTs via CONFIG_VT. These VTs are TTYs that
 * the kernel runs a very limit VT102 compatible console on. They also provide a
 * mechanism to switch between graphical user-applications.
 * An application that opens a VT is notified via two signals whenever the user
 * switches to or away from the VT. We catch these signals and forward a
 * notification to the application via callbacks.
 *
 * Real VTs are only available on seat0 and should be avoided whenever possible
 * as they have a horrible API, have synchronization issues and are unflexible.
 *
 * Also note that the VT API is asynchronous and requires acknowledgment of
 * applications when switching VTs. That means, when a VT-switch is started, the
 * currently-active VT is notified about this and needs to acknowledge this
 * switch. If it allows it, the new VT is notified that it is now started up.
 * This control-passing is very fragile. For instance if the currently-active VT
 * is stuck or paused, the VT switch cannot take place as it is not acknowledged
 * by the currently active VT.
 * Furthermore, there are some race-conditions during a switch. If resources
 * that are passed from one VT to another are acquired during this switch from a
 * 3rd party application, then they can highjack the VT-switch and make the new
 * VT fail acquiring the resources.
 *
 * There are a lot more issues. For instance VTs are not cleaned up when closed
 * which can cause deadlocks if VT_SETMODE is not reset.
 * All in all, real VTs are very fragile and should be avoided. They should only
 * be used for backwards-compatibility.
 */

static void real_sig_enter(struct uterm_vt *vt, struct signalfd_siginfo *info)
{
	struct vt_stat vts;
	int ret;

	if (info->ssi_code != SI_KERNEL)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warning("cannot get current VT state (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num)
		return;

	if (vt->active)
		log_warning("activating VT %d even though it's already active",
			    vt->real_num);
	else
		uterm_input_wake_up(vt->input);

	log_debug("enter VT %d %p due to VT signal", vt->real_num, vt);
	ioctl(vt->real_fd, VT_RELDISP, VT_ACKACQ);
	if (ioctl(vt->real_fd, KDSETMODE, KD_GRAPHICS))
		log_warn("cannot set graphics mode on vt %p (%d): %m", vt,
			 errno);
	vt_call(vt, UTERM_VT_ACTIVATE);
}

static void real_sig_leave(struct uterm_vt *vt, struct signalfd_siginfo *info)
{
	struct vt_stat vts;
	int ret;

	if (info->ssi_code != SI_KERNEL)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warning("cannot get current VT state (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num)
		return;

	if (!vt->active)
		log_warning("deactivating VT %d even though it's not active",
			    vt->real_num);
	else
		uterm_input_sleep(vt->input);

	log_debug("leaving VT %d %p due to VT signal", vt->real_num, vt);
	vt_call(vt, UTERM_VT_DEACTIVATE);
	ioctl(vt->real_fd, VT_RELDISP, 1);
	if (ioctl(vt->real_fd, KDSETMODE, KD_TEXT))
		log_warn("cannot set text mode on vt %p (%d): %m", vt, errno);
}

static void real_vt_input(struct ev_fd *fd, int mask, void *data)
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
				log_error("cannot find parent tty (%d, %d): %m",
					  err1, errno);
				return -EFAULT;
			}
		}

		errno = 0;
		if (ioctl(fd, VT_OPENQRY, &id) || id <= 0) {
			close(fd);
			log_err("cannot get unused tty (%d): %m", errno);
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
		log_err("cannot open tty %s (%d): %m", dev, errno);
		return -errno;
	}

	ret = fstat(fd, &st);
	if (ret) {
		log_error("cannot introspect tty %s (%d): %m", dev, errno);
		close(fd);
		return -errno;
	}
	id = minor(st.st_rdev);
	log_debug("new tty ID is %d", id);

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
			      EV_READABLE, real_vt_input, vt);
	if (ret)
		goto err_fd;

	/* Get the number of the VT which is active now, so we have something
	 * to switch back to in kmscon_vt_switch_leave. */
	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find the currently active VT (%d): %m", errno);
		ret = -EFAULT;
		goto err_eloop;
	}
	vt->real_saved_num = vts.v_active;

	if (tcgetattr(vt->real_fd, &vt->real_saved_attribs) < 0) {
		log_err("cannot get terminal attributes (%d): %m", errno);
		ret = -EFAULT;
		goto err_eloop;
	}

	/* Ignore control characters and disable echo */
	raw_attribs = vt->real_saved_attribs;
	cfmakeraw(&raw_attribs);

	/* Fix up line endings to be normal (cfmakeraw hoses them) */
	raw_attribs.c_oflag |= OPOST | OCRNL;

	if (tcsetattr(vt->real_fd, TCSANOW, &raw_attribs) < 0)
		log_warn("cannot put terminal into raw mode (%d): %m", errno);

	if (ioctl(vt->real_fd, KDSETMODE, KD_GRAPHICS)) {
		log_err("cannot put VT in graphics mode (%d): %m", errno);
		ret = -errno;
		goto err_reset;
	}

	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	ret = sigprocmask(SIG_BLOCK, &mask, NULL);
	if (ret) {
		log_error("cannot block SIGUSR1/2 (%d): %m", errno);
		ret = -EFAULT;
		goto err_text;
	}

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_PROCESS;
	mode.acqsig = SIGUSR1;
	mode.relsig = SIGUSR2;

	if (ioctl(vt->real_fd, VT_SETMODE, &mode)) {
		log_err("cannot take control of vt handling (%d): %m", errno);
		ret = -errno;
		goto err_text;
	}

	ret = ioctl(vt->real_fd, KDGKBMODE, &vt->real_kbmode);
	if (ret) {
		log_error("cannot retrieve VT KBMODE (%d): %m", errno);
		ret = -EFAULT;
		goto err_setmode;
	}

	log_debug("previous VT KBMODE was %d", vt->real_kbmode);
	if (vt->real_kbmode == K_OFF) {
		log_warning("VT KBMODE was K_OFF, using K_UNICODE instead");
		vt->real_kbmode = K_UNICODE;
	}

	ret = ioctl(vt->real_fd, KDSKBMODE, K_OFF);
	if (ret) {
		log_error("cannot set VT KBMODE to K_OFF (%d): %m", errno);
		ret = -EFAULT;
		goto err_setmode;
	}

	return 0;

err_setmode:
	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_AUTO;
	ret = ioctl(vt->real_fd, VT_SETMODE, &mode);
	if (ret)
		log_warning("cannot reset VT %d to VT_AUTO mode (%d): %m",
			    vt->real_num, errno);
err_text:
	ret = ioctl(vt->real_fd, KDSETMODE, KD_TEXT);
	if (ret)
		log_warning("cannot reset VT %d to text-mode (%d): %m",
			    vt->real_num, errno);
err_reset:
	ret = tcsetattr(vt->real_fd, TCSANOW, &vt->real_saved_attribs);
	if (ret)
		log_warning("cannot reset VT %d attributes (%d): %m",
			    vt->real_num, errno);
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
	int ret;

	log_debug("closing VT %d", vt->real_num);

	ret = ioctl(vt->real_fd, KDSKBMODE, vt->real_kbmode);
	if (ret)
		log_error("cannot reset VT KBMODE to %d (%d): %m",
			  vt->real_kbmode, errno);

	memset(&mode, 0, sizeof(mode));
	mode.mode = VT_AUTO;
	ret = ioctl(vt->real_fd, VT_SETMODE, &mode);
	if (ret)
		log_warning("cannot reset VT %d to VT_AUTO mode (%d): %m",
			    vt->real_num, errno);

	ret = ioctl(vt->real_fd, KDSETMODE, KD_TEXT);
	if (ret)
		log_warning("cannot reset VT %d to text-mode (%d): %m",
			    vt->real_num, errno);

	ret = tcsetattr(vt->real_fd, TCSANOW, &vt->real_saved_attribs);
	if (ret)
		log_warning("cannot reset VT %d attributes (%d): %m",
			    vt->real_num, errno);

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

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret)
		log_warn("cannot find current VT (%d): %m", errno);
	else if (vts.v_active == vt->real_num)
		return 0;

	if (vt->active)
		log_warning("activating VT %d even though it's already active",
			    vt->real_num);

	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_num);
	if (ret) {
		log_warn("cannot enter VT %d (%d): %m", vt->real_num, errno);
		return -EFAULT;
	}

	log_debug("entering VT %d on demand", vt->real_num);
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
 *
 * When run as a daemon, the VT where we were started on is often no longer a
 * safe return-path when we shut-down. Therefore, you might want to avoid
 * calling this when started as a long-running daemon.
 */
static int real_deactivate(struct uterm_vt *vt)
{
	int ret;
	struct vt_stat vts;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT (%d): %m", errno);
		return -EFAULT;
	}

	if (vts.v_active != vt->real_num)
		return 0;

	if (!vt->active)
		log_warning("deactivating VT %d even though it's not active",
			    vt->real_num);

	ret = ioctl(vt->real_fd, VT_ACTIVATE, vt->real_saved_num);
	if (ret) {
		log_warn("cannot leave VT %d to VT %d (%d): %m", vt->real_num,
			 vt->real_saved_num, errno);
		return -EFAULT;
	}

	log_debug("leaving VT %d on demand to VT %d", vt->real_num,
		  vt->real_saved_num);
	return -EINPROGRESS;
}

static void real_input(struct uterm_vt *vt, struct uterm_input_event *ev)
{
	int id;
	struct vt_stat vts;
	int ret;

	if (ev->handled)
		return;

	ret = ioctl(vt->real_fd, VT_GETSTATE, &vts);
	if (ret) {
		log_warn("cannot find current VT (%d): %m", errno);
		return;
	}

	if (vts.v_active != vt->real_num)
		return;

	id = 0;
	if (SHL_HAS_BITS(ev->mods, SHL_CONTROL_MASK | SHL_ALT_MASK) &&
	    ev->keysyms[0] >= XKB_KEY_F1 && ev->keysyms[0] <= XKB_KEY_F12) {
		ev->handled = true;
		id = ev->keysyms[0] - XKB_KEY_F1 + 1;
		if (id == vt->real_num)
			return;
	} else if (ev->keysyms[0] >= XKB_KEY_XF86Switch_VT_1 &&
		   ev->keysyms[0] <= XKB_KEY_XF86Switch_VT_12) {
		ev->handled = true;
		id = ev->keysyms[0] - XKB_KEY_XF86Switch_VT_1 + 1;
		if (id == vt->real_num)
			return;
	}

	if (!id)
		return;

	if (!vt->active)
		log_warning("leaving VT %d even though it's not active",
			    vt->real_num);

	log_debug("deactivating VT %d to %d due to user input", vt->real_num,
		  id);

	ret = ioctl(vt->real_fd, VT_ACTIVATE, id);
	if (ret) {
		log_warn("cannot leave VT %d to %d (%d): %m", vt->real_num,
			 id, errno);
		return;
	}
}

/*
 * Fake VT:
 * For systems without CONFIG_VT or for all seats that have no real VTs (which
 * is all seats except seat0), we support a fake-VT mechanism. This machanism is
 * only used for debugging and should not be used in production.
 *
 * Fake-VTs react on a key-press and activate themselves if not active. If they
 * are already active, they deactivate themself. To switch from one fake-VT to
 * another, you first need to deactivate the current fake-VT and then activate
 * the new fake-VT. This also means that you must use different hotkeys for each
 * fake-VT.
 * This is a very fragile infrastructure and should only be used for debugging.
 *
 * To avoid this bad situation, you simply activate a fake-VT during startup
 * with uterm_vt_activate() and then do not use the hotkeys at all. This assumes
 * that the fake-VT is the only application on this seat.
 *
 * If you use multiple fake-VTs on a seat without real-VTs, you should really
 * use some other daemon that handles VT-switches. Otherwise, there is no sane
 * way to communicate this between the fake-VTs. So please use fake-VTs only for
 * debugging or if they are the only session on their seat.
 */

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

static void fake_input(struct uterm_vt *vt, struct uterm_input_event *ev)
{
	if (ev->handled)
		return;

	if (SHL_HAS_BITS(ev->mods, SHL_CONTROL_MASK | SHL_LOGO_MASK) &&
	    ev->keysyms[0] == XKB_KEY_F12) {
		ev->handled = true;
		if (vt->active) {
			log_debug("deactivating fake VT due to user input");
			vt_call(vt, UTERM_VT_DEACTIVATE);
		} else {
			log_debug("activating fake VT due to user input");
			vt_call(vt, UTERM_VT_ACTIVATE);
		}
	}
}

/*
 * Generic VT handling layer
 * VTs are a historical concept. Technically, they actually are a VT102
 * compatible terminal emulator, but with the invention of X11 and other
 * graphics servers, VTs were mainly used to control which application is
 * currently active.
 * If an application is "active" it is allowed to read keyboard/mouse/etc input
 * and access the output devices (like displays/monitors). If an application is
 * not active (that is, inactive) it should not access these devices at all and
 * leave them for other VTs so they can access them.
 *
 * The kernel VTs have a horrible API and thus should be avoided whenever
 * possible. We provide a layer for this VT as "real_*" VTs here. If those are
 * not available, we also provide a layer for "fake_*" VTs. See their
 * description for more information.
 *
 * If you allocate a new VT with this API, it automatically chooses the right
 * implementation for you. So you are notified whenever your VT becomes active
 * and when it becomes inactive. You do not have to care for any other VT
 * handling.
 */

static void vt_input(struct uterm_input *input,
		     struct uterm_input_event *ev,
		     void *data)
{
	struct uterm_vt *vt = data;

	if (vt->mode == UTERM_VT_REAL)
		real_input(vt, ev);
	else if (vt->mode == UTERM_VT_FAKE)
		fake_input(vt, ev);
}

static void vt_sigusr1(struct ev_eloop *eloop, struct signalfd_siginfo *info,
		       void *data)
{
	struct uterm_vt *vt = data;

	if (vt->mode == UTERM_VT_REAL)
		real_sig_enter(vt, info);
}

static void vt_sigusr2(struct ev_eloop *eloop, struct signalfd_siginfo *info,
		       void *data)
{
	struct uterm_vt *vt = data;

	if (vt->mode == UTERM_VT_REAL)
		real_sig_leave(vt, info);
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
	vt->input = input;

	vt->real_fd = -1;
	vt->real_num = -1;
	vt->real_saved_num = -1;

	ret = ev_eloop_register_signal_cb(vtm->eloop, SIGUSR1, vt_sigusr1, vt);
	if (ret)
		goto err_free;

	ret = ev_eloop_register_signal_cb(vtm->eloop, SIGUSR2, vt_sigusr2, vt);
	if (ret)
		goto err_sig1;

	ret = uterm_input_register_cb(vt->input, vt_input, vt);
	if (ret)
		goto err_sig2;

	if (!strcmp(seat, "seat0") && vtm->vt_support) {
		vt->mode = UTERM_VT_REAL;
		ret = real_open(vt, vt_for_seat0);
		if (ret)
			goto err_input;
	} else {
		vt->mode = UTERM_VT_FAKE;
		uterm_input_wake_up(vt->input);
	}

	uterm_input_ref(vt->input);
	shl_dlist_link(&vtm->vts, &vt->list);
	*out = vt;
	return 0;

err_input:
	uterm_input_unregister_cb(vt->input, vt_input, vt);
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
	if (!vt || !vt->vtm)
		return;

	if (vt->mode == UTERM_VT_REAL) {
		real_close(vt);
	} else if (vt->mode == UTERM_VT_FAKE) {
		vt_call(vt, UTERM_VT_DEACTIVATE);
		uterm_input_sleep(vt->input);
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
	vtm->vt_support = !access("/dev/tty0", F_OK);

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

/* Drops a reference to the VT-master. If the reference drops to 0, all
 * allocated VTs are deallocated and the VT-master is destroyed. */
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

/* Calls uterm_vt_activate() on all allocated VTs on this master. Returns
 * number of VTs that returned -EINPROGRESS or a negative error code on failure.
 * See uterm_vt_activate() for information. */
int uterm_vt_master_activate_all(struct uterm_vt_master *vtm)
{
	struct uterm_vt *vt;
	struct shl_dlist *iter;
	int ret, res = 0;
	unsigned int in_progress = 0;

	if (!vtm)
		return -EINVAL;

	shl_dlist_for_each(iter, &vtm->vts) {
		vt = shl_dlist_entry(iter, struct uterm_vt, list);
		ret = uterm_vt_activate(vt);
		if (ret == -EINPROGRESS)
			in_progress++;
		else if (ret)
			res = ret;
	}

	if (in_progress)
		return in_progress;

	return res;
}

/* Calls uterm_vt_deactivate() on all allocated VTs on this master. Returns
 * number of VTs that returned -EINPROGRESS or a negative error code on failure.
 * See uterm_vt_deactivate() for information. */
int uterm_vt_master_deactivate_all(struct uterm_vt_master *vtm)
{
	struct uterm_vt *vt;
	struct shl_dlist *iter;
	int ret, res = 0;
	unsigned int in_progress = 0;

	if (!vtm)
		return -EINVAL;

	shl_dlist_for_each(iter, &vtm->vts) {
		vt = shl_dlist_entry(iter, struct uterm_vt, list);
		ret = uterm_vt_deactivate(vt);
		if (ret == -EINPROGRESS)
			in_progress++;
		else if (ret)
			res = ret;
	}

	if (in_progress)
		return in_progress;

	return res;
}
