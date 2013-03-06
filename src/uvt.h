/*
 * UVT - Userspace Virtual Terminals
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@gmail.com>
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
 * Userspace Virtual Terminals
 * Virtual terminals were historically implemented in the kernel via a
 * character-device. This layer provides a user-space implementation via
 * CUSE/FUSE that can be used to provide the same API from user-space.
 */

#ifndef UVT_H
#define UVT_H

#include <inttypes.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <termio.h>
#include <termios.h>

/* UVT types */

struct uvt_client;
struct uvt_cdev;
struct uvt_ctx;

/* TTYs */

enum uvt_tty_event_type {
	UVT_TTY_HUP		= 0x01,
	UVT_TTY_READ		= 0x02,
	UVT_TTY_WRITE		= 0x04,
};

struct uvt_tty_event {
	unsigned int type;
};

typedef void (*uvt_tty_cb) (void *tty, struct uvt_tty_event *ev, void *data);

struct uvt_tty_ops {
	void (*ref) (void *data);
	void (*unref) (void *data);
	int (*register_cb) (void *data, uvt_tty_cb cb, void *cb_data);
	void (*unregister_cb) (void *data, uvt_tty_cb cb, void *cb_data);

	int (*read) (void *data, uint8_t *mem, size_t len);
	int (*write) (void *data, const uint8_t *mem, size_t len);
	unsigned int (*poll) (void *data);
};

/* virtual terminals */

enum uvt_vt_event_type {
	UVT_VT_HUP		= 0x01,
	UVT_VT_TTY		= 0x02,
};

struct uvt_vt_event {
	unsigned int type;

	union {
		struct uvt_tty_event tty;
	};
};

typedef void (*uvt_vt_cb) (void *vt, struct uvt_vt_event *ev, void *data);

struct uvt_vt_ops {
	void (*ref) (void *data);
	void (*unref) (void *data);
	int (*register_cb) (void *data, uvt_vt_cb cb, void *cb_data);
	void (*unregister_cb) (void *data, uvt_vt_cb cb, void *cb_data);

	int (*read) (void *data, uint8_t *mem, size_t len);
	int (*write) (void *data, const uint8_t *mem, size_t len);
	unsigned int (*poll) (void *data);

	/* TTY ioctls */
	int (*ioctl_TCFLSH) (void *data, unsigned long arg);

	/* VT ioctls */
	int (*ioctl_VT_ACTIVATE) (void *data, unsigned long arg);
	int (*ioctl_VT_WAITACTIVE) (void *data, unsigned long arg);
	int (*ioctl_VT_GETSTATE) (void *data, struct vt_stat *arg);
	int (*ioctl_VT_OPENQRY) (void *data, unsigned int *arg);
	int (*ioctl_VT_GETMODE) (void *data, struct vt_mode *arg);
	int (*ioctl_VT_SETMODE) (void *data, const struct vt_mode *arg,
				 pid_t pid);
	int (*ioctl_VT_RELDISP) (void *data, unsigned long arg);
	int (*ioctl_KDGETMODE) (void *data, unsigned int *arg);
	int (*ioctl_KDSETMODE) (void *data, unsigned int arg);
	int (*ioctl_KDGKBMODE) (void *data, unsigned int *arg);
	int (*ioctl_KDSKBMODE) (void *data, unsigned int arg);

/*
   Complete list of all ioctls that the kernel supports. The internal handler
   returns -EOPNOTSUPP for all of them as they haven't been implemented, yet.
   We need to check if they are actually required or whether it's not worth the
   effort.
   Please implement them only if you know a client that requires them. Also
   consider implementing them as a no-op if the client doesn't depend on the
   call to actually do something. We want to keep the actual callbacks at a
   minimum.

   TTY ioctls

	int (*ioctl_TIOCPKT) (void *data, ...);
	int (*ioctl_TCXONC) (void *data, ...);
	int (*ioctl_TCGETS) (void *data, struct termios *arg);
	int (*ioctl_TCSETS) (void *data, const struct termios *arg);
	int (*ioctl_TCSETSF) (void *data, const struct termios *arg);
	int (*ioctl_TCSETSW) (void *data, const struct termios *arg);
	int (*ioctl_TCGETA) (void *data, ...);
	int (*ioctl_TCSETA) (void *data, ...);
	int (*ioctl_TCSETAF) (void *data, ...);
	int (*ioctl_TCSETAW) (void *data, ...);
	int (*ioctl_TIOCGLCKTRMIOS) (void *data, ...);
	int (*ioctl_TIOCSLCKTRMIOS) (void *data, ...);
	int (*ioctl_TCGETX) (void *data, ...);
	int (*ioctl_TCSETX) (void *data, ...);
	int (*ioctl_TCSETXW) (void *data, ...);
	int (*ioctl_TCSETXF) (void *data, ...);
	int (*ioctl_TIOCGSOFTCAR) (void *data, ...);
	int (*ioctl_TIOCSSOFTCAR) (void *data, ...);

   VT ioctls

	int (*ioctl_TIOCLINUX) (void *data, ...);
	int (*ioctl_KIOCSOUND) (void *data, ...);
	int (*ioctl_KDMKTONE) (void *data, ...);
	int (*ioctl_KDGKBTYPE) (void *data, char *arg);
	int (*ioctl_KDADDIO) (void *data, unsigned long arg);
	int (*ioctl_KDDELIO) (void *data, unsigned long arg);
	int (*ioctl_KDENABIO) (void *data);
	int (*ioctl_KDDISABIO) (void *data);
	int (*ioctl_KDKBDREP) (void *data, struct kbd_repeat *arg);
	int (*ioctl_KDMAPDISP) (void *data);
	int (*ioctl_KDUNMAPDISP) (void *data);
	int (*ioctl_KDGKBMETA) (void *data, long *arg);
	int (*ioctl_KDSKBMETA) (void *data, long arg);
	int (*ioctl_KDGETKEYCODE) (void *data, ...);
	int (*ioctl_KDSETKEYCODE) (void *data, ...);
	int (*ioctl_KDGKBENT) (void *data, ...);
	int (*ioctl_KDSKBENT) (void *data, ...);
	int (*ioctl_KDGKBSENT) (void *data, ...);
	int (*ioctl_KDSKBSENT) (void *data, ...);
	int (*ioctl_KDGKBDIACR) (void *data, ...);
	int (*ioctl_KDSKBDIACR) (void *data, ...);
	int (*ioctl_KDGKBDIACRUC) (void *data, ...);
	int (*ioctl_KDSKBDIACRUC) (void *data, ...);
	int (*ioctl_KDGETLED) (void *data, char *arg);
	int (*ioctl_KDSETLED) (void *data, long arg);
	int (*ioctl_KDGKBLED) (void *data, char *arg);
	int (*ioctl_KDSKBLED) (void *data, long arg);
	int (*ioctl_KDSIGACCEPT) (void *data, ...);
	int (*ioctl_VT_SETACTIVATE) (void *data, ...);
	int (*ioctl_VT_DISALLOCATE) (void *data, ...);
	int (*ioctl_VT_RESIZE) (void *data, ...);
	int (*ioctl_VT_RESIZEX) (void *data, ...);
	int (*ioctl_GIO_FONT) (void *data, ...);
	int (*ioctl_PIO_FONT) (void *data, ...);
	int (*ioctl_GIO_CMAP) (void *data, ...);
	int (*ioctl_PIO_CMAP) (void *data, ...);
	int (*ioctl_GIO_FONTX) (void *data, ...);
	int (*ioctl_PIO_FONTX) (void *data, ...);
	int (*ioctl_PIO_FONTRESET) (void *data, ...);
	int (*ioctl_KDFONTOP) (void *data, ...);
	int (*ioctl_GIO_SCRNMAP) (void *data, ...);
	int (*ioctl_PIO_SCRNMAP) (void *data, ...);
	int (*ioctl_GIO_UNISCRNMAP) (void *data, ...);
	int (*ioctl_PIO_UNISCRNMAP) (void *data, ...);
	int (*ioctl_PIO_UNIMAPCLR) (void *data, ...);
	int (*ioctl_GIO_UNIMAP) (void *data, ...);
	int (*ioctl_PIO_UNIMAP) (void *data, ...);
	int (*ioctl_VT_LOCKSWITCH) (void *data);
	int (*ioctl_VT_UNLOCKSWITCH) (void *data);
	int (*ioctl_VT_GETHIFONTMASK) (void *data, ...);
	int (*ioctl_VT_WAITEVENT) (void *data, ...);
*/
};

/* client sessions */

void uvt_client_ref(struct uvt_client *client);
void uvt_client_unref(struct uvt_client *client);

int uvt_client_set_vt(struct uvt_client *client, const struct uvt_vt_ops *vt,
		      void *vt_data);
void uvt_client_kill(struct uvt_client *client);
bool uvt_client_is_dead(struct uvt_client *client);

/* character devices */

enum uvt_cdev_event_type {
	UVT_CDEV_HUP,
	UVT_CDEV_OPEN,
};

struct uvt_cdev_event {
	unsigned int type;

	union {
		struct uvt_client *client;
	};
};

typedef void (*uvt_cdev_cb) (struct uvt_cdev *cdev,
			     struct uvt_cdev_event *ev,
			     void *data);

int uvt_cdev_new(struct uvt_cdev **out, struct uvt_ctx *ctx,
		 const char *name, unsigned int major, unsigned int minor);
void uvt_cdev_ref(struct uvt_cdev *cdev);
void uvt_cdev_unref(struct uvt_cdev *cdev);

int uvt_cdev_register_cb(struct uvt_cdev *cdev, uvt_cdev_cb cb, void *data);
void uvt_cdev_unregister_cb(struct uvt_cdev *cdev, uvt_cdev_cb cb, void *data);

/* contexts */

typedef void (*uvt_log_t) (void *data,
			   const char *file,
			   int line,
			   const char *func,
			   const char *subs,
			   unsigned int sev,
			   const char *format,
			   va_list args);

int uvt_ctx_new(struct uvt_ctx **out, uvt_log_t log, void *log_data);
void uvt_ctx_ref(struct uvt_ctx *ctx);
void uvt_ctx_unref(struct uvt_ctx *ctx);

int uvt_ctx_get_fd(struct uvt_ctx *ctx);
void uvt_ctx_dispatch(struct uvt_ctx *ctx);

unsigned int uvt_ctx_get_major(struct uvt_ctx *ctx);
int uvt_ctx_new_minor(struct uvt_ctx *ctx, unsigned int *out);
void uvt_ctx_free_minor(struct uvt_ctx *ctx, unsigned int minor);

/* pty tty implementation */

struct uvt_tty_null;
extern const struct uvt_tty_ops uvt_tty_null_ops;

int uvt_tty_null_new(struct uvt_tty_null **out, struct uvt_ctx *ctx);
void uvt_tty_null_ref(struct uvt_tty_null *tty);
void uvt_tty_null_unref(struct uvt_tty_null *tty);

#endif /* UVT_H */
