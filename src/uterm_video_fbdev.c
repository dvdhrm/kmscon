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
 * FBDEV Video backend
 */

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "log.h"
#include "misc.h"
#include "uterm.h"
#include "uterm_internal.h"

static const char *mode_get_name(const struct uterm_mode *mode)
{
	return NULL;
}

static unsigned int mode_get_width(const struct uterm_mode *mode)
{
	return mode->fbdev.unused;
}

static unsigned int mode_get_height(const struct uterm_mode *mode)
{
	return mode->fbdev.unused;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	struct fb_var_screeninfo *info;
	int ret;
	uint64_t quot;
	size_t len;

	if (!disp->video || !(disp->flags & DISPLAY_OPEN))
		return -EINVAL;

	/* TODO: we currently use the current mode of the framebuffer and do not
	 * allow changing this mode. However, we should rather list valid modes
	 * when opening the device and set the framebuffer to the requested mode
	 * here first.
	 * For now you can use the fbset(1) program to modify
	 * frambuffer-resolutions and timers.
	 *
	 * info->bits_per_pixel = 32 is almost a prerequisite here! We also need
	 * to check for TRUECOLOR. Everything else is just old-school.
	 */

	info = &disp->fbdev.vinfo;
	info->xoffset = 0;
	info->yoffset = 0;
	info->activate = FB_ACTIVATE_NOW;
	info->xres_virtual = info->xres;
	info->yres_virtual = info->yres * 2;

	log_info("video_fbdev: activating display %p to %ux%u", disp,
			info->xres, info->yres);

	ret = ioctl(disp->fbdev.fd, FBIOPUT_VSCREENINFO, info);
	if (ret) {
		log_err("video_fbdev: cannot set vinfo (%d): %m", errno);
		return -EFAULT;
	}

	/* vinfo/finfo may have changed so refetch them */

	ret = ioctl(disp->fbdev.fd, FBIOGET_VSCREENINFO, &info);
	if (ret) {
		log_err("video_fbdev: cannot get vinfo (%d): %m", errno);
		return -EFAULT;
	}

	ret = ioctl(disp->fbdev.fd, FBIOGET_FSCREENINFO, &disp->fbdev.finfo);
	if (ret) {
		log_err("video_fbdev: cannot get finfo (%d): %m", errno);
		return -EFAULT;
	}

	quot = (info->upper_margin + info->lower_margin + info->yres);
	quot *= (info->left_margin + info->right_margin + info->xres);
	quot *= info->pixclock;
	disp->fbdev.rate = quot ? (1000000000000000LLU / quot) : 0;
	if (!disp->fbdev.rate)
		disp->fbdev.rate = 60 * 1000; /* 60 Hz by default */

	len = disp->fbdev.finfo.line_length * disp->fbdev.vinfo.yres_virtual;
	disp->fbdev.map = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED,
				disp->fbdev.fd, 0);
	if (disp->fbdev.map == MAP_FAILED) {
		log_err("video_fbdev: cannot mmap framebuffer (%d): %m", errno);
		return -EFAULT;
	}
	memset(disp->fbdev.map, 0, len);
	disp->fbdev.len = len;

	disp->current_mode = mode;
	disp->flags |= DISPLAY_ONLINE;

	return 0;
}

static void display_deactivate(struct uterm_display *disp)
{
	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return;

	log_info("video_fbdev: deactivating display %p", disp);
	disp->current_mode = NULL;
	disp->flags &= ~DISPLAY_ONLINE;
	munmap(disp->fbdev.map, disp->fbdev.len);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int set, ret;

	if (!disp->video || !(disp->flags & DISPLAY_OPEN))
		return -EINVAL;

	switch (state) {
	case UTERM_DPMS_ON:
		set = FB_BLANK_UNBLANK;
		break;
	case UTERM_DPMS_STANDBY:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_SUSPEND:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_OFF:
		set = FB_BLANK_POWERDOWN;
		break;
	default:
		return -EINVAL;
	}

	log_info("video_fbdev: setting DPMS of display %p to %s", disp,
			uterm_dpms_to_name(state));

	ret = ioctl(disp->fbdev.fd, FBIOBLANK, set);
	if (ret) {
		log_err("video_fbdev: cannot set DPMS on %p (%d): %m", disp,
				errno);
		return -EFAULT;
	}

	disp->dpms = state;
	return 0;
}

void *fbdev_display_map(struct uterm_display *disp)
{
	if (!disp->video || !(disp->flags & DISPLAY_OPEN))
		return NULL;
	if (!(disp->flags & DISPLAY_ONLINE))
		return NULL;

	/* TODO: temporary function to obtain a pointer to the frambuffer from
	 * the calling application. Stuff like bpp, size, etc. must be
	 * published, too, otherwise, no-one will be able to use it.
	 */

	return disp->fbdev.map;
}

static int display_use(struct uterm_display *disp)
{
	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!(disp->flags & DISPLAY_OPEN))
		return -EINVAL;

	return 0;
}

static int display_swap(struct uterm_display *disp)
{
	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!(disp->flags & DISPLAY_OPEN) || (disp->dpms != UTERM_DPMS_ON))
		return -EINVAL;

	/* TODO: swap */
/*
	vinfo.activate = FB_ACTIVATE_VBL;
	vinfo.yoffset = 0 or vinfo.vres;
	ioctl(fd, FBIOPUT_VSCREENINFO, &info)
*/

	return 0;
}

static int get_id(struct udev_device *dev)
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

static int open_display(struct uterm_display *disp)
{
	int ret;

	if (!(disp->video->flags & VIDEO_AWAKE))
		return -EINVAL;
	if (disp->flags & DISPLAY_OPEN)
		return 0;

	disp->fbdev.fd = open(disp->fbdev.node, O_RDWR | O_CLOEXEC);
	if (disp->fbdev.fd < 0) {
		log_err("video_fbdev: cannot open %s (%d): %m",
				disp->fbdev.node, errno);
		return -EFAULT;
	}

	ret = ioctl(disp->fbdev.fd, FBIOGET_FSCREENINFO, &disp->fbdev.finfo);
	if (ret) {
		log_err("video_fbdev: cannot get finfo (%d): %m", errno);
		ret = -EFAULT;
		goto err_fd;
	}

	ret = ioctl(disp->fbdev.fd, FBIOGET_VSCREENINFO, &disp->fbdev.vinfo);
	if (ret) {
		log_err("video_fbdev: cannot get vinfo (%d): %m", errno);
		ret = -EFAULT;
		goto err_fd;
	}

	disp->flags |= DISPLAY_OPEN;

	if (disp->flags & DISPLAY_ONLINE) {
		ret = display_activate(disp, NULL);
		if (ret)
			goto err_open;
	}

	return 0;

err_open:
	disp->flags &= ~DISPLAY_OPEN;
err_fd:
	close(disp->fbdev.fd);
	return ret;
}

static void close_display(struct uterm_display *disp)
{
	if (!(disp->flags & DISPLAY_OPEN))
		return;

	close(disp->fbdev.fd);
	disp->flags &= ~DISPLAY_OPEN;
	if (disp->fbdev.map)
		munmap(disp->fbdev.map, disp->fbdev.len);
}

static int init_display(struct uterm_video *video, struct udev_device *dev)
{
	struct uterm_display *disp;
	int ret, id;
	const char *node;

	id = get_id(dev);
	if (id < 0)
		return id;

	ret = display_new(&disp, &fbdev_display_ops);
	if (ret)
		return ret;

	log_info("video_fbdev: probing %s", udev_device_get_sysname(dev));

	node = udev_device_get_devnode(dev);
	if (!node) {
		log_err("video_fbdev: cannot get device node");
		return -ENODEV;
	}
	disp->fbdev.node = mem_strdup(node);
	if (!disp->fbdev.node) {
		ret = -ENOMEM;
		goto err_free;
	}

	disp->video = video;
	disp->fbdev.id = id;
	disp->dpms = UTERM_DPMS_UNKNOWN;

	if (video->flags & VIDEO_AWAKE) {
		ret = open_display(disp);
		if (ret)
			goto err_str;
	}

	disp->next = video->displays;
	video->displays = disp;
	return 0;

err_str:
	disp->video = NULL;
	mem_free(disp->fbdev.node);
err_free:
	uterm_display_unref(disp);
	return ret;
}

static void destroy_display(struct uterm_display *disp)
{
	display_deactivate(disp);
	close_display(disp);
	mem_free(disp->fbdev.node);
	disp->video = NULL;
	uterm_display_unref(disp);
}

static int video_init(struct uterm_video *video)
{
	struct udev_enumerate *e;
	struct udev_list_entry *name;
	const char *path;
	struct udev_device *dev;
	int ret;

	ret = udev_monitor_filter_add_match_subsystem_devtype(video->umon,
							"graphics", NULL);
	if (ret) {
		log_err("video_fbdev: cannot add udev filter (%d): %m", ret);
		return -EFAULT;
	}

	ret = udev_monitor_enable_receiving(video->umon);
	if (ret) {
		log_err("video_fbdev: cannot start udev_monitor (%d): %m", ret);
		return -EFAULT;
	}

	e = udev_enumerate_new(video->udev);
	if (!e) {
		log_err("video_fbdev: cannot create udev_enumerate object");
		return -EFAULT;
	}

	ret = udev_enumerate_add_match_subsystem(e, "graphics");
	if (ret) {
		log_err("video_fbdev: cannot add udev match (%d): %m", ret);
		ret = -EFAULT;
		goto err_enum;
	}
	ret = udev_enumerate_add_match_sysname(e, "fb[0-9]*");
	if (ret) {
		log_err("video_fbdev: cannot add udev match (%d): %m", ret);
		ret = -EFAULT;
		goto err_enum;
	}
	ret = udev_enumerate_scan_devices(e);
	if (ret) {
		log_err("video_fbdev: cannot scan udev devices (%d): %m", ret);
		ret = -EFAULT;
		goto err_enum;
	}

	udev_list_entry_foreach(name, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(name);
		if (!path || !*path)
			continue;
		dev = udev_device_new_from_syspath(video->udev, path);
		if (!dev)
			continue;

		init_display(video, dev);
		udev_device_unref(dev);
	}

	log_info("video_fbdev: new fbdev device");
	return 0;

err_enum:
	udev_enumerate_unref(e);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct uterm_display *disp;

	log_info("video_fbdev: free fbdev device");

	while ((disp = video->displays)) {
		video->displays = disp->next;
		disp->next = NULL;
		destroy_display(disp);
	}
}

static int video_use(struct uterm_video *video)
{
	return 0;
}

static int hotplug(struct uterm_video *video)
{
	struct udev_device *dev;
	const char *action;
	unsigned int id;
	struct uterm_display *disp, *tmp;

	dev = udev_monitor_receive_device(video->umon);
	if (!dev) {
		log_warn("video_fbdev: cannot receive device from udev_monitor");
		return 0;
	}

	action = udev_device_get_action(dev);
	if (!action)
		goto ignore;
	if (!strcmp(action, "add")) {
		init_display(video, dev);
	} else if (!strcmp(action, "remove")) {
		if (!video->displays)
			goto ignore;
		id = get_id(dev);
		if (id < 0)
			goto ignore;

		disp = NULL;
		if (video->displays->fbdev.id == id) {
			disp = video->displays;
			video->displays = disp->next;
		} else for (tmp = video->displays; tmp->next; tmp = tmp->next) {
			if (tmp->next->fbdev.id == id) {
				disp = tmp->next;
				tmp->next = tmp->next->next;
				break;
			}
		}
		if (disp) {
			disp->next = NULL;
			destroy_display(disp);
		}
	}

ignore:
	udev_device_unref(dev);
	return 0;
}

static int video_poll(struct uterm_video *video, unsigned int num)
{
	unsigned int i;
	struct epoll_event *ev;
	int ret;

	for (i = 0; i < num; ++i) {
		ev = &video->efd_evs[i];
		if (ev->data.fd == video->umon_fd) {
			if (ev->events & (EPOLLERR | EPOLLHUP)) {
				log_err("video_fbdev: udev_monitor closed unexpectedly");
				return -EFAULT;
			}
			if (ev->events & (EPOLLIN)) {
				ret = hotplug(video);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

static void video_sleep(struct uterm_video *video)
{
	struct uterm_display *disp;

	if (!(video->flags & VIDEO_AWAKE))
		return;

	for (disp = video->displays; disp; disp = disp->next)
		close_display(disp);

	video->flags &= ~VIDEO_AWAKE;
}

static int video_wake_up(struct uterm_video *video)
{
	struct uterm_display *disp, *tmp;
	int ret;

	if (video->flags & VIDEO_AWAKE)
		return 0;

	video->flags |= VIDEO_AWAKE;

	while (video->displays) {
		tmp = video->displays;
		ret = open_display(tmp);
		if (!ret)
			break;

		video->displays = tmp->next;
		tmp->next = NULL;
		destroy_display(tmp);
	}
	for (disp = video->displays; disp && disp->next; ) {
		tmp = disp->next;
		ret = open_display(tmp);
		if (!ret) {
			disp = tmp;
		} else {
			disp->next = tmp->next;
			tmp->next = NULL;
			destroy_display(tmp);
		}
	}

	return 0;
}

const struct mode_ops fbdev_mode_ops = {
	.init = NULL,
	.destroy = NULL,
	.get_name = mode_get_name,
	.get_width = mode_get_width,
	.get_height = mode_get_height,
};

const struct display_ops fbdev_display_ops = {
	.init = NULL,
	.destroy = NULL,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = display_use,
	.swap = display_swap,
};

const struct video_ops fbdev_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO */
	.use = video_use,
	.poll = video_poll,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};
