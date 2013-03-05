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
 * Character Devices
 * This implements a VT character device entry point via the CUSE API. It does
 * not implement the VT API on top of the character-device (cdev) but only
 * provides the entry point. It is up to the user to bind open-files to VT and
 * client objects.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/major.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_llog.h"
#include "uvt.h"
#include "uvt_internal.h"

#include <fuse/fuse.h>
#include <fuse/fuse_common.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#include <fuse/cuse_lowlevel.h>

#define LLOG_SUBSYSTEM "uvt_cdev"

/*
 * FUSE low-level ops
 * This implements all the file-system operations on the character-device. It
 * is important that we handle interrupts correctly (ENOENT) and never loose
 * any data. This is all single threaded as it is not performance critical at
 * all.
 * We simply dispatch each call to uvt_client as this implements all the
 * client-session related operations.
 */

static void ll_open(fuse_req_t req, struct fuse_file_info *fi)
{
	struct uvt_cdev *cdev = fuse_req_userdata(req);
	struct uvt_client *client;
	struct uvt_cdev_event ev;
	int ret;

	ret = uvt_client_ll_open(&client, cdev, req, fi);
	if (ret)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = UVT_CDEV_OPEN;
	ev.client = client;
	shl_hook_call(cdev->hook, cdev, &ev);
}

static void ll_destroy(void *data) {
	struct uvt_cdev *cdev = data;
	struct uvt_client *client;

	/* on unexpected shutdown this kills all open clients */
	while (!shl_dlist_empty(&cdev->clients)) {
		client = shl_dlist_entry(cdev->clients.next,
					 struct uvt_client, list);
		uvt_client_kill(client);
		uvt_client_unref(client);
	}
}

static const struct cuse_lowlevel_ops ll_ops = {
	.init = NULL,
	.destroy = ll_destroy,
	.open = ll_open,
	.release = uvt_client_ll_release,
	.read = uvt_client_ll_read,
	.write = uvt_client_ll_write,
	.poll = uvt_client_ll_poll,
	.ioctl = uvt_client_ll_ioctl,
	.flush = NULL,
	.fsync = NULL,
};

/*
 * FUSE channel ops
 * The connection to the FUSE kernel module is done via a file-descriptor.
 * Writing to it is synchronous, so the commands that we write are
 * _immediately_ executed and return the result to us. Furthermore, write()
 * is always non-blocking and always succeeds so no reason to watch for
 * EAGAIN. Reading from the FD, on the other hand, may block if there is no
 * data available so we mark it as O_NONBLOCK. The kernel maintains
 * an event-queue that we read from. So there may be pending events that we
 * haven't read but which affect the calls that we write to the kernel. This
 * is important when handling interrupts.
 * chan_receive() and chan_send() handle I/O to the kernel module and are
 * hooked up into a fuse-channel.
 */

static int chan_receive(struct fuse_chan **chp, char *buf, size_t size)
{
	struct fuse_chan *ch = *chp;
	struct uvt_cdev *cdev = fuse_chan_data(ch);
	struct fuse_session *se = fuse_chan_session(ch);
	int fd = fuse_chan_fd(ch);
	ssize_t res;

	if (!se || !cdev)
		return -EINVAL;

	if (!size)
		return 0;

restart:
	if (fuse_session_exited(se))
		return 0;

	res = read(fd, buf, size);
	if (!res) {
		/* EOF on cuse file */
		llog_error(cdev, "fuse channel shut down on cdev %p", cdev);
		fuse_session_exit(se);
		return 0;
	} else if (res < 0) {
		/* ENOENT is returned if the operation was interrupted, it's
		 * safe to restart */
		if (errno == ENOENT)
			goto restart;

		/* ENODEV is returned if the FS got unmounted. This shouldn't
		 * occur for CUSE devices. Anyway, exit if this happens. */
		if (errno == ENODEV) {
			llog_error(cdev, "fuse channel unmounted on cdev %p",
				   cdev);
			fuse_session_exit(se);
			return 0;
		}

		/* EINTR and EAGAIN are simply forwarded to the caller. */
		if (errno == EINTR || errno == EAGAIN)
			return -errno;

		cdev->error = -errno;
		llog_error(cdev, "fuse channel read error on cdev %p (%d): %m",
			   cdev, errno);
		fuse_session_exit(se);
		return cdev->error;
	}

	return res;
}

static int chan_send(struct fuse_chan *ch, const struct iovec iov[],
		     size_t count)
{
	struct uvt_cdev *cdev = fuse_chan_data(ch);
	struct fuse_session *se = fuse_chan_session(ch);
	int fd = fuse_chan_fd(ch);
	int ret;

	if (!cdev || !se)
		return -EINVAL;
	if (!iov || !count)
		return 0;

	ret = writev(fd, iov, count);
	if (ret < 0) {
		/* ENOENT is returned on interrupts */
		if (!fuse_session_exited(se) && errno != ENOENT) {
			cdev->error = -errno;
			llog_error(cdev, "cannot write to fuse-channel on cdev %p (%d): %m",
				   cdev, errno);
			fuse_session_exit(se);
		}
		return cdev->error;
	}

	return 0;
}

static const struct fuse_chan_ops chan_ops = {
	.receive = chan_receive,
	.send = chan_send,
	.destroy = NULL,
};

/*
 * Character Device
 * This creates the high-level character-device driver and registers a
 * fake-session that is used to control each character file.
 * channel_event() is a callback when I/O is possible on the FUSE FD and
 * performs all outstanding tasks.
 * On error, the fake-session is unregistered and deleted. This also stops all
 * client sessions, obviously.
 */

static void uvt_cdev_hup(struct uvt_cdev *cdev, int error)
{
	struct uvt_cdev_event ev;

	ev_eloop_rm_fd(cdev->efd);
	cdev->efd = NULL;
	cdev->error = error;

	memset(&ev, 0, sizeof(ev));
	ev.type = UVT_CDEV_HUP;

	shl_hook_call(cdev->hook, cdev, &ev);
}

static void channel_event(struct ev_fd *fd, int mask, void *data)
{
	struct uvt_cdev *cdev = data;
	int ret;
	struct fuse_buf buf;
	struct fuse_chan *ch;
	struct shl_dlist *iter;
	struct uvt_client *client;

	if (!(mask & EV_READABLE)) {
		if (mask & (EV_HUP | EV_ERR)) {
			llog_error(cdev, "HUP/ERR on fuse channel on cdev %p",
				   cdev);
			uvt_cdev_hup(cdev, -EPIPE);
		}

		return;
	}

	memset(&buf, 0, sizeof(buf));
	buf.mem = cdev->buf;
	buf.size = cdev->bufsize;
	ch = cdev->channel;
	ret = fuse_session_receive_buf(cdev->session, &buf, &ch);
	if (ret == -EINTR || ret == -EAGAIN) {
		return;
	} else if (ret < 0) {
		llog_error(cdev, "fuse channel read error on cdev %p: %d",
			   cdev, ret);
		uvt_cdev_hup(cdev, ret);
		return;
	}

	fuse_session_process_buf(cdev->session, &buf, ch);
	if (fuse_session_exited(cdev->session)) {
		llog_error(cdev, "fuse session exited on cdev %p", cdev);
		uvt_cdev_hup(cdev, cdev->error ? : -EFAULT);
		return;
	}

	/* Readers can get interrupted asynchronously. Due to heavy locking
	 * inside of FUSE, we cannot release them right away. So cleanup all
	 * killed readers after we processed all buffers. */
	shl_dlist_for_each(iter, &cdev->clients) {
		client = shl_dlist_entry(iter, struct uvt_client, list);
		uvt_client_cleanup(client);
	}
}

static int uvt_cdev_init(struct uvt_cdev *cdev, const char *name,
			 unsigned int major, unsigned int minor)
{
	const char *dev_info_argv[1];
	struct cuse_info ci;
	size_t bufsize;
	char *nparam;
	int ret;

	/* TODO: libfuse makes sure that fd 0, 1 and 2 are available as
	 * standard streams, otherwise they fail. This is awkward and we
	 * should check whether this is really needed and _why_?
	 * If it is needed, fix upstream to stop that crazy! */

	if (!major)
		major = TTY_MAJOR;

	if (!major || major > 255) {
		llog_error(cdev, "invalid major %u on cdev %p",
			   major, cdev);
		return -EINVAL;
	}
	if (!minor) {
		llog_error(cdev, "invalid minor %u on cdev %p",
			   minor, cdev);
		return -EINVAL;
	}
	if (!name || !*name) {
		llog_error(cdev, "empty name on cdev %p",
			   cdev);
		return -EINVAL;
	}

	llog_info(cdev, "creating device /dev/%s %u:%u on cdev %p",
		  name, major, minor, cdev);

	ret = asprintf(&nparam, "DEVNAME=%s", name);
	if (ret <= 0)
		return llog_ENOMEM(cdev);

	dev_info_argv[0] = nparam;
	memset(&ci, 0, sizeof(ci));
	ci.dev_major = major;
	ci.dev_minor = minor;
	ci.dev_info_argc = 1;
	ci.dev_info_argv = dev_info_argv;
	ci.flags = CUSE_UNRESTRICTED_IOCTL;

	cdev->session = cuse_lowlevel_new(NULL, &ci, &ll_ops, cdev);
	free(nparam);

	if (!cdev->session) {
		llog_error(cdev, "cannot create fuse-ll session on cdev %p",
			   cdev);
		return -ENOMEM;
	}

	cdev->fd = open(cdev->ctx->cuse_file, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (cdev->fd < 0) {
		llog_error(cdev, "cannot open cuse-file %s on cdev %p (%d): %m",
			   cdev->ctx->cuse_file, cdev, errno);
		ret = -EFAULT;
		goto err_session;
	}

	bufsize = getpagesize() + 0x1000;
	if (bufsize < 0x21000)
		bufsize = 0x21000;

	cdev->bufsize = bufsize;
	cdev->buf = malloc(bufsize);
	if (!cdev->buf) {
		ret = llog_ENOMEM(cdev);
		goto err_fd;
	}

	/* Argh! libfuse does not use "const" for the "chan_ops" pointer so we
	 * actually have to cast it. Their implementation does not write into it
	 * so we can safely use a constant storage for it.
	 * TODO: Fix libfuse upstream! */
	cdev->channel = fuse_chan_new((void*)&chan_ops, cdev->fd, bufsize,
				      cdev);
	if (!cdev->channel) {
		llog_error(cdev, "cannot allocate fuse-channel on cdev %p",
			   cdev);
		ret = -ENOMEM;
		goto err_buf;
	}

	ret = ev_eloop_new_fd(cdev->ctx->eloop, &cdev->efd, cdev->fd,
			      EV_READABLE, channel_event, cdev);
	if (ret)
		goto err_chan;

	fuse_session_add_chan(cdev->session, cdev->channel);
	return 0;

err_chan:
	fuse_chan_destroy(cdev->channel);
err_buf:
	free(cdev->buf);
err_fd:
	close(cdev->fd);
err_session:
	fuse_session_destroy(cdev->session);
	return ret;
}

static void uvt_cdev_destroy(struct uvt_cdev *cdev)
{
	if (cdev->error)
		llog_warning(cdev, "cdev %p failed with error %d",
			     cdev, cdev->error);

	fuse_session_destroy(cdev->session);
	ev_eloop_rm_fd(cdev->efd);
	free(cdev->buf);
	close(cdev->fd);
}

SHL_EXPORT
int uvt_cdev_new(struct uvt_cdev **out, struct uvt_ctx *ctx,
		 const char *name, unsigned int major, unsigned int minor)
{
	struct uvt_cdev *cdev;
	int ret;

	if (!ctx)
		return -EINVAL;
	if (!out)
		return llog_EINVAL(ctx);

	cdev = malloc(sizeof(*cdev));
	if (!cdev)
		return llog_ENOMEM(ctx);
	memset(cdev, 0, sizeof(*cdev));
	cdev->ref = 1;
	cdev->ctx = ctx;
	cdev->llog = ctx->llog;
	cdev->llog_data = ctx->llog_data;
	shl_dlist_init(&cdev->clients);

	llog_debug(cdev, "new cdev %p on ctx %p", cdev, cdev->ctx);

	ret = shl_hook_new(&cdev->hook);
	if (ret)
		goto err_free;

	ret = uvt_cdev_init(cdev, name, major, minor);
	if (ret)
		goto err_hook;

	uvt_ctx_ref(cdev->ctx);
	*out = cdev;
	return 0;

err_hook:
	shl_hook_free(cdev->hook);
err_free:
	free(cdev);
	return ret;
}

SHL_EXPORT
void uvt_cdev_ref(struct uvt_cdev *cdev)
{
	if (!cdev || !cdev->ref)
		return;

	++cdev->ref;
}

SHL_EXPORT
void uvt_cdev_unref(struct uvt_cdev *cdev)
{
	if (!cdev || !cdev->ref || --cdev->ref)
		return;

	llog_debug(cdev, "free cdev %p", cdev);

	uvt_cdev_destroy(cdev);
	shl_hook_free(cdev->hook);
	uvt_ctx_unref(cdev->ctx);
	free(cdev);
}

SHL_EXPORT
int uvt_cdev_register_cb(struct uvt_cdev *cdev, uvt_cdev_cb cb, void *data)
{
	if (!cdev)
		return -EINVAL;

	return shl_hook_add_cast(cdev->hook, cb, data, false);
}

SHL_EXPORT
void uvt_cdev_unregister_cb(struct uvt_cdev *cdev, uvt_cdev_cb cb, void *data)
{
	if (!cdev)
		return;

	shl_hook_rm_cast(cdev->hook, cb, data);
}
