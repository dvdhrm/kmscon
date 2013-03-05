/*
 * uvtd - User-space VT daemon
 *
 * Copyright (c) 2013 David Herrmann <dh.herrmann@gmail.com>
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

#ifndef UVTD_VT_H
#define UVTD_VT_H

#include <inttypes.h>
#include <stdlib.h>
#include "uvt.h"

struct uvtd_vt;
extern struct uvt_vt_ops uvtd_vt_ops;

int uvtd_vt_new(struct uvtd_vt **out, struct uvt_ctx *uctx, unsigned int id,
		struct uvtd_seat *seat, bool is_legacy);
void uvtd_vt_ref(struct uvtd_vt *vt);
void uvtd_vt_unref(struct uvtd_vt *vt);

int uvtd_vt_register_cb(struct uvtd_vt *vt, uvt_vt_cb cb, void *data);
void uvtd_vt_unregister_cb(struct uvtd_vt *vt, uvt_vt_cb cb, void *data);

int uvtd_vt_read(struct uvtd_vt *vt, uint8_t *mem, size_t len);
int uvtd_vt_write(struct uvtd_vt *vt, const uint8_t *mem, size_t len);
unsigned int uvtd_vt_poll(struct uvtd_vt *vt);

#endif /* UVTD_VT_H */
