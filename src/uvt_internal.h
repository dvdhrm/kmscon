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
 * Userspace Virtual Terminals Internals
 * Internal header of the UVT implementation.
 */

#ifndef UVT_INTERNAL_H
#define UVT_INTERNAL_H

#include <eloop.h>
#include <stdlib.h>
#include <uvt.h>
#include "shl_array.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_llog.h"

#include <fuse/fuse.h>
#include <fuse/fuse_common.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#include <fuse/cuse_lowlevel.h>

/* contexts */

struct uvt_ctx {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;
	struct ev_eloop *eloop;

	char *cuse_file;
	unsigned int major;
	unsigned int minor_offset;
	struct shl_array *minors;
};

/* character devices */

struct uvt_cdev {
	unsigned long ref;
	struct uvt_ctx *ctx;
	llog_submit_t llog;
	void *llog_data;

	int error;
	struct shl_hook *hook;

	struct fuse_session *session;
	int fd;
	struct fuse_chan *channel;
	struct ev_fd *efd;

	size_t bufsize;
	char *buf;

	struct shl_dlist clients;
};

/* client sessions */

struct uvt_client {
	unsigned long ref;
	struct shl_dlist list;
	struct uvt_cdev *cdev;
	llog_submit_t llog;
	void *llog_data;

	struct fuse_pollhandle *ph;
	struct shl_dlist waiters;

	const struct uvt_vt_ops *vt;
	void *vt_data;
	bool vt_locked;
	bool vt_in_unlock;
	unsigned int vt_retry;
};

void uvt_client_cleanup(struct uvt_client *client);

int uvt_client_ll_open(struct uvt_client **out, struct uvt_cdev *cdev,
		       fuse_req_t req, struct fuse_file_info *fi);
void uvt_client_ll_release(fuse_req_t req, struct fuse_file_info *fi);
void uvt_client_ll_read(fuse_req_t req, size_t size, off_t off,
			struct fuse_file_info *fi);
void uvt_client_ll_write(fuse_req_t req, const char *buf, size_t size,
			 off_t off, struct fuse_file_info *fi);
void uvt_client_ll_poll(fuse_req_t req, struct fuse_file_info *fi,
			struct fuse_pollhandle *ph);
void uvt_client_ll_ioctl(fuse_req_t req, int cmd, void *arg,
			 struct fuse_file_info *fi, unsigned int flags,
			 const void *in_buf, size_t in_bufsz, size_t out_bufsz);

#endif /* UVT_INTERNAL_H */
