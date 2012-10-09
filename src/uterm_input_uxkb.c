/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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

#include <errno.h>
#include <inttypes.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include "log.h"
#include "shl_misc.h"
#include "uterm.h"
#include "uterm_input.h"

#define LOG_SUBSYSTEM "input_uxkb"

int uxkb_desc_init(struct uterm_input *input,
		   const char *layout,
		   const char *variant,
		   const char *options)
{
	int ret;
	struct xkb_rule_names rmlvo = {
		.rules = "evdev",
		.model = "evdev",
		.layout = layout,
		.variant = variant,
		.options = options,
	};

	input->ctx = xkb_context_new(0);
	if (!input->ctx) {
		log_error("cannot create XKB context");
		return -ENOMEM;
	}

	input->keymap = xkb_map_new_from_names(input->ctx, &rmlvo, 0);
	if (!input->keymap) {
		log_warn("failed to create keymap (%s, %s, %s), "
			 "reverting to default US keymap",
			 layout, variant, options);

		rmlvo.layout = "us";
		rmlvo.variant = "";
		rmlvo.options = "";

		input->keymap = xkb_map_new_from_names(input->ctx, &rmlvo, 0);
		if (!input->keymap) {
			log_warn("failed to create XKB keymap");
			ret = -EFAULT;
			goto err_ctx;
		}
	}

	log_debug("new keyboard description (%s, %s, %s)",
		  layout, variant, options);
	return 0;

err_ctx:
	xkb_context_unref(input->ctx);
	return ret;
}

void uxkb_desc_destroy(struct uterm_input *input)
{
	xkb_map_unref(input->keymap);
	xkb_context_unref(input->ctx);
}

int uxkb_dev_init(struct uterm_input_dev *dev)
{
	dev->state = xkb_state_new(dev->input->keymap);
	if (!dev->state)
		return -ENOMEM;

	return 0;
}

void uxkb_dev_destroy(struct uterm_input_dev *dev)
{
	xkb_state_unref(dev->state);
}

#define EVDEV_KEYCODE_OFFSET 8
enum {
	KEY_RELEASED = 0,
	KEY_PRESSED = 1,
	KEY_REPEATED = 2,
};

int uxkb_dev_process(struct uterm_input_dev *dev,
		     uint16_t key_state,
		     uint16_t code,
		     struct uterm_input_event *out)
{
	struct xkb_state *state;
	struct xkb_keymap *keymap;
	xkb_keycode_t keycode;
	const xkb_keysym_t *keysyms;
	int num_keysyms;

	state = dev->state;
	keymap = xkb_state_get_map(state);
	keycode = code + EVDEV_KEYCODE_OFFSET;

	num_keysyms = xkb_key_get_syms(state, keycode, &keysyms);

	if (key_state == KEY_PRESSED)
		xkb_state_update_key(state, keycode, XKB_KEY_DOWN);
	else if (key_state == KEY_RELEASED)
		xkb_state_update_key(state, keycode, XKB_KEY_UP);

	if (key_state == KEY_RELEASED)
		return -ENOKEY;

	if (key_state == KEY_REPEATED && !xkb_key_repeats(keymap, keycode))
		return -ENOKEY;

	if (num_keysyms <= 0)
		return -ENOKEY;

	/*
	 * TODO: xkbcommon actually supports multiple keysyms
	 * per key press. Here we're just using the first one,
	 * but we might want to support this feature.
	 */
	out->keycode = code;
	out->keysym = keysyms[0];
	out->mods = shl_get_xkb_mods(state);
	out->unicode = xkb_keysym_to_utf32(out->keysym) ? : UTERM_INPUT_INVALID;

	return 0;
}

/*
 * Call this when we regain control of the keyboard after losing it.
 * We don't reset the locked group, this should survive a VT switch, etc. The
 * locked modifiers are reset according to the keyboard LEDs.
 */
void uxkb_dev_reset(struct uterm_input_dev *dev, const unsigned long *ledbits)
{
	unsigned int i;
	struct xkb_state *state;
	static const struct {
		int led;
		const char *name;
	} led_names[] = {
		{ LED_NUML, XKB_LED_NAME_NUM },
		{ LED_CAPSL, XKB_LED_NAME_CAPS },
		{ LED_SCROLLL, XKB_LED_NAME_SCROLL },
	};

	/* TODO: Urghs, while the input device was closed we might have missed
	 * some events that affect internal state. As xkbcommon does not provide
	 * a way to reset the internal state, we simply recreate the state. This
	 * should have the same effect.
	 * It also has a bug that if the CTRL-Release event is skipped, then
	 * every further release will never perform a _real_ release. Kind of
	 * buggy so we should fix it upstream. */
	state = xkb_state_new(dev->input->keymap);
	if (!state) {
		log_warning("cannot recreate xkb-state");
		return;
	}
	xkb_state_unref(dev->state);
	dev->state = state;

	for (i = 0; i < sizeof(led_names) / sizeof(*led_names); i++) {
		if (!input_bit_is_set(ledbits, led_names[i].led))
			continue;

		/*
		 * TODO: Add support in xkbcommon for setting the led state,
		 * and updating the modifier state accordingly. E.g., something
		 * like this:
		 *	xkb_state_led_name_set_active(state, led_names[i].led);
		 */
	}
}
