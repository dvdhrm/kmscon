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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "eloop.h"
#include "log.h"
#include "misc.h"
#include "uterm.h"
#include "uterm_internal.h"
#include "vt.h"

#define LOG_SUBSYSTEM "vt"

struct uterm_vt {
	unsigned long ref;
	struct kmscon_dlist list;
	struct uterm_vt_master *vtm;

	uterm_vt_cb cb;
	void *data;

	bool active;
	struct kmscon_vt *vt;
};

struct uterm_vt_master {
	unsigned long ref;
	struct ev_eloop *eloop;

	struct kmscon_dlist vts;
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

static bool vt_event(struct kmscon_vt *ovt, enum kmscon_vt_action action,
		     void *data)
{
	struct uterm_vt *vt = data;
	int ret;

	switch (action) {
	case KMSCON_VT_ENTER:
		ret = vt_call(vt, UTERM_VT_ACTIVATE);
		break;
	case KMSCON_VT_LEAVE:
		ret = vt_call(vt, UTERM_VT_DEACTIVATE);
		break;
	}

	if (ret)
		return false;

	return true;
}

static void vt_idle_event(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_vt *vt = data;

	ev_eloop_unregister_idle_cb(eloop, vt_idle_event, data);
	vt_call(vt, UTERM_VT_ACTIVATE);
}

int uterm_vt_allocate(struct uterm_vt_master *vtm,
		      struct uterm_vt **out,
		      const char *seat,
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

	if (!strcmp(seat, "seat0") && kmscon_vt_supported()) {
		ret = kmscon_vt_new(&vt->vt, vt_event, vt);
		if (ret)
			goto err_free;
		ret = kmscon_vt_open(vt->vt, KMSCON_VT_NEW, vt->vtm->eloop);
		if (ret) {
			kmscon_vt_unref(vt->vt);
			goto err_free;
		}
	} else {
		ret = ev_eloop_register_idle_cb(vtm->eloop, vt_idle_event,
						vt);
		if (ret)
			goto err_free;
	}

	kmscon_dlist_link(&vtm->vts, &vt->list);
	*out = vt;
	return 0;

err_free:
	free(vt);
	return ret;
}

void uterm_vt_deallocate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return;

	if (vt->vt) {
		kmscon_vt_close(vt->vt);
		kmscon_vt_unref(vt->vt);
	} else {
		ev_eloop_unregister_idle_cb(vt->vtm->eloop, vt_idle_event,
					    vt);
		vt_call(vt, UTERM_VT_DEACTIVATE);
	}
	kmscon_dlist_unlink(&vt->list);
	vt->vtm = NULL;
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

	if (vt->vt)
		return kmscon_vt_enter(vt->vt);
	else
		return -EFAULT;
}

int uterm_vt_deactivate(struct uterm_vt *vt)
{
	if (!vt || !vt->vtm)
		return -EINVAL;

	if (vt->vt)
		return kmscon_vt_leave(vt->vt);
	else
		return -EFAULT;
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
	kmscon_dlist_init(&vtm->vts);

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
		vt = kmscon_dlist_entry(vtm->vts.next,
					struct uterm_vt,
					list);
		uterm_vt_deallocate(vt);
	}

	ev_eloop_unref(vtm->eloop);
	free(vtm);
}
