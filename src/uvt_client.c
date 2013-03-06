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
 * Client Sessions
 * A client session represents the internal object that corresponds to a single
 * open-file in the kernel. That is, for each user calling open() on a cdev, we
 * create a client-session in UVT.
 * Note that multiple client-sessions can share the same VT object. It is up to
 * the API user to assign clients to the correct VTs. You can even move clients
 * from one VT to another.
 * On the other hand, user-space can have multiple FDs open for a single
 * client-session similar to how they can have multiple FDs for a single
 * open-file.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <termio.h>
#include <termios.h>
#include <unistd.h>
#include "shl_dlist.h"
#include "shl_llog.h"
#include "shl_misc.h"
#include "uvt.h"
#include "uvt_internal.h"

#define LLOG_SUBSYSTEM "uvt_client"

/*
 * Blocking Waiters
 * I/O has always two modes: blocking and nonblocking
 * Nonblocking I/O is easy. We simply check whether we can actually forward the
 * data. If we can't, we signal that back. However, blocking I/O is a lot more
 * complex to implement. If a user submits a blocking I/O call, we have to wait
 * until we can finish that request. In the kernel we simply put the user
 * context asleep until we call can finish. However, in user-space via FUSE we
 * have no user-context. Instead, we need to work around that.
 * The most straightforward way would be to create a thread and put that thread
 * asleep. However, this would create one thread for every blocking I/O call
 * which seems to be way too much overhead. Also, we don't want threads in a
 * library. Therefore, we use a different approach.
 * For each blocking request, we create a uvt_waiter. This waiter is then linked
 * into the waiter list and we continue with other requests. Everytime the I/O
 * status changes, we retry the whole waiter list and try to finish the
 * requests. If a request is done, we signal it back and destroy the waiter.
 * This gets slightly more complex with interrupts and fuse_req objects. See
 * below for the implementation.
 */

enum uvt_waiter_type {
	UVT_WAITER_INVALID		= 0x00,

	UVT_WAITER_READ			= 0x01,
	UVT_WAITER_WRITE		= 0x02,

	UVT_WAITER_ALL			= UVT_WAITER_READ |
					  UVT_WAITER_WRITE,
};

enum uvt_waiter_flags {
	UVT_WAITER_KILLED		= 0x01,
	UVT_WAITER_RELEASED		= 0x02,
};

struct uvt_waiter {
	struct shl_dlist list;
	struct uvt_client *client;
	unsigned int flags;
	fuse_req_t req;

	unsigned int type;

	union {
		struct {
			size_t size;
			uint8_t *buf;
		} read;

		struct {
			size_t size;
			uint8_t *buf;
		} write;
	};
};

static bool uvt_waiter_is_killed(struct uvt_waiter *waiter)
{
	return !waiter || (waiter->flags & UVT_WAITER_KILLED);
}

static void uvt_waiter_set_killed(struct uvt_waiter *waiter)
{
	if (waiter)
		waiter->flags |= UVT_WAITER_KILLED;
}

static bool uvt_waiter_is_released(struct uvt_waiter *waiter)
{
	return !waiter || (waiter->flags & UVT_WAITER_RELEASED);
}

static void uvt_waiter_set_released(struct uvt_waiter *waiter)
{
	if (waiter)
		waiter->flags |= UVT_WAITER_RELEASED;
}

static void uvt_waiter_interrupt(fuse_req_t req, void *data)
{
	struct uvt_waiter *waiter = data;

	uvt_waiter_set_killed(waiter);
}

static int uvt_waiter_new(struct uvt_waiter **out, struct uvt_client *client,
			  fuse_req_t req)
{
	struct uvt_waiter *waiter;

	if (!client->vt)
		return -EPIPE;
	if (fuse_req_interrupted(req))
		return -ENOENT;

	waiter = malloc(sizeof(*waiter));
	if (!waiter)
		return -ENOMEM;
	memset(waiter, 0, sizeof(*waiter));
	waiter->client = client;
	waiter->flags = 0;
	waiter->req = req;

	fuse_req_interrupt_func(req, uvt_waiter_interrupt, waiter);
	if (uvt_waiter_is_killed(waiter)) {
		fuse_req_interrupt_func(req, NULL, NULL);
		free(waiter);
		return -ENOENT;
	}

	shl_dlist_link_tail(&client->waiters, &waiter->list);
	*out = waiter;
	return 0;
}

static int uvt_waiter_new_read(struct uvt_waiter **out,
			       struct uvt_client *client, fuse_req_t req,
			       uint8_t *buf, size_t size)
{
	struct uvt_waiter *waiter;
	int ret;

	if (!size)
		return -EINVAL;

	ret = uvt_waiter_new(&waiter, client, req);
	if (ret)
		return ret;
	waiter->type = UVT_WAITER_READ;
	waiter->read.size = size;
	waiter->read.buf = buf;

	*out = waiter;
	return 0;
}

static int uvt_waiter_new_write(struct uvt_waiter **out,
				struct uvt_client *client, fuse_req_t req,
				const uint8_t *mem, size_t size)
{
	struct uvt_waiter *waiter;
	uint8_t *buf;
	int ret;

	if (!size)
		return -EINVAL;

	buf = malloc(size);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, mem, size);

	ret = uvt_waiter_new(&waiter, client, req);
	if (ret)
		goto err_free;
	waiter->type = UVT_WAITER_WRITE;
	waiter->write.size = size;
	waiter->write.buf = buf;

	*out = waiter;
	return 0;

err_free:
	free(buf);
	return ret;
}

static void uvt_waiter_release(struct uvt_waiter *waiter, int error)
{
	if (!waiter || uvt_waiter_is_released(waiter))
		return;

	uvt_waiter_set_released(waiter);
	fuse_req_interrupt_func(waiter->req, NULL, NULL);
	if (error)
		fuse_reply_err(waiter->req, abs(error));
}

static void uvt_waiter_free(struct uvt_waiter *waiter, int error)
{
	shl_dlist_unlink(&waiter->list);
	uvt_waiter_release(waiter, error);

	switch (waiter->type) {
	case UVT_WAITER_READ:
		free(waiter->read.buf);
		break;
	case UVT_WAITER_WRITE:
		free(waiter->write.buf);
		break;
	}

	free(waiter);
}

static void uvt_waiter_free_read(struct uvt_waiter *waiter, size_t len)
{
	if (!waiter)
		return;

	if (!uvt_waiter_is_released(waiter)) {
		uvt_waiter_release(waiter, 0);
		fuse_reply_buf(waiter->req, (void*)waiter->read.buf, len);
	}
	uvt_waiter_free(waiter, -EINVAL);
}

static void uvt_waiter_free_write(struct uvt_waiter *waiter, size_t len)
{
	if (!waiter)
		return;

	if (!uvt_waiter_is_released(waiter)) {
		uvt_waiter_release(waiter, 0);
		fuse_reply_write(waiter->req, len);
	}
	uvt_waiter_free(waiter, -EINVAL);
}

/*
 * Client Sessions
 * A client session is the user-space counterpart of kernel-space open-files.
 * For each open-file we have one client-session in user-space. Users can access
 * a single client-session via multiple file-descriptors via dup(). However, for
 * each open() call on the device, we create a new open-file, that is, a new
 * client-session.
 * A single client session dispatches all the I/O calls on the file. It does
 * blocking and nonblocking I/O, parses ioctls() and correctly performs any
 * other state-tracking. But it does not implement any device logic. That means,
 * the client-session doesn't provide any functionality. Instead, you have to
 * assign a VT to the session. The client-session performs any maintenance tasks
 * and then forwards the requests to the VT object. If no VT object is assigned,
 * the user gets ENODEV as error.
 * Because the client-session performs all state-tracking and parsing, the VT
 * object can be a lot simpler and doesn't have to be aware of any FUSE objects
 * or sessions. Instead, the VT object can concentrate on implementing a _VT_
 * and nothing more.
 * Furthermore, this allows to assign the same VT object to multiple different
 * sessions at the same time. Or to assign a different VT to each session on the
 * same device, or any other combination you want.
 */

static void uvt_client_waiters_retry(struct uvt_client *client,
				     unsigned int types);

static int uvt_client_new(struct uvt_client **out, struct uvt_cdev *cdev)
{
	struct uvt_client *client;

	if (!cdev)
		return -EINVAL;
	if (!out)
		return llog_EINVAL(cdev);

	client = malloc(sizeof(*client));
	if (!client)
		return llog_ENOMEM(cdev);
	memset(client, 0, sizeof(*client));
	client->ref = 1;
	client->cdev = cdev;
	client->llog = cdev->llog;
	client->llog_data = cdev->llog_data;
	shl_dlist_init(&client->waiters);

	llog_debug(client, "new client %p on cdev %p", client, cdev);

	shl_dlist_link_tail(&cdev->clients, &client->list);
	*out = client;
	return 0;
}

SHL_EXPORT
void uvt_client_ref(struct uvt_client *client)
{
	if (!client || !client->ref)
		return;

	++client->ref;
}

SHL_EXPORT
void uvt_client_unref(struct uvt_client *client)
{
	if (!client || !client->ref || --client->ref)
		return;

	llog_debug(client, "free client %p", client);

	uvt_client_kill(client);
	free(client);
}

/*
 * This must be called after each event dispatch round. It cleans up all
 * interrupted/killed readers. The readers cannot be released right away due
 * to heavy locking inside of FUSE. We have to delay these tasks and clean up
 * after each dispatch round.
 */
void uvt_client_cleanup(struct uvt_client *client)
{
	struct shl_dlist *i, *tmp;
	struct uvt_waiter *waiter;

	if (!client)
		return;

	shl_dlist_for_each_safe(i, tmp, &client->waiters) {
		waiter = shl_dlist_entry(i, struct uvt_waiter, list);
		if (uvt_waiter_is_killed(waiter))
			uvt_waiter_free(waiter, -ENOENT);
	}
}

static void uvt_client_waiters_release(struct uvt_client *client, int error)
{
	struct uvt_waiter *waiter;
	int err;

	if (!client)
		return;

	while (!shl_dlist_empty(&client->waiters)) {
		waiter = shl_dlist_entry(client->waiters.next,
					 struct uvt_waiter, list);

		if (uvt_waiter_is_killed(waiter))
			err = -ENOENT;
		else
			err = error;

		uvt_waiter_free(waiter, err);
	}
}

SHL_EXPORT
bool uvt_client_is_dead(struct uvt_client *client)
{
	return !client || !client->cdev;
}

SHL_EXPORT
void uvt_client_kill(struct uvt_client *client)
{
	if (!client || !client->cdev)
		return;

	llog_debug(client, "kill client %p", client);

	if (client->ph) {
		fuse_notify_poll(client->ph);
		fuse_pollhandle_destroy(client->ph);
		client->ph = NULL;
	}

	shl_dlist_unlink(&client->list);
	client->cdev = NULL;
	uvt_client_set_vt(client, NULL, NULL);
	uvt_client_waiters_release(client, -EPIPE);
}

/*
 * We allow recursive VT-actions so we need sophisticated locking. That is, we
 * allow each client->vt->XY() function to itself raise VT events. These VT
 * events cause our uvt_client_vt_event() handler to call
 * uvt_client_waiters_retry(). But uvt_client_waiters_retry() itself can call
 * VT functions again.
 * This recursion isn't particularly bad, as any _proper_ implementation would
 * have an upper limit (which is the number of active waiters). However, to
 * avoid wasting stack space for recursion, we lock the VT when calling VT
 * callbacks. The uvt_client_vt_event() handler checks whether the callbacks are
 * currently locked and sets markers otherwise. These markers cause our
 * unlock-function to notice that we got events in between and then retries all
 * interrupted operations.
 * The client->vt_in_unlock is used to avoid recursion in unlock() itself.
 */

static bool uvt_client_lock_vt(struct uvt_client *client)
{
	if (!client || client->vt_locked)
		return false;

	client->vt_locked = true;
	return true;
}

static void uvt_client_unlock_vt(struct uvt_client *client)
{
	unsigned int retry;

	if (!client || !client->vt_locked)
		return;

	client->vt_locked = false;
	if (client->vt_in_unlock)
		return;

	while (client->vt_retry) {
		retry = client->vt_retry;
		client->vt_retry = 0;

		client->vt_in_unlock = true;
		uvt_client_waiters_retry(client, retry);
		client->vt_in_unlock = false;
	}
}

static void uvt_client_waiters_retry(struct uvt_client *client,
				     unsigned int types)
{
	struct shl_dlist *iter, *tmp;
	struct uvt_waiter *waiter;
	int ret;

	if (!client || !types || uvt_client_is_dead(client) || !client->vt)
		return;

	if (!uvt_client_lock_vt(client))
		return;

	shl_dlist_for_each_safe(iter, tmp, &client->waiters) {
		if (!types)
			break;

		waiter = shl_dlist_entry(iter, struct uvt_waiter, list);
		if (!(waiter->type & types) || uvt_waiter_is_killed(waiter))
			continue;

		if (waiter->type == UVT_WAITER_READ) {
			ret = client->vt->read(client->vt_data,
					       waiter->read.buf,
					       waiter->read.size);
			if (ret == -EAGAIN) {
				types &= ~UVT_WAITER_READ;
				continue;
			} else if (ret < 0) {
				uvt_waiter_free(waiter, ret);
			} else {
				if (ret > waiter->read.size)
					ret = waiter->read.size;
				uvt_waiter_free_read(waiter, ret);
			}
		} else if (waiter->type == UVT_WAITER_WRITE) {
			ret = client->vt->write(client->vt_data,
						waiter->write.buf,
						waiter->write.size);
			if (ret == -EAGAIN) {
				types &= ~UVT_WAITER_WRITE;
				continue;
			} else if (ret < 0) {
				uvt_waiter_free(waiter, ret);
			} else {
				if (ret > waiter->write.size)
					ret = waiter->write.size;
				uvt_waiter_free_write(waiter, ret);
			}
		}
	}

	uvt_client_unlock_vt(client);
}

static void uvt_client_vt_event(void *vt, struct uvt_vt_event *ev, void *data)
{
	struct uvt_client *client = data;

	if (uvt_client_is_dead(client))
		return;

	switch (ev->type) {
	case UVT_VT_HUP:
		uvt_client_kill(client);
		break;
	case UVT_VT_TTY:
		switch (ev->tty.type) {
		case UVT_TTY_HUP:
			uvt_client_kill(client);
			break;
		case UVT_TTY_READ:
			if (client->ph)
				fuse_notify_poll(client->ph);
			client->vt_retry |= UVT_WAITER_READ;
			break;
		case UVT_TTY_WRITE:
			if (client->ph)
				fuse_notify_poll(client->ph);
			client->vt_retry |= UVT_WAITER_WRITE;
			break;
		}
		break;
	}

	uvt_client_waiters_retry(client, client->vt_retry);
}

SHL_EXPORT
int uvt_client_set_vt(struct uvt_client *client, const struct uvt_vt_ops *vt,
		      void *vt_data)
{
	int ret;

	if (!client)
		return -EINVAL;
	if (uvt_client_is_dead(client) && vt)
		return -EINVAL;

	if (client->vt) {
		client->vt->unregister_cb(client->vt_data, uvt_client_vt_event,
					  client);
		client->vt->unref(client->vt_data);
	}

	client->vt = vt;
	client->vt_data = vt_data;

	if (client->vt) {
		ret = client->vt->register_cb(client->vt_data,
					      uvt_client_vt_event, client);
		if (!ret) {
			client->vt->ref(client->vt_data);
			uvt_client_waiters_retry(client, UVT_WAITER_ALL);
			return 0;
		}
	} else {
		ret = 0;
	}

	client->vt = NULL;
	client->vt_data = NULL;
	uvt_client_waiters_release(client, -ENODEV);
	return ret;
}

/*
 * Internal FUSE low-level fops implementation
 * These functions implement the callbacks used by the CUSE/FUSE-ll
 * implementation in uvt_cdev objects. Our infrastructure allows to provide
 * other callbacks, too, but this is currently not needed. Moreover, I cannot
 * see any reason to add them to the public API as nobody would want anything
 * different than CUSE/FUSE as frontend.
 */

int uvt_client_ll_open(struct uvt_client **out, struct uvt_cdev *cdev,
		       fuse_req_t req, struct fuse_file_info *fi)
{
	struct uvt_client *client;
	int ret;

	ret = uvt_client_new(&client, cdev);
	if (ret) {
		fuse_reply_err(req, -ret);
		return ret;
	}

	fi->fh = (uint64_t)(uintptr_t)(void*)client;
	fi->nonseekable = 1;
	fi->direct_io = 1;
	ret = fuse_reply_open(req, fi);
	if (ret < 0) {
		uvt_client_kill(client);
		uvt_client_unref(client);
		return -EFAULT;
	}

	*out = client;
	return 0;
}

void uvt_client_ll_release(fuse_req_t req, struct fuse_file_info *fi)
{
	struct uvt_client *client = (void*)(uintptr_t)fi->fh;

	if (!client) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	uvt_client_kill(client);
	uvt_client_unref(client);
	fuse_reply_err(req, 0);
}

void uvt_client_ll_read(fuse_req_t req, size_t size, off_t off,
			struct fuse_file_info *fi)
{
	struct uvt_client *client = (void*)(uintptr_t)fi->fh;
	struct uvt_waiter *waiter;
	uint8_t *buf;
	int ret;

	if (!client) {
		fuse_reply_err(req, EINVAL);
		return;
	} else if (uvt_client_is_dead(client)) {
		fuse_reply_err(req, EPIPE);
		return;
	} else if (off) {
		fuse_reply_err(req, EINVAL);
		return;
	} else if (!size) {
		fuse_reply_buf(req, "", 0);
		return;
	} else if (!client->vt) {
		fuse_reply_err(req, ENODEV);
		return;
	}

	buf = malloc(size);
	if (!buf) {
		fuse_reply_err(req, ENOMEM);
		return;
	}

	ret = client->vt->read(client->vt_data, buf, size);
	if (ret >= 0) {
		if (ret > size)
			ret = size;

		fuse_reply_buf(req, (void*)buf, ret);
		free(buf);
		return;
	} else if (ret == -EAGAIN && !(fi->flags & O_NONBLOCK)) {
		ret = uvt_waiter_new_read(&waiter, client, req, buf, size);
		if (!ret)
			return;
	}

	fuse_reply_err(req, -ret);
	free(buf);
}

void uvt_client_ll_write(fuse_req_t req, const char *buf, size_t size,
			 off_t off, struct fuse_file_info *fi)
{
	struct uvt_client *client = (void*)(uintptr_t)fi->fh;
	struct uvt_waiter *waiter;
	int ret;

	if (!client) {
		fuse_reply_err(req, EINVAL);
		return;
	} else if (uvt_client_is_dead(client)) {
		fuse_reply_err(req, EPIPE);
		return;
	} else if (off) {
		fuse_reply_err(req, EINVAL);
		return;
	} else if (!size) {
		fuse_reply_write(req, 0);
		return;
	} else if (!client->vt) {
		fuse_reply_err(req, ENODEV);
		return;
	}

	ret = client->vt->write(client->vt_data, (void*)buf, size);
	if (ret >= 0) {
		if (ret > size)
			ret = size;

		fuse_reply_write(req, ret);
		return;
	} else if (ret == -EAGAIN && !(fi->flags & O_NONBLOCK)) {
		ret = uvt_waiter_new_write(&waiter, client, req, (void*)buf,
					   size);
		if (!ret)
			return;
	}

	fuse_reply_err(req, -ret);
}

void uvt_client_ll_poll(fuse_req_t req, struct fuse_file_info *fi,
			struct fuse_pollhandle *ph)
{
	struct uvt_client *client = (void*)(uintptr_t)fi->fh;
	unsigned int flags, fl;

	if (!client) {
		fuse_reply_err(req, EINVAL);
		return;
	} else if (uvt_client_is_dead(client)) {
		if (ph)
			fuse_pollhandle_destroy(ph);
		fuse_reply_poll(req, EPOLLHUP | EPOLLIN | EPOLLOUT |
				     EPOLLWRNORM | EPOLLRDNORM);
		return;
	}

	if (client->ph)
		fuse_pollhandle_destroy(client->ph);
	client->ph = ph;

	if (!client->vt) {
		fuse_reply_err(req, ENODEV);
		return;
	}

	flags = 0;
	fl = client->vt->poll(client->vt_data);
	if (fl & UVT_TTY_HUP)
		flags |= EPOLLHUP;
	if (fl & UVT_TTY_READ)
		flags |= EPOLLIN | EPOLLRDNORM;
	if (fl & UVT_TTY_WRITE)
		flags |= EPOLLOUT | EPOLLWRNORM;

	fuse_reply_poll(req, flags);
}

static bool ioctl_param(fuse_req_t req, void *arg, size_t in_want,
			size_t in_have, size_t out_want, size_t out_have)
{
	bool retry;
	struct iovec in, out;
	size_t in_num, out_num;

	retry = false;
	memset(&in, 0, sizeof(in));
	in_num = 0;
	memset(&out, 0, sizeof(out));
	out_num = 0;

	if (in_want) {
		if (!in_have) {
			retry = true;
		} else if (in_have < in_want) {
			fuse_reply_err(req, EFAULT);
			return true;
		}

		in.iov_base = arg;
		in.iov_len = in_want;
		in_num = 1;
	}
	if (out_want) {
		if (!out_have) {
			retry = true;
		} else if (out_have < out_want) {
			fuse_reply_err(req, EFAULT);
			return true;
		}

		out.iov_base = arg;
		out.iov_len = out_want;
		out_num = 1;
	}

	if (retry)
		fuse_reply_ioctl_retry(req, in_num ? &in : NULL, in_num,
				       out_num ? &out : NULL, out_num);
	return retry;
}

void uvt_client_ll_ioctl(fuse_req_t req, int cmd, void *arg,
			 struct fuse_file_info *fi, unsigned int flags,
			 const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
	struct uvt_client *client = (void*)(uintptr_t)fi->fh;
	uintptr_t uarg = (uintptr_t)arg;
	bool compat;
	int ret;
	struct vt_stat vtstat;
	struct vt_mode vtmode;
	unsigned int uval;

	if (!client) {
		fuse_reply_err(req, EINVAL);
		return;
	} else if (uvt_client_is_dead(client)) {
		fuse_reply_err(req, EPIPE);
		return;
	} else if (!client->vt) {
		fuse_reply_err(req, ENODEV);
		return;
	}

	/* TODO: fix compat-ioctls */
	compat = !!(flags & FUSE_IOCTL_COMPAT);
	if (compat) {
		fuse_reply_err(req, EOPNOTSUPP);
		return;
	}

	switch (cmd) {

	/* TTY ioctls */

	case TCFLSH:
		if (ioctl_param(req, arg, 0, in_bufsz, 0, out_bufsz))
			return;
		if (!client->vt->ioctl_TCFLSH) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_TCFLSH(client->vt_data,
						       (unsigned long)uarg);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case TIOCPKT:
	case TCXONC:
	case TCGETS:
	case TCSETS:
	case TCSETSF:
	case TCSETSW:
	case TCGETA:
	case TCSETA:
	case TCSETAF:
	case TCSETAW:
	case TIOCGLCKTRMIOS:
	case TIOCSLCKTRMIOS:
	case TCGETX:
	case TCSETX:
	case TCSETXW:
	case TCSETXF:
	case TIOCGSOFTCAR:
	case TIOCSSOFTCAR:
		fuse_reply_err(req, EOPNOTSUPP);
		break;

	/* VT ioctls */

	case VT_ACTIVATE:
		if (ioctl_param(req, arg, 0, in_bufsz, 0, out_bufsz))
			return;
		if (!client->vt->ioctl_VT_ACTIVATE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_VT_ACTIVATE(client->vt_data,
							(unsigned long)uarg);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case VT_WAITACTIVE:
		if (ioctl_param(req, arg, 0, in_bufsz, 0, out_bufsz))
			return;
		if (!client->vt->ioctl_VT_WAITACTIVE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_VT_WAITACTIVE(client->vt_data,
							(unsigned long)uarg);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case VT_GETSTATE:
		if (ioctl_param(req, arg, 0, in_bufsz,
				sizeof(struct vt_stat), out_bufsz))
			return;
		if (!client->vt->ioctl_VT_GETSTATE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			memset(&vtstat, 0, sizeof(vtstat));
			ret = client->vt->ioctl_VT_GETSTATE(client->vt_data,
							    &vtstat);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, &vtstat,
						 sizeof(vtstat));
		}
		break;

	case VT_OPENQRY:
		if (ioctl_param(req, arg, 0, in_bufsz,
				sizeof(unsigned int), out_bufsz))
			return;
		if (!client->vt->ioctl_VT_OPENQRY) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			uval = 0;
			ret = client->vt->ioctl_VT_OPENQRY(client->vt_data,
							   &uval);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, &uval, sizeof(uval));
		}
		break;

	case VT_GETMODE:
		if (ioctl_param(req, arg, 0, in_bufsz,
				sizeof(struct vt_mode), out_bufsz))
			return;
		if (!client->vt->ioctl_VT_GETMODE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			memset(&vtmode, 0, sizeof(vtmode));
			ret = client->vt->ioctl_VT_GETMODE(client->vt_data,
							   &vtmode);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, &vtmode,
						 sizeof(vtmode));
		}
		break;

	case VT_SETMODE:
		if (ioctl_param(req, arg, sizeof(struct vt_mode), in_bufsz,
				0, out_bufsz))
			return;
		if (!client->vt->ioctl_VT_SETMODE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_VT_SETMODE(client->vt_data,
						(const struct vt_mode*)in_buf,
						fuse_req_ctx(req)->pid);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case VT_RELDISP:
		if (ioctl_param(req, arg, 0, in_bufsz, 0, out_bufsz))
			return;
		if (!client->vt->ioctl_VT_RELDISP) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_VT_RELDISP(client->vt_data,
							(unsigned long)uarg);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case KDGETMODE:
		if (ioctl_param(req, arg, 0, in_bufsz,
				sizeof(unsigned int), out_bufsz))
			return;
		if (!client->vt->ioctl_KDGETMODE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			uval = 0;
			ret = client->vt->ioctl_KDGETMODE(client->vt_data,
							  &uval);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, &uval, sizeof(uval));
		}
		break;

	case KDSETMODE:
		if (ioctl_param(req, arg, 0, in_bufsz, 0, out_bufsz))
			return;
		if (!client->vt->ioctl_KDSETMODE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_KDSETMODE(client->vt_data,
							(unsigned int)uarg);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case KDGKBMODE:
		if (ioctl_param(req, arg, 0, in_bufsz,
				sizeof(unsigned int), out_bufsz))
			return;
		if (!client->vt->ioctl_KDGKBMODE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			uval = 0;
			ret = client->vt->ioctl_KDGKBMODE(client->vt_data,
							  &uval);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, &uval, sizeof(uval));
		}
		break;

	case KDSKBMODE:
		if (ioctl_param(req, arg, 0, in_bufsz, 0, out_bufsz))
			return;
		if (!client->vt->ioctl_KDSKBMODE) {
			fuse_reply_err(req, EOPNOTSUPP);
		} else {
			ret = client->vt->ioctl_KDSKBMODE(client->vt_data,
							(unsigned int)uarg);
			if (ret)
				fuse_reply_err(req, abs(ret));
			else
				fuse_reply_ioctl(req, 0, NULL, 0);
		}
		break;

	case TIOCLINUX:
	case KIOCSOUND:
	case KDMKTONE:
	case KDGKBTYPE:
	case KDADDIO:
	case KDDELIO:
	case KDENABIO:
	case KDDISABIO:
	case KDKBDREP:
	case KDMAPDISP:
	case KDUNMAPDISP:
	case KDGKBMETA:
	case KDSKBMETA:
	case KDGETKEYCODE:
	case KDSETKEYCODE:
	case KDGKBENT:
	case KDSKBENT:
	case KDGKBSENT:
	case KDSKBSENT:
	case KDGKBDIACR:
	case KDSKBDIACR:
	case KDGKBDIACRUC:
	case KDSKBDIACRUC:
	case KDGETLED:
	case KDSETLED:
	case KDGKBLED:
	case KDSKBLED:
	case KDSIGACCEPT:
	case VT_SETACTIVATE:
	case VT_DISALLOCATE:
	case VT_RESIZE:
	case VT_RESIZEX:
	case GIO_FONT:
	case PIO_FONT:
	case GIO_CMAP:
	case PIO_CMAP:
	case GIO_FONTX:
	case PIO_FONTX:
	case PIO_FONTRESET:
	case KDFONTOP:
	case GIO_SCRNMAP:
	case PIO_SCRNMAP:
	case GIO_UNISCRNMAP:
	case PIO_UNISCRNMAP:
	case PIO_UNIMAPCLR:
	case GIO_UNIMAP:
	case PIO_UNIMAP:
	case VT_LOCKSWITCH:
	case VT_UNLOCKSWITCH:
	case VT_GETHIFONTMASK:
	case VT_WAITEVENT:
		fuse_reply_err(req, EOPNOTSUPP);
		break;
	default:
		fuse_reply_err(req, EINVAL);
		break;
	}
}
