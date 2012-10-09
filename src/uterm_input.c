/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
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
 * Input Devices
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "log.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "uterm.h"
#include "uterm_input.h"

#define LOG_SUBSYSTEM "input"

/* How many longs are needed to hold \n bits. */
#define NLONGS(n) (((n) + LONG_BIT - 1) / LONG_BIT)

enum device_feature {
	FEATURE_HAS_KEYS = 0x01,
	FEATURE_HAS_LEDS = 0x02,
};

static void input_free_dev(struct uterm_input_dev *dev);

static void notify_key(struct uterm_input_dev *dev,
			uint16_t type,
			uint16_t code,
			int32_t value)
{
	int ret;

	if (type != EV_KEY)
		return;

	ret = uxkb_dev_process(dev, value, code);
	if (ret)
		return;

	shl_hook_call(dev->input->hook, dev->input, &dev->event);
}

static void input_data_dev(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_input_dev *dev = data;
	struct input_event ev[16];
	ssize_t len, n;
	int i;

	if (mask & (EV_HUP | EV_ERR)) {
		log_debug("EOF on %s", dev->node);
		input_free_dev(dev);
		return;
	}

	len = sizeof(ev);
	while (len == sizeof(ev)) {
		len = read(dev->rfd, &ev, sizeof(ev));
		if (len < 0) {
			if (errno == EWOULDBLOCK)
				break;
			log_warn("reading from %s failed (%d): %m",
						dev->node, errno);
			input_free_dev(dev);
		} else if (len == 0) {
			log_debug("EOF on %s", dev->node);
			input_free_dev(dev);
		} else if (len % sizeof(*ev)) {
			log_warn("invalid input_event on %s", dev->node);
		} else {
			n = len / sizeof(*ev);
			for (i = 0; i < n; i++)
				notify_key(dev, ev[i].type, ev[i].code,
								ev[i].value);
		}
	}
}

static int input_wake_up_dev(struct uterm_input_dev *dev)
{
	int ret;
	unsigned long ledbits[NLONGS(LED_CNT)] = { 0 };

	if (dev->rfd >= 0)
		return 0;

	dev->rfd = open(dev->node, O_CLOEXEC | O_NONBLOCK | O_RDONLY);
	if (dev->rfd < 0) {
		log_warn("cannot open device %s (%d): %m", dev->node, errno);
		return -EFAULT;
	}

	if (dev->features & FEATURE_HAS_KEYS) {
		if (dev->features & FEATURE_HAS_LEDS) {
			ret = ioctl(dev->rfd, EVIOCGLED(sizeof(ledbits)),
								&ledbits);
			if (ret == -1)
				log_warn("cannot read LED state of %s (%d): %m",
					 dev->node, errno);
		}

		/* rediscover the keyboard state if sth changed during sleep */
		uxkb_dev_reset(dev, ledbits);

		ret = ev_eloop_new_fd(dev->input->eloop, &dev->fd,
						dev->rfd, EV_READABLE,
						input_data_dev, dev);
		if (ret) {
			close(dev->rfd);
			dev->rfd = -1;
			return ret;
		}
	}

	return 0;
}

static void input_sleep_dev(struct uterm_input_dev *dev)
{
	if (dev->rfd < 0)
		return;

	ev_eloop_rm_fd(dev->fd);
	dev->fd = NULL;
	close(dev->rfd);
	dev->rfd = -1;
}

static void input_new_dev(struct uterm_input *input,
				const char *node,
				unsigned int features)
{
	struct uterm_input_dev *dev;
	int ret;

	dev = malloc(sizeof(*dev));
	if (!dev)
		return;
	memset(dev, 0, sizeof(*dev));
	dev->input = input;
	dev->rfd = -1;
	dev->features = features;

	dev->node = strdup(node);
	if (!dev->node)
		goto err_free;

	dev->num_syms = 1;
	dev->event.keysyms = malloc(sizeof(uint32_t) * dev->num_syms);
	if (!dev->event.keysyms)
		goto err_node;
	dev->event.codepoints = malloc(sizeof(uint32_t) * dev->num_syms);
	if (!dev->event.codepoints)
		goto err_syms;

	ret = uxkb_dev_init(dev);
	if (ret)
		goto err_codepoints;

	if (input->awake > 0) {
		ret = input_wake_up_dev(dev);
		if (ret)
			goto err_kbd;
	}

	log_debug("new device %s", node);
	shl_dlist_link(&input->devices, &dev->list);
	return;

err_kbd:
	uxkb_dev_destroy(dev);
err_codepoints:
	free(dev->event.codepoints);
err_syms:
	free(dev->event.keysyms);
err_node:
	free(dev->node);
err_free:
	free(dev);
}

static void input_free_dev(struct uterm_input_dev *dev)
{
	log_debug("free device %s", dev->node);
	input_sleep_dev(dev);
	shl_dlist_unlink(&dev->list);
	uxkb_dev_destroy(dev);
	free(dev->event.codepoints);
	free(dev->event.keysyms);
	free(dev->node);
	free(dev);
}

int uterm_input_new(struct uterm_input **out,
		    struct ev_eloop *eloop,
		    const char *layout,
		    const char *variant,
		    const char *options)
{
	struct uterm_input *input;
	int ret;

	if (!out || !eloop)
		return -EINVAL;

	input = malloc(sizeof(*input));
	if (!input)
		return -ENOMEM;
	memset(input, 0, sizeof(*input));
	input->ref = 1;
	input->eloop = eloop;
	shl_dlist_init(&input->devices);

	ret = shl_hook_new(&input->hook);
	if (ret)
		goto err_free;

	ret = uxkb_desc_init(input, layout, variant, options);
	if (ret)
		goto err_hook;

	log_debug("new object %p", input);
	ev_eloop_ref(input->eloop);
	*out = input;
	return 0;

err_hook:
	shl_hook_free(input->hook);
err_free:
	free(input);
	return ret;
}

void uterm_input_ref(struct uterm_input *input)
{
	if (!input || !input->ref)
		return;

	++input->ref;
}

void uterm_input_unref(struct uterm_input *input)
{
	struct uterm_input_dev *dev;

	if (!input || !input->ref || --input->ref)
		return;

	log_debug("free object %p", input);

	while (input->devices.next != &input->devices) {
		dev = shl_dlist_entry(input->devices.next,
					struct uterm_input_dev,
					list);
		input_free_dev(dev);
	}

	uxkb_desc_destroy(input);
	shl_hook_free(input->hook);
	ev_eloop_unref(input->eloop);
	free(input);
}

/*
 * See if the device has anything useful to offer.
 * We go over the desired features and return a mask of enum device_feature's.
 */
static unsigned int probe_device_features(const char *node)
{
	int i, fd, ret;
	unsigned int features = 0;
	unsigned long evbits[NLONGS(EV_CNT)] = { 0 };
	unsigned long keybits[NLONGS(KEY_CNT)] = { 0 };

	fd = open(node, O_NONBLOCK | O_CLOEXEC | O_RDONLY);
	if (fd < 0)
		return 0;

	/* Which types of input events the device supports. */
	ret = ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
	if (ret == -1)
		goto err_ioctl;

	/* Device supports keys/buttons. */
	if (input_bit_is_set(evbits, EV_KEY)) {
		ret = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
		if (ret == -1)
			goto err_ioctl;

		/*
		 * If the device support any of the normal keyboard keys, we
		 * take it. Even if the keys are not ordinary they can be
		 * mapped to anything by the keyboard backend.
		 */
		for (i = KEY_RESERVED; i <= KEY_MIN_INTERESTING; i++) {
			if (input_bit_is_set(keybits, i)) {
				features |= FEATURE_HAS_KEYS;
				break;
			}
		}
	}

	if (input_bit_is_set(evbits, EV_LED))
		features |= FEATURE_HAS_LEDS;

	close(fd);
	return features;

err_ioctl:
	log_warn("cannot probe features of device %s (%d): %m", node, errno);
	close(fd);
	return 0;
}

void uterm_input_add_dev(struct uterm_input *input, const char *node)
{
	unsigned int features;

	if (!input || !node)
		return;

	features = probe_device_features(node);
	if (!(features & FEATURE_HAS_KEYS)) {
		log_debug("ignoring non-useful device %s", node);
		return;
	}

	input_new_dev(input, node, features);
}

void uterm_input_remove_dev(struct uterm_input *input, const char *node)
{
	struct shl_dlist *iter;
	struct uterm_input_dev *dev;

	if (!input || !node)
		return;

	shl_dlist_for_each(iter, &input->devices) {
		dev = shl_dlist_entry(iter,
					struct uterm_input_dev,
					list);
		if (!strcmp(dev->node, node)) {
			input_free_dev(dev);
			break;
		}
	}
}

int uterm_input_register_cb(struct uterm_input *input,
				uterm_input_cb cb,
				void *data)
{
	if (!input || !cb)
		return -EINVAL;

	return shl_hook_add_cast(input->hook, cb, data);
}

void uterm_input_unregister_cb(struct uterm_input *input,
				uterm_input_cb cb,
				void *data)
{
	if (!input || !cb)
		return;

	shl_hook_rm_cast(input->hook, cb, data);
}

void uterm_input_sleep(struct uterm_input *input)
{
	struct shl_dlist *iter;
	struct uterm_input_dev *dev;

	if (!input)
		return;

	--input->awake;
	if (input->awake != 0)
		return;

	log_debug("going to sleep");

	shl_dlist_for_each(iter, &input->devices) {
		dev = shl_dlist_entry(iter,
					struct uterm_input_dev,
					list);
		input_sleep_dev(dev);
	}
}

void uterm_input_wake_up(struct uterm_input *input)
{
	struct shl_dlist *iter, *tmp;
	struct uterm_input_dev *dev;
	int ret;

	if (!input)
		return;

	++input->awake;
	if (input->awake != 1)
		return;

	log_debug("wakeing up");

	shl_dlist_for_each_safe(iter, tmp, &input->devices) {
		dev = shl_dlist_entry(iter,
					struct uterm_input_dev,
					list);
		ret = input_wake_up_dev(dev);
		if (ret)
			input_free_dev(dev);
	}
}

bool uterm_input_is_awake(struct uterm_input *input)
{
	if (!input)
		return false;

	return input->awake > 0;
}
