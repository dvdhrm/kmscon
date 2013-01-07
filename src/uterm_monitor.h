/*
 * uterm - Linux User-Space Terminal System Monitor
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * System Monitor
 * This watches the system for new seats, graphics devices or other devices that
 * are used by terminals.
 */

#ifndef UTERM_UTERM_MONITOR_H
#define UTERM_UTERM_MONITOR_H

#include <eloop.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

struct uterm_monitor;
struct uterm_monitor_seat;
struct uterm_monitor_dev;

enum uterm_monitor_event_type {
	UTERM_MONITOR_NEW_SEAT,
	UTERM_MONITOR_FREE_SEAT,
	UTERM_MONITOR_NEW_DEV,
	UTERM_MONITOR_FREE_DEV,
	UTERM_MONITOR_HOTPLUG_DEV,
};

enum uterm_monitor_dev_type {
	UTERM_MONITOR_DRM,
	UTERM_MONITOR_FBDEV,
	UTERM_MONITOR_INPUT,
};

enum uterm_monitor_dev_flag {
	UTERM_MONITOR_DRM_BACKED	= 0x01,
	UTERM_MONITOR_PRIMARY		= 0x02,
	UTERM_MONITOR_AUX		= 0x04,
};

struct uterm_monitor_event {
	unsigned int type;

	struct uterm_monitor_seat *seat;
	const char *seat_name;
	void *seat_data;

	struct uterm_monitor_dev *dev;
	unsigned int dev_type;
	unsigned int dev_flags;
	const char *dev_node;
	void *dev_data;
};

typedef void (*uterm_monitor_cb) (struct uterm_monitor *mon,
				  struct uterm_monitor_event *event,
				  void *data);

int uterm_monitor_new(struct uterm_monitor **out, struct ev_eloop *eloop,
		      uterm_monitor_cb cb, void *data);
void uterm_monitor_ref(struct uterm_monitor *mon);
void uterm_monitor_unref(struct uterm_monitor *mon);
void uterm_monitor_scan(struct uterm_monitor *mon);

void uterm_monitor_set_seat_data(struct uterm_monitor_seat *seat, void *data);
void uterm_monitor_set_dev_data(struct uterm_monitor_dev *dev, void *data);

#endif /* UTERM_UTERM_MONITOR_H */
