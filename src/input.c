/*
 * kmscon - udev input hotplug and evdev handling
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
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
 * The main object kmscon_input discovers and monitors input devices, and
 * adds/removes them accordingly from the devices linked list.
 *
 * The udev monitor keeps running even while the object is in INPUT_ASLEEP.
 * We do this because we'll either lose track of the devices, or otherwise
 * have to re-scan the devices at every wakeup.
 *
 * The kmscon_input_device objects hold the file descriptors for their device
 * nodes. All events go through the input-object callback; there is currently
 * no "routing" or any differentiation between them. When the input is put to
 * sleep, all fd's are closed. When woken up, they are opened. There should be
 * not spurious events delivered. The initial state depends on the
 * kmscon_input's state.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <libudev.h>
#include <linux/input.h>

#include "log.h"
#include "input.h"

enum input_state {
	INPUT_ASLEEP,
	INPUT_AWAKE,
};

struct kmscon_input {
	size_t ref;
	enum input_state state;
	struct kmscon_input_device *devices;

	/*
	 * We need to keep a reference to the eloop for sleeping, waking up and
	 * hotplug. Avoiding this makes things harder.
	 */
	struct kmscon_eloop *loop;

	struct udev *udev;
	struct udev_monitor *monitor;
	struct kmscon_fd *monitor_fd;

	kmscon_input_cb cb;
};

struct kmscon_input_device {
	size_t ref;
	struct kmscon_input *input;
	struct kmscon_input_device *next;

	int rfd;
	char *devnode;
	struct kmscon_fd *fd;
};

/* kmscon_input_device prototypes */
int kmscon_input_device_new(struct kmscon_input_device **out,
						struct kmscon_input *input,
						const char *devnode);
void kmscon_input_device_ref(struct kmscon_input_device *device);
void kmscon_input_device_unref(struct kmscon_input_device *device);

int kmscon_input_device_open(struct kmscon_input_device *device);
void kmscon_input_device_close(struct kmscon_input_device *device);

int kmscon_input_device_connect_eloop(struct kmscon_input_device *device);
void kmscon_input_device_disconnect_eloop(struct kmscon_input_device *device);
void kmscon_input_device_sleep(struct kmscon_input_device *device);
int kmscon_input_device_wake_up(struct kmscon_input_device *device);

/* internal procedures prototypes */
static int init_input(struct kmscon_input *input);
static void device_changed(struct kmscon_fd *fd, int mask, void *data);
static int add_initial_devices(struct kmscon_input *input);
static int add_device(struct kmscon_input *input, struct udev_device *udev_device);
static void device_added(struct kmscon_input *input, struct udev_device *udev_device);
static void device_removed(struct kmscon_input *input, struct udev_device *udev_device);
static int remove_device(struct kmscon_input *input, struct udev_device *udev_device);
static void device_data_arrived(struct kmscon_fd *fd, int mask, void *data);

int kmscon_input_new(struct kmscon_input **out, kmscon_input_cb cb)
{
	int ret;
	struct kmscon_input *input;

	if (!out)
		return -EINVAL;

	input = malloc(sizeof(*input));
	if (!input)
		return -ENOMEM;

	memset(input, 0, sizeof(*input));
	input->ref = 1;
	input->state = INPUT_ASLEEP;
	input->cb = cb;

	ret = init_input(input);
	if (ret) {
		free(input);
		return ret;
	}

	*out = input;
	return 0;
}

void kmscon_input_ref(struct kmscon_input *input)
{
	if (!input)
		return;

	++input->ref;
}

static int init_input(struct kmscon_input *input)
{
	int ret;

	input->udev = udev_new();
	if (!input->udev)
		return -EFAULT;

	input->monitor = udev_monitor_new_from_netlink(input->udev, "udev");
	if (!input->monitor) {
		ret = -EFAULT;
		goto err_udev;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(input->monitor,
								"input", NULL);
	if (ret)
		goto err_monitor;

	/*
	 * There's no way to query the state of the monitor, so just start it
	 * from here.
	 */
	ret = udev_monitor_enable_receiving(input->monitor);
	if (ret)
		goto err_monitor;

	return 0;

err_monitor:
	udev_monitor_unref(input->monitor);
err_udev:
	udev_unref(input->udev);
	return ret;
}

/*
 * This takes a ref of the loop, but it most likely not what should be
 * keeping it alive.
 */
int kmscon_input_connect_eloop(struct kmscon_input *input, struct kmscon_eloop *loop)
{
	int ret;
	int fd;

	if (!input || !loop || !input->monitor)
		return -EINVAL;

	if (input->loop)
		return -EALREADY;

	input->loop = loop;
	kmscon_eloop_ref(loop);

	fd = udev_monitor_get_fd(input->monitor);

	ret = kmscon_eloop_new_fd(loop, &input->monitor_fd, fd, KMSCON_READABLE,
							device_changed, input);
	if (ret)
		goto err_loop;

	/* XXX: What if a device is added NOW? */

	ret = add_initial_devices(input);
	if (ret)
		goto err_fd;

	return 0;

err_fd:
	kmscon_eloop_rm_fd(input->monitor_fd);
	input->monitor_fd = NULL;
err_loop:
	kmscon_eloop_unref(loop);
	input->loop = NULL;
	return ret;
}

static int add_initial_devices(struct kmscon_input *input)
{
	int ret;

	struct udev_enumerate *e;
	struct udev_list_entry *first;
	struct udev_list_entry *item;
	struct udev_device *udev_device;
	const char *syspath;

	e = udev_enumerate_new(input->udev);
	if (!e)
		return -EFAULT;

	ret = udev_enumerate_add_match_subsystem(e, "input");
	if (ret)
		goto err_enum;

	ret = udev_enumerate_scan_devices(e);
	if (ret)
		goto err_enum;

	first = udev_enumerate_get_list_entry(e);
	udev_list_entry_foreach(item, first) {
		syspath = udev_list_entry_get_name(item);

		udev_device = udev_device_new_from_syspath(input->udev, syspath);
		if (!udev_device)
			continue;

		add_device(input, udev_device);

		udev_device_unref(udev_device);
	}

err_enum:
	udev_enumerate_unref(e);
	return ret;
}

static void device_changed(struct kmscon_fd *fd, int mask, void *data)
{
	struct kmscon_input *input = data;
	struct udev_device *udev_device;
	const char *action;

	if (!input || !input->monitor)
	       return;

	udev_device = udev_monitor_receive_device(input->monitor);
	if (!udev_device)
		goto err_device;

	action = udev_device_get_action(udev_device);
	if (!action)
		goto err_device;

	/*
	 * XXX: need to do something with the others? (change, online,
	 * offline)
	 */
	if (!strcmp(action, "add"))
		device_added(input,  udev_device);
	else if (!strcmp(action, "remove"))
		device_removed(input, udev_device);

err_device:
	udev_device_unref(udev_device);
}

static void device_added(struct kmscon_input *input,
				struct udev_device *udev_device)
{
	add_device(input, udev_device);
}

static void device_removed(struct kmscon_input *input,
				struct udev_device *udev_device)
{
	remove_device(input, udev_device);
}

static int add_device(struct kmscon_input *input, struct udev_device *udev_device)
{
	int ret;
	struct kmscon_input_device *device;
	const char *value, *devnode;

	if (!input || !udev_device)
		return -EINVAL;

	/*
	 * TODO: Here should go a proper filtering of input devices we're
	 * interested in. Maybe also seats, etc?
	 */
	value = udev_device_get_property_value(udev_device, "ID_INPUT_KEYBOARD");
	if (!value || strcmp(value, "1") != 0)
		return 0;

	devnode = udev_device_get_devnode(udev_device);
	if (!devnode)
		return -EFAULT;

	ret = kmscon_input_device_new(&device, input, devnode);
	if (ret)
		return ret;

	if (input->state == INPUT_AWAKE) {
		ret = kmscon_input_device_wake_up(device);
		if (ret) {
			log_warning("input: cannot wake up new device %s: %s\n",
							devnode, strerror(-ret));
			goto err_device;
		}
	}

	device->next = input->devices;
	input->devices = device;

	log_debug("input: added device %s\n", devnode);

	return 0;

err_device:
	kmscon_input_device_unref(device);
	return ret;
}

static int remove_device(struct kmscon_input *input, struct udev_device *udev_device)
{
	struct kmscon_input_device *iter, *prev;
	const char *devnode;

	if (!input || !udev_device)
		return -EINVAL;

	if (!input->devices)
		return 0;

	devnode = udev_device_get_devnode(udev_device);
	if (!devnode)
		return -EFAULT;

	iter = input->devices;
	prev = NULL;
	while (iter) {
		if (!strcmp(iter->devnode, devnode)) {
			if (prev == NULL)
				input->devices = iter->next;
			else
				prev->next = iter->next;
			kmscon_input_device_unref(iter);
			log_debug("input: removed device %s\n", devnode);
			break;
		}

		prev = iter;
		iter = iter->next;
	}

	return 0;
}

void kmscon_input_unref(struct kmscon_input *input)
{
	struct kmscon_input_device *iter, *next;

	if (!input || !input->ref)
		return;

	if (--input->ref)
		return;

	iter = input->devices;
	while (iter) {
		next = iter->next;
		kmscon_input_device_unref(iter);
		iter = next;
	}

	kmscon_input_disconnect_eloop(input);
	udev_monitor_unref(input->monitor);
	udev_unref(input->udev);

	free(input);
}

void kmscon_input_disconnect_eloop(struct kmscon_input *input)
{
	if (!input || !input->loop)
		return;

	kmscon_eloop_rm_fd(input->monitor_fd);
	input->monitor_fd = NULL;
	kmscon_eloop_unref(input->loop);
	input->loop = NULL;
}

void kmscon_input_sleep(struct kmscon_input *input)
{
	struct kmscon_input_device *iter;

	if (!input)
		return;

	for (iter = input->devices; iter; iter = iter->next)
		kmscon_input_device_sleep(iter);

	input->state = INPUT_ASLEEP;
}

void kmscon_input_wake_up(struct kmscon_input *input)
{
	struct kmscon_input_device *iter;

	if (!input)
		return;

	 /*
	  * XXX: should probably catch errors here and do something about
	  * them.
	  */
	for (iter = input->devices; iter; iter = iter->next)
		kmscon_input_device_wake_up(iter);

	input->state = INPUT_AWAKE;
}

bool kmscon_input_is_asleep(struct kmscon_input *input)
{
	if (!input)
		return false;

	return input->state == INPUT_ASLEEP;
}

int kmscon_input_device_new(struct kmscon_input_device **out,
						struct kmscon_input *input,
						const char *devnode)
{
	struct kmscon_input_device *device;

	if (!out || !input)
		return -EINVAL;

	device = malloc(sizeof(*device));
	if (!device)
		return -ENOMEM;

	memset(device, 0, sizeof(*device));
	device->ref = 1;
	device->input = input;
	device->rfd = -1;

	device->devnode = strdup(devnode);
	if (!device->devnode) {
		free(device);
		return -ENOMEM;
	}

	*out = device;
	return 0;
}

void kmscon_input_device_ref(struct kmscon_input_device *device)
{
	if (!device)
		return;

	++device->ref;
}

void kmscon_input_device_unref(struct kmscon_input_device *device)
{
	if (!device || !device->ref)
		return;

	if (--device->ref)
		return;

	kmscon_input_device_close(device);
	free(device->devnode);
	free(device);
}

int kmscon_input_device_open(struct kmscon_input_device *device)
{
	if (!device || !device->devnode)
		return -EINVAL;

	if (device->rfd >= 0)
		return -EALREADY;

	device->rfd = open(device->devnode, O_CLOEXEC, O_RDONLY);
	if (device->rfd < 0)
		return -errno;

	return 0;
}

void kmscon_input_device_close(struct kmscon_input_device *device)
{
	if (!device)
		return;

	if (device->rfd < 0)
		return;

	kmscon_input_device_disconnect_eloop(device);
	close(device->rfd);
	device->rfd = -1;
}

int kmscon_input_device_connect_eloop(struct kmscon_input_device *device)
{
	int ret;

	if (!device || !device->input || !device->input->loop)
		return -EINVAL;

	if (device->fd)
		return -EALREADY;

	ret = kmscon_eloop_new_fd(device->input->loop, &device->fd,
						device->rfd, KMSCON_READABLE,
						device_data_arrived, device);
	if (ret)
		return ret;

	return 0;
}

static void device_data_arrived(struct kmscon_fd *fd, int mask, void *data)
{
	int i;
	ssize_t len, n;
	struct kmscon_input_device *device = data;
	/* 16 is what xf86-input-evdev uses (NUM_EVENTS) */
	struct input_event ev[16];

	len = sizeof(ev);
	while (len == sizeof(ev)) {
		/* we're always supposed to get full events */
		len = read(device->rfd, &ev, sizeof(ev));
		if (len <= 0 || len%sizeof(*ev) != 0)
			break;

		/* this shouldn't happen */
		if (device->input->state != INPUT_AWAKE)
			continue;

		n = len/sizeof(*ev);
		for (i=0; i < n; i++)
			device->input->cb(ev[i].type, ev[i].code, ev[i].value);
	}
}

void kmscon_input_device_disconnect_eloop(struct kmscon_input_device *device)
{
	if (!device || !device->fd)
		return;

	kmscon_eloop_rm_fd(device->fd);
	device->fd = NULL;
}

void kmscon_input_device_sleep(struct kmscon_input_device *device)
{
	kmscon_input_device_close(device);
}

int kmscon_input_device_wake_up(struct kmscon_input_device *device)
{
	int ret;

	ret = kmscon_input_device_open(device);
	if (ret && ret != -EALREADY)
		return ret;

	ret = kmscon_input_device_connect_eloop(device);
	if (ret && ret != -EALREADY) {
		kmscon_input_device_close(device);
		return ret;
	}

	return 0;
}
