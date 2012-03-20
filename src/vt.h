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
 * VT compatibility
 * If the kmscon application runs in a VT we need to react on VT switch events
 * to allow other applications to access the DRM. This is only needed as long as
 * we run in a VT. In the future we will be able to disable all VTs and run as
 * service daemon. We then need another way to switch between graphical
 * applications, though.
 */

#ifndef KMSCON_VT_H
#define KMSCON_VT_H

#include <stdbool.h>
#include <stdlib.h>

#include "eloop.h"

struct kmscon_vt;
typedef bool (*kmscon_vt_cb) (struct kmscon_vt *vt, int action, void *data);

enum kmscon_vt_action {
	KMSCON_VT_ENTER,
	KMSCON_VT_LEAVE,
};

enum kmscon_vt_id {
	KMSCON_VT_CUR = 0,
	KMSCON_VT_NEW = -1,
};

int kmscon_vt_new(struct kmscon_vt **out, kmscon_vt_cb cb, void *data);
void kmscon_vt_ref(struct kmscon_vt *vt);
void kmscon_vt_unref(struct kmscon_vt *vt);

int kmscon_vt_open(struct kmscon_vt *vt, int id, struct ev_eloop *eloop);
void kmscon_vt_close(struct kmscon_vt *vt);

int kmscon_vt_enter(struct kmscon_vt *vt);
int kmscon_vt_leave(struct kmscon_vt *vt);

#endif /* KMSCON_VT_H */
