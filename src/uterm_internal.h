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

/* Internal definitions */

#ifndef UTERM_INTERNAL_H
#define UTERM_INTERNAL_H

#include <inttypes.h>
#include <libudev.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"
#include "static_misc.h"
#include "uterm.h"

static inline bool input_bit_is_set(const unsigned long *array, int bit)
{
	return !!(array[bit / LONG_BIT] & (1LL << (bit % LONG_BIT)));
}

/* kbd API */

struct kbd_desc;
struct kbd_dev;

struct kbd_desc_ops {
	int (*init) (struct kbd_desc **out, const char *layout,
		     const char *variant, const char *options);
	void (*ref) (struct kbd_desc *desc);
	void (*unref) (struct kbd_desc *desc);
	int (*alloc) (struct kbd_desc *desc, struct kbd_dev **out);
	void (*keysym_to_string) (uint32_t keysym, char *str, size_t size);
	int (*string_to_keysym) (const char *n, uint32_t *out);
};

struct kbd_dev_ops {
	void (*ref) (struct kbd_dev *dev);
	void (*unref) (struct kbd_dev *dev);
	void (*reset) (struct kbd_dev *dev, const unsigned long *ledbits);
	int (*process) (struct kbd_dev *dev, uint16_t state, uint16_t code,
			struct uterm_input_event *out);
};

struct plain_desc {
	int unused;
};

struct plain_dev {
	unsigned int mods;
};

static const bool plain_available = true;
extern const struct kbd_desc_ops plain_desc_ops;
extern const struct kbd_dev_ops plain_dev_ops;

extern int plain_string_to_keysym(const char *n, uint32_t *out);

#ifdef UTERM_HAVE_XKBCOMMON

struct uxkb_desc {
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
};

struct uxkb_dev {
	struct xkb_state *state;
};

static const bool uxkb_available = true;
extern const struct kbd_desc_ops uxkb_desc_ops;
extern const struct kbd_dev_ops uxkb_dev_ops;

extern int uxkb_string_to_keysym(const char *n, uint32_t *out);

#else /* !UTERM_HAVE_XKBCOMMON */

struct uxkb_desc {
	int unused;
};

struct uxkb_dev {
	int unused;
};

static const bool uxkb_available = false;
static const struct kbd_desc_ops uxkb_desc_ops;
static const struct kbd_dev_ops uxkb_dev_ops;

#endif /* UTERM_HAVE_XKBCOMMON */

struct kbd_desc {
	unsigned long ref;
	const struct kbd_desc_ops *ops;

	union {
		struct plain_desc plain;
		struct uxkb_desc uxkb;
	};
};

struct kbd_dev {
	unsigned long ref;
	struct kbd_desc *desc;
	const struct kbd_dev_ops *ops;

	union {
		struct plain_dev plain;
		struct uxkb_dev uxkb;
	};
};

enum kbd_mode {
	KBD_PLAIN,
	KBD_UXKB,
};

static inline int kbd_desc_new(struct kbd_desc **out, const char *layout,
			       const char *variant, const char *options,
			       unsigned int mode)
{
	const struct kbd_desc_ops *ops;

	switch (mode) {
	case KBD_UXKB:
		if (!uxkb_available) {
			log_error("XKB KBD backend not available");
			return -EOPNOTSUPP;
		}
		ops = &uxkb_desc_ops;
		break;
	case KBD_PLAIN:
		if (!plain_available) {
			log_error("plain KBD backend not available");
			return -EOPNOTSUPP;
		}
		ops = &plain_desc_ops;
		break;
	default:
		log_error("unknown KBD backend %u", mode);
		return -EINVAL;
	}

	return ops->init(out, layout, variant, options);
}

static inline void kbd_desc_ref(struct kbd_desc *desc)
{
	if (!desc)
		return;

	return desc->ops->ref(desc);
}

static inline void kbd_desc_unref(struct kbd_desc *desc)
{
	if (!desc)
		return;

	return desc->ops->unref(desc);
}

static inline int kbd_desc_alloc(struct kbd_desc *desc, struct kbd_dev **out)
{
	if (!desc)
		return -EINVAL;

	return desc->ops->alloc(desc, out);
}

static inline void kbd_desc_keysym_to_string(struct kbd_desc *desc,
					     uint32_t keysym,
					     char *str, size_t size)
{
	if (!desc)
		return;

	return desc->ops->keysym_to_string(keysym, str, size);
}

static inline int kbd_desc_string_to_keysym(struct kbd_desc *desc,
					    const char *n,
					    uint32_t *out)
{
	if (!desc)
		return -EINVAL;

	return desc->ops->string_to_keysym(n, out);
}

static inline void kbd_dev_ref(struct kbd_dev *dev)
{
	if (!dev)
		return;

	return dev->ops->ref(dev);
}

static inline void kbd_dev_unref(struct kbd_dev *dev)
{
	if (!dev)
		return;

	return dev->ops->unref(dev);
}

static inline void kbd_dev_reset(struct kbd_dev *dev,
				 const unsigned long *ledbits)
{
	if (!dev)
		return;

	return dev->ops->reset(dev, ledbits);
}

static inline int kbd_dev_process(struct kbd_dev *dev,
				  uint16_t key_state,
				  uint16_t code,
				  struct uterm_input_event *out)
{
	if (!dev)
		return -EINVAL;

	return dev->ops->process(dev, key_state, code, out);
}

#endif /* UTERM_INTERNAL_H */
