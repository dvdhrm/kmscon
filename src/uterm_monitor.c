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
 * System Monitor
 * This uses systemd's login monitor to watch the system for new seats. When
 * udev reports new devices, this automatically assigns the device to the right
 * seat. Devices that are not associated to seats are ignored. If a device
 * changes seats it is automatically removed and added again.
 */

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "shl_dlist.h"
#include "static_misc.h"
#include "uterm.h"

#ifdef UTERM_HAVE_SYSTEMD
	#include <systemd/sd-login.h>
#endif

#define LOG_SUBSYSTEM "monitor"

struct uterm_monitor_dev {
	struct shl_dlist list;
	struct uterm_monitor_seat *seat;
	unsigned int type;
	char *node;
	void *data;
};

struct uterm_monitor_seat {
	struct shl_dlist list;
	struct uterm_monitor *mon;
	char *name;
	void *data;
	struct shl_dlist devices;
};

struct uterm_monitor {
	unsigned long ref;
	struct ev_eloop *eloop;
	uterm_monitor_cb cb;
	void *data;

#ifdef UTERM_HAVE_SYSTEMD
	sd_login_monitor *sd_mon;
	struct ev_fd *sd_mon_fd;
#endif

	struct udev *udev;
	struct udev_monitor *umon;
	struct ev_fd *umon_fd;

	struct shl_dlist seats;
};

static void monitor_new_seat(struct uterm_monitor *mon, const char *name);
static void monitor_free_seat(struct uterm_monitor_seat *seat);

#ifdef UTERM_HAVE_SYSTEMD

static void monitor_refresh_seats(struct uterm_monitor *mon)
{
	char **seats;
	int num, i;
	struct shl_dlist *iter, *tmp;
	struct uterm_monitor_seat *seat;

	num = sd_get_seats(&seats);
	if (num < 0) {
		log_warn("cannot read seat information from systemd: %d", num);
		return;
	}

	/* Remove all seats that are no longer present */
	shl_dlist_for_each_safe(iter, tmp, &mon->seats) {
		seat = shl_dlist_entry(iter, struct uterm_monitor_seat,
						list);
		for (i = 0; i < num; ++i) {
			if (!strcmp(seats[i], seat->name))
				break;
		}

		if (i < num)
			seats[i] = NULL;
		else
			monitor_free_seat(seat);
	}

	/* Add all new seats */
	for (i = 0; i < num; ++i) {
		if (seats[i])
			monitor_new_seat(mon, seats[i]);
		free(seats[i]);
	}

	free(seats);
}

static void monitor_sd_event(struct ev_fd *fd,
				int mask,
				void *data)
{
	struct uterm_monitor *mon = data;

	if (mask & (EV_HUP | EV_ERR)) {
		log_warn("systemd login monitor closed unexpectedly");
		return;
	}

	sd_login_monitor_flush(mon->sd_mon);
	ev_eloop_flush_fd(mon->eloop, mon->sd_mon_fd);
	monitor_refresh_seats(mon);
}

static void monitor_sd_poll(struct uterm_monitor *mon)
{
	monitor_sd_event(mon->sd_mon_fd, EV_READABLE, mon);
}

static int monitor_sd_init(struct uterm_monitor *mon)
{
	int ret, sfd;

	ret = sd_login_monitor_new("seat", &mon->sd_mon);
	if (ret) {
		errno = -ret;
		log_err("cannot create systemd login monitor (%d): %m", ret);
		return -EFAULT;
	}

	sfd = sd_login_monitor_get_fd(mon->sd_mon);
	if (sfd < 0) {
		log_err("cannot get systemd login monitor fd");
		ret = -EFAULT;
		goto err_sd;
	}

	ret = ev_eloop_new_fd(mon->eloop, &mon->sd_mon_fd, sfd, EV_READABLE,
				monitor_sd_event, mon);
	if (ret)
		goto err_sd;

	return 0;

err_sd:
	sd_login_monitor_unref(mon->sd_mon);
	return ret;
}

static void monitor_sd_deinit(struct uterm_monitor *mon)
{
	ev_eloop_rm_fd(mon->sd_mon_fd);
	sd_login_monitor_unref(mon->sd_mon);
}

#else /* !UTERM_HAVE_SYSTEMD */

static void monitor_refresh_seats(struct uterm_monitor *mon)
{
	if (shl_dlist_empty(&mon->seats))
		monitor_new_seat(mon, "seat0");
}

static void monitor_sd_poll(struct uterm_monitor *mon)
{
}

static int monitor_sd_init(struct uterm_monitor *mon)
{
	return 0;
}

static void monitor_sd_deinit(struct uterm_monitor *mon)
{
}

#endif /* UTERM_HAVE_SYSTEMD */

static void seat_new_dev(struct uterm_monitor_seat *seat,
				unsigned int type,
				const char *node)
{
	struct uterm_monitor_dev *dev;
	struct uterm_monitor_event ev;

	dev = malloc(sizeof(*dev));
	if (!dev)
		return;
	memset(dev, 0, sizeof(*dev));
	dev->seat = seat;
	dev->type = type;

	dev->node = strdup(node);
	if (!dev->node)
		goto err_free;

	shl_dlist_link(&seat->devices, &dev->list);

	memset(&ev, 0, sizeof(ev));
	ev.type = UTERM_MONITOR_NEW_DEV;
	ev.seat = dev->seat;
	ev.seat_name = dev->seat->name;
	ev.seat_data = dev->seat->data;
	ev.dev = dev;
	ev.dev_type = dev->type;
	ev.dev_node = dev->node;
	ev.dev_data = dev->data;
	dev->seat->mon->cb(dev->seat->mon, &ev, dev->seat->mon->data);

	log_debug("new device %s on %s", node, seat->name);
	return;

err_free:
	free(dev);
}

static void seat_free_dev(struct uterm_monitor_dev *dev)
{
	struct uterm_monitor_event ev;

	log_debug("free device %s on %s", dev->node, dev->seat->name);

	shl_dlist_unlink(&dev->list);

	memset(&ev, 0, sizeof(ev));
	ev.type = UTERM_MONITOR_FREE_DEV;
	ev.seat = dev->seat;
	ev.seat_name = dev->seat->name;
	ev.seat_data = dev->seat->data;
	ev.dev = dev;
	ev.dev_type = dev->type;
	ev.dev_node = dev->node;
	ev.dev_data = dev->data;
	dev->seat->mon->cb(dev->seat->mon, &ev, dev->seat->mon->data);

	free(dev->node);
	free(dev);
}

static struct uterm_monitor_dev *monitor_find_dev(struct uterm_monitor *mon,
						struct udev_device *dev)
{
	const char *node;
	struct shl_dlist *iter, *iter2;
	struct uterm_monitor_seat *seat;
	struct uterm_monitor_dev *sdev;

	node = udev_device_get_devnode(dev);
	if (!node)
		return NULL;

	shl_dlist_for_each(iter, &mon->seats) {
		seat = shl_dlist_entry(iter, struct uterm_monitor_seat,
						list);
		shl_dlist_for_each(iter2, &seat->devices) {
			sdev = shl_dlist_entry(iter2,
						struct uterm_monitor_dev,
						list);
			if (!strcmp(node, sdev->node))
				return sdev;
		}
	}

	return NULL;
}

static void monitor_new_seat(struct uterm_monitor *mon, const char *name)
{
	struct uterm_monitor_seat *seat;
	struct uterm_monitor_event ev;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return;
	memset(seat, 0, sizeof(*seat));
	seat->mon = mon;
	shl_dlist_init(&seat->devices);

	seat->name = strdup(name);
	if (!seat->name)
		goto err_free;

	shl_dlist_link(&mon->seats, &seat->list);

	memset(&ev, 0, sizeof(ev));
	ev.type = UTERM_MONITOR_NEW_SEAT;
	ev.seat = seat;
	ev.seat_name = seat->name;
	ev.seat_data = seat->data;
	seat->mon->cb(seat->mon, &ev, seat->mon->data);

	log_debug("new seat %s", name);
	return;

err_free:
	free(seat);
}

static void monitor_free_seat(struct uterm_monitor_seat *seat)
{
	struct uterm_monitor_event ev;
	struct uterm_monitor_dev *dev;

	log_debug("free seat %s", seat->name);

	while (seat->devices.next != &seat->devices) {
		dev = shl_dlist_entry(seat->devices.next,
						struct uterm_monitor_dev,
						list);
		seat_free_dev(dev);
	}

	shl_dlist_unlink(&seat->list);

	memset(&ev, 0, sizeof(ev));
	ev.type = UTERM_MONITOR_FREE_SEAT;
	ev.seat = seat;
	ev.seat_name = seat->name;
	ev.seat_data = seat->data;
	seat->mon->cb(seat->mon, &ev, seat->mon->data);

	free(seat->name);
	free(seat);
}

static int get_card_id(struct udev_device *dev)
{
	const char *name;
	char *end;
	int devnum;

	name = udev_device_get_sysname(dev);
	if (!name)
		return -ENODEV;
	if (strncmp(name, "card", 4) || !name[4])
		return -ENODEV;

	devnum = strtol(&name[4], &end, 10);
	if (devnum < 0 || *end)
		return -ENODEV;

	return devnum;
}

static int get_fb_id(struct udev_device *dev)
{
	const char *name;
	char *end;
	int devnum;

	name = udev_device_get_sysname(dev);
	if (!name)
		return -ENODEV;
	if (strncmp(name, "fb", 2) || !name[2])
		return -ENODEV;

	devnum = strtol(&name[2], &end, 10);
	if (devnum < 0 || *end)
		return -ENODEV;

	return devnum;
}

static void monitor_udev_add(struct uterm_monitor *mon,
				struct udev_device *dev)
{
	const char *sname, *subs, *node, *name, *sysname;
	struct shl_dlist *iter;
	struct uterm_monitor_seat *seat;
	unsigned int type;
	int id;
	struct udev_device *p;

	name = udev_device_get_syspath(dev);
	if (!name) {
		log_debug("cannot get syspath of udev device");
		return;
	}

	if (monitor_find_dev(mon, dev)) {
		log_debug("adding already available device %s", name);
		return;
	}

	node = udev_device_get_devnode(dev);
	if (!node)
		return;

	subs = udev_device_get_subsystem(dev);
	if (!subs) {
		log_debug("adding device with invalid subsystem %s", name);
		return;
	}

	if (!strcmp(subs, "drm")) {
#ifdef UTERM_HAVE_SYSTEMD
		if (udev_device_has_tag(dev, "seat") != 1) {
			log_debug("adding non-seat'ed device %s", name);
			return;
		}
#endif
		id = get_card_id(dev);
		if (id < 0) {
			log_debug("adding drm sub-device %s", name);
			return;
		}
		sname = udev_device_get_property_value(dev, "ID_SEAT");
		type = UTERM_MONITOR_DRM;
	} else if (!strcmp(subs, "graphics")) {
#ifdef UTERM_HAVE_SYSTEMD
		if (udev_device_has_tag(dev, "seat") != 1) {
			log_debug("adding non-seat'ed device %s", name);
			return;
		}
#endif
		id = get_fb_id(dev);
		if (id < 0) {
			log_debug("adding fbdev sub-device %s", name);
			return;
		}
		sname = udev_device_get_property_value(dev, "ID_SEAT");
		type = UTERM_MONITOR_FBDEV;
	} else if (!strcmp(subs, "input")) {
		sysname = udev_device_get_sysname(dev);
		if (!sysname || strncmp(sysname, "event", 5)) {
			log_debug("adding unsupported input dev %s", name);
			return;
		}
		p = udev_device_get_parent_with_subsystem_devtype(dev,
								"input", NULL);
		if (!p) {
			log_debug("adding device without parent %s", name);
			return;
		}
#ifdef UTERM_HAVE_SYSTEMD
		if (udev_device_has_tag(p, "seat") != 1) {
			log_debug("adding non-seat'ed device %s", name);
			return;
		}
#endif
		sname = udev_device_get_property_value(p, "ID_SEAT");
		type = UTERM_MONITOR_INPUT;
	} else {
		log_debug("adding device with unknown subsystem %s (%s)",
				subs, name);
		return;
	}

	if (!sname)
		sname = "seat0";

	/* find correct seat */
	shl_dlist_for_each(iter, &mon->seats) {
		seat = shl_dlist_entry(iter, struct uterm_monitor_seat,
						list);
		if (!strcmp(sname, seat->name))
			break;
	}

	if (iter == &mon->seats) {
		log_debug("adding device for unknown seat %s (%s)",
				sname, name);
		return;
	}

	seat_new_dev(seat, type, node);
}

static void monitor_udev_remove(struct uterm_monitor *mon,
				struct udev_device *dev)
{
	struct uterm_monitor_dev *sdev;

	sdev = monitor_find_dev(mon, dev);
	if (!sdev) {
		log_debug("removing unknown device");
		return;
	}

	seat_free_dev(sdev);
}

static void monitor_udev_change(struct uterm_monitor *mon,
				struct udev_device *dev)
{
	const char *sname, *val;
	struct uterm_monitor_dev *sdev;
	struct uterm_monitor_event ev;

	sdev = monitor_find_dev(mon, dev);
	if (sdev) {
		sname = udev_device_get_property_value(dev, "ID_SEAT");
		if (!sname)
			sname = "seat0";
		if (strcmp(sname, sdev->seat->name)) {
			/* device switched seats; remove and add it again */
			seat_free_dev(sdev);
			monitor_udev_add(mon, dev);
			return;
		}

		/* DRM devices send hotplug events; catch them here */
		val = udev_device_get_property_value(dev, "HOTPLUG");
		if (val && !strcmp(val, "1")) {
			memset(&ev, 0, sizeof(ev));
			ev.type = UTERM_MONITOR_HOTPLUG_DEV;
			ev.seat = sdev->seat;
			ev.seat_name = sdev->seat->name;
			ev.seat_data = sdev->seat->data;
			ev.dev = sdev;
			ev.dev_type = sdev->type;
			ev.dev_node = sdev->node;
			ev.dev_data = sdev->data;
			sdev->seat->mon->cb(sdev->seat->mon, &ev,
						sdev->seat->mon->data);
		}
	} else {
		/* Unknown device; maybe it switched into a known seat? Try
		 * adding it as new device. If that fails, we ignore it */
		monitor_udev_add(mon, dev);
	}
}

static void monitor_udev_event(struct ev_fd *fd,
				int mask,
				void *data)
{
	struct uterm_monitor *mon = data;
	struct udev_device *dev;
	const char *action;

	if (mask & (EV_HUP | EV_ERR)) {
		log_warn("udev monitor closed unexpectedly");
		return;
	}

	/*
	 * If there is a pending sd_event in the current epoll-queue and our
	 * udev event is called first, we must make sure to first execute the
	 * sd_event. Otherwise, our udev event might introduce new seats that
	 * will be initialized later and we loose devices.
	 * monitor_sd_event() flushes the sd-fd so we will never refresh seat
	 * values twice in a single epoll-loop.
	 */
	monitor_sd_poll(mon);

	while (true) {
		/* we use non-blocking udev monitor so ignore errors */
		dev = udev_monitor_receive_device(mon->umon);
		if (!dev)
			return;

		action = udev_device_get_action(dev);
		if (action) {
			if (!strcmp(action, "add"))
				monitor_udev_add(mon, dev);
			else if (!strcmp(action, "remove"))
				monitor_udev_remove(mon, dev);
			else if (!strcmp(action, "change"))
				monitor_udev_change(mon, dev);
		}

		udev_device_unref(dev);
	}
}

int uterm_monitor_new(struct uterm_monitor **out,
			struct ev_eloop *eloop,
			uterm_monitor_cb cb,
			void *data)
{
	struct uterm_monitor *mon;
	int ret, ufd, set;

	if (!out || !eloop || !cb)
		return -EINVAL;

	mon = malloc(sizeof(*mon));
	if (!mon)
		return -EINVAL;
	memset(mon, 0, sizeof(*mon));
	mon->ref = 1;
	mon->eloop = eloop;
	mon->cb = cb;
	mon->data = data;
	shl_dlist_init(&mon->seats);

	ret = monitor_sd_init(mon);
	if (ret)
		goto err_free;

	mon->udev = udev_new();
	if (!mon->udev) {
		log_err("cannot create udev object");
		ret = -EFAULT;
		goto err_sd;
	}

	mon->umon = udev_monitor_new_from_netlink(mon->udev, "udev");
	if (!mon->umon) {
		log_err("cannot create udev monitor");
		ret = -EFAULT;
		goto err_udev;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(mon->umon,
							"drm", "drm_minor");
	if (ret) {
		errno = -ret;
		log_err("cannot add udev filter (%d): %m", ret);
		ret = -EFAULT;
		goto err_umon;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(mon->umon,
							"graphics", NULL);
	if (ret) {
		errno = -ret;
		log_err("cannot add udev filter (%d): %m", ret);
		ret = -EFAULT;
		goto err_umon;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(mon->umon,
							"input", NULL);
	if (ret) {
		errno = -ret;
		log_err("cannot add udev filter (%d): %m", ret);
		ret = -EFAULT;
		goto err_umon;
	}

	ret = udev_monitor_enable_receiving(mon->umon);
	if (ret) {
		errno = -ret;
		log_err("cannot start udev monitor (%d): %m", ret);
		ret = -EFAULT;
		goto err_umon;
	}

	ufd = udev_monitor_get_fd(mon->umon);
	if (ufd < 0) {
		log_err("cannot get udev monitor fd");
		ret = -EFAULT;
		goto err_umon;
	}

	set = fcntl(ufd, F_GETFL);
	if (set < 0) {
		log_err("cannot get udev monitor fd flags");
		ret = -EFAULT;
		goto err_umon;
	}

	set |= O_NONBLOCK;
	ret = fcntl(ufd, F_SETFL, set);
	if (ret != 0) {
		log_err("cannot set udev monitor fd flags");
		ret = -EFAULT;
		goto err_umon;
	}

	ret = ev_eloop_new_fd(mon->eloop, &mon->umon_fd, ufd, EV_READABLE,
				monitor_udev_event, mon);
	if (ret)
		goto err_umon;

	ev_eloop_ref(mon->eloop);
	*out = mon;
	return 0;

err_umon:
	udev_monitor_unref(mon->umon);
err_udev:
	udev_unref(mon->udev);
err_sd:
	monitor_sd_deinit(mon);
err_free:
	free(mon);
	return ret;
}

void uterm_monitor_ref(struct uterm_monitor *mon)
{
	if (!mon || !mon->ref)
		return;

	++mon->ref;
}

void uterm_monitor_unref(struct uterm_monitor *mon)
{
	struct uterm_monitor_seat *seat;

	if (!mon || !mon->ref || --mon->ref)
		return;

	while (mon->seats.next != &mon->seats) {
		seat = shl_dlist_entry(mon->seats.next,
						struct uterm_monitor_seat,
						list);
		monitor_free_seat(seat);
	}

	ev_eloop_rm_fd(mon->umon_fd);
	udev_monitor_unref(mon->umon);
	udev_unref(mon->udev);
	monitor_sd_deinit(mon);
	ev_eloop_unref(mon->eloop);
	free(mon);
}

void uterm_monitor_scan(struct uterm_monitor *mon)
{
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	struct udev_device *dev;
	const char *path;
	int ret;

	if (!mon)
		return;

	monitor_refresh_seats(mon);

	e = udev_enumerate_new(mon->udev);
	if (!e) {
		log_err("cannot create udev enumeration");
		return;
	}

	ret = udev_enumerate_add_match_subsystem(e, "drm");
	if (ret) {
		errno = -ret;
		log_err("cannot add udev match (%d): %m", ret);
		goto out_enum;
	}

	ret = udev_enumerate_add_match_subsystem(e, "graphics");
	if (ret) {
		errno = -ret;
		log_err("cannot add udev match (%d): %m", ret);
		goto out_enum;
	}

	ret = udev_enumerate_add_match_subsystem(e, "input");
	if (ret) {
		errno = -ret;
		log_err("cannot add udev match (%d): %m", ret);
		goto out_enum;
	}

	ret = udev_enumerate_scan_devices(e);
	if (ret) {
		log_err("cannot scan udev devices (%d): %m", ret);
		goto out_enum;
	}

	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(entry);
		if (!path) {
			log_debug("udev device without syspath");
			continue;
		}
		dev = udev_device_new_from_syspath(mon->udev, path);
		if (!dev) {
			log_debug("cannot get udev device for %s", path);
			continue;
		}

		monitor_udev_add(mon, dev);
		udev_device_unref(dev);
	}

out_enum:
	udev_enumerate_unref(e);
}

void uterm_monitor_set_seat_data(struct uterm_monitor_seat *seat, void *data)
{
	if (!seat)
		return;

	seat->data = data;
}

void uterm_monitor_set_dev_data(struct uterm_monitor_dev *dev, void *data)
{
	if (!dev)
		return;

	dev->data = data;
}
