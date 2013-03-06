/*
 * uvtd - User-space VT daemon
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
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
 * Every virtual terminal forms a session inside of uvtd. Sessions are scheduled
 * by the seat/session-scheduler and notified whenever they get active/inactive.
 */

#include <errno.h>
#include <inttypes.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termio.h>
#include <termios.h>
#include <unistd.h>
#include "shl_hook.h"
#include "shl_log.h"
#include "uvt.h"
#include "uvtd_seat.h"
#include "uvtd_vt.h"

#define LOG_SUBSYSTEM "vt"

struct uvtd_vt {
	unsigned long ref;
	struct uvt_ctx *uctx;
	struct shl_hook *hook;
	struct uvtd_session *session;
	struct uvtd_seat *seat;
	bool is_legacy;

	unsigned int mode;
	unsigned int kbmode;
	struct vt_mode vtmode;
	pid_t vtpid;
};

static void vt_hup(struct uvtd_vt *vt)
{
	struct uvt_vt_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UVT_VT_HUP;

	shl_hook_call(vt->hook, vt, &ev);
}

static int vt_session_event(struct uvtd_session *session, unsigned int event,
			    void *data)
{
	struct uvtd_vt *vt = data;

	switch (event) {
	case UVTD_SESSION_UNREGISTER:
		vt->session = NULL;
		vt->seat = NULL;
		vt_hup(vt);
		break;
	case UVTD_SESSION_ACTIVATE:
		log_debug("activate %p", vt);
		break;
	case UVTD_SESSION_DEACTIVATE:
		log_debug("deactivate %p", vt);
		break;
	}

	return 0;
}

int uvtd_vt_new(struct uvtd_vt **out, struct uvt_ctx *uctx, unsigned int id,
		struct uvtd_seat *seat, bool is_legacy)
{
	struct uvtd_vt *vt;
	int ret;

	if (!out || !uctx)
		return -EINVAL;

	vt = malloc(sizeof(*vt));
	if (!vt)
		return -ENOMEM;

	memset(vt, 0, sizeof(*vt));
	vt->ref = 1;
	vt->uctx = uctx;
	vt->seat = seat;
	vt->is_legacy = is_legacy;
	vt->mode = KD_TEXT;
	vt->kbmode = K_UNICODE;
	vt->vtmode.mode = VT_AUTO;

	ret = shl_hook_new(&vt->hook);
	if (ret)
		goto err_free;

	ret = uvtd_seat_register_session(seat, &vt->session, id,
					 vt_session_event, vt);
	if (ret)
		goto err_hook;

	uvt_ctx_ref(vt->uctx);
	*out = vt;
	return 0;

err_hook:
	shl_hook_free(vt->hook);
err_free:
	free(vt);
	return ret;
}

void uvtd_vt_ref(struct uvtd_vt *vt)
{
	if (!vt || !vt->ref)
		return;

	++vt->ref;
}

void uvtd_vt_unref(struct uvtd_vt *vt)
{
	if (!vt || !vt->ref || --vt->ref)
		return;

	uvtd_session_unregister(vt->session);
	shl_hook_free(vt->hook);
	uvt_ctx_unref(vt->uctx);
	free(vt);
}

int uvtd_vt_register_cb(struct uvtd_vt *vt, uvt_vt_cb cb, void *data)
{
	if (!vt)
		return -EINVAL;

	return shl_hook_add_cast(vt->hook, cb, data, false);
}

void uvtd_vt_unregister_cb(struct uvtd_vt *vt, uvt_vt_cb cb, void *data)
{
	if (!vt)
		return;

	shl_hook_rm_cast(vt->hook, cb, data);
}

int uvtd_vt_read(struct uvtd_vt *vt, uint8_t *mem, size_t len)
{
	if (!vt || !vt->seat)
		return -ENODEV;

	return -EAGAIN;
}

int uvtd_vt_write(struct uvtd_vt *vt, const uint8_t *mem, size_t len)
{
	if (!vt || !vt->seat)
		return -ENODEV;

	return len;
}

unsigned int uvtd_vt_poll(struct uvtd_vt *vt)
{
	if (!vt || !vt->seat)
		return UVT_TTY_HUP | UVT_TTY_READ | UVT_TTY_WRITE;

	return UVT_TTY_WRITE;
}

static int vt_ioctl_TCFLSH(void *data, unsigned long arg)
{
	switch (arg) {
	case TCIFLUSH:
	case TCOFLUSH:
	case TCIOFLUSH:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vt_ioctl_VT_ACTIVATE(void *data, unsigned long arg)
{
	struct uvtd_vt *vt = data;

	if (!vt->seat)
		return -ENODEV;

	return -EINVAL;
}

static int vt_ioctl_VT_WAITACTIVE(void *data, unsigned long arg)
{
	struct uvtd_vt *vt = data;

	if (!vt->seat)
		return -ENODEV;

	return -EINVAL;
}

static int vt_ioctl_VT_GETSTATE(void *data, struct vt_stat *arg)
{
	struct uvtd_vt *vt = data;

	if (!vt->seat)
		return -ENODEV;

	return -EINVAL;
}

static int vt_ioctl_VT_OPENQRY(void *data, unsigned int *arg)
{
	struct uvtd_vt *vt = data;

	if (!vt->seat)
		return -ENODEV;

	return -EINVAL;
}

static int vt_ioctl_VT_GETMODE(void *data, struct vt_mode *arg)
{
	struct uvtd_vt *vt = data;

	memcpy(arg, &vt->vtmode, sizeof(*arg));
	return 0;
}

static int vt_ioctl_VT_SETMODE(void *data, const struct vt_mode *arg,
			       pid_t pid)
{
	struct uvtd_vt *vt = data;

	/* TODO: implement waitv logic (hang on write if not active) */
	if (arg->waitv)
		return -EOPNOTSUPP;

	if (arg->frsig)
		return -EINVAL;
	if (arg->relsig > SIGRTMAX || arg->relsig < 0)
		return -EINVAL;
	if (arg->acqsig > SIGRTMAX || arg->acqsig < 0)
		return -EINVAL;

	switch (arg->mode) {
	case VT_AUTO:
		if (arg->acqsig || arg->relsig)
			return -EINVAL;
		vt->vtpid = 0;
		break;
	case VT_PROCESS:
		vt->vtpid = pid;
		break;
	default:
		return -EINVAL;
	}

	memcpy(&vt->vtmode, arg, sizeof(*arg));
	return 0;
}

static int vt_ioctl_VT_RELDISP(void *data, unsigned long arg)
{
	struct uvtd_vt *vt = data;

	if (!vt->seat)
		return -ENODEV;

	return -EINVAL;
}

static int vt_ioctl_KDGETMODE(void *data, unsigned int *arg)
{
	struct uvtd_vt *vt = data;

	*arg = vt->mode;
	return 0;
}

static int vt_ioctl_KDSETMODE(void *data, unsigned int arg)
{
	struct uvtd_vt *vt = data;

	switch (arg) {
	case KD_TEXT0:
	case KD_TEXT1:
		arg = KD_TEXT;
		/* fallthrough */
	case KD_TEXT:
	case KD_GRAPHICS:
		vt->mode = arg;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int vt_ioctl_KDGKBMODE(void *data, unsigned int *arg)
{
	struct uvtd_vt *vt = data;

	*arg = vt->kbmode;
	return 0;
}

static int vt_ioctl_KDSKBMODE(void *data, unsigned int arg)
{
	struct uvtd_vt *vt = data;

	switch (arg) {
	case K_RAW:
		/* TODO: what does K_RAW do? */
	case K_UNICODE:
	case K_OFF:
		vt->kbmode = arg;
		break;
	case K_XLATE:
	case K_MEDIUMRAW:
		/* TODO: do we need these? */
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}

	return 0;
}

/* compatibility to UVT-VT ops */

static void vt_ref(void *vt)
{
	uvtd_vt_ref(vt);
}

static void vt_unref(void *vt)
{
	uvtd_vt_unref(vt);
}

static int vt_register_cb(void *vt, uvt_vt_cb cb, void *data)
{
	return uvtd_vt_register_cb(vt, cb, data);
}

static void vt_unregister_cb(void *vt, uvt_vt_cb cb, void *data)
{
	uvtd_vt_register_cb(vt, cb, data);
}

static int vt_read(void *vt, uint8_t *mem, size_t len)
{
	return uvtd_vt_read(vt, mem, len);
}

static int vt_write(void *vt, const uint8_t *mem, size_t len)
{
	return uvtd_vt_write(vt, mem, len);
}

static unsigned int vt_poll(void *vt)
{
	return uvtd_vt_poll(vt);
}

struct uvt_vt_ops uvtd_vt_ops = {
	.ref = vt_ref,
	.unref = vt_unref,
	.register_cb = vt_register_cb,
	.unregister_cb = vt_unregister_cb,
	.read = vt_read,
	.write = vt_write,
	.poll = vt_poll,

	.ioctl_TCFLSH = vt_ioctl_TCFLSH,

	.ioctl_VT_ACTIVATE = vt_ioctl_VT_ACTIVATE,
	.ioctl_VT_WAITACTIVE = vt_ioctl_VT_WAITACTIVE,
	.ioctl_VT_GETSTATE = vt_ioctl_VT_GETSTATE,
	.ioctl_VT_OPENQRY = vt_ioctl_VT_OPENQRY,
	.ioctl_VT_GETMODE = vt_ioctl_VT_GETMODE,
	.ioctl_VT_SETMODE = vt_ioctl_VT_SETMODE,
	.ioctl_VT_RELDISP = vt_ioctl_VT_RELDISP,
	.ioctl_KDGETMODE = vt_ioctl_KDGETMODE,
	.ioctl_KDSETMODE = vt_ioctl_KDSETMODE,
	.ioctl_KDGKBMODE = vt_ioctl_KDGKBMODE,
	.ioctl_KDSKBMODE = vt_ioctl_KDSKBMODE,
};
