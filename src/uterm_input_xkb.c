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
#include "uterm.h"
#include "uterm_internal.h"

#define LOG_SUBSYSTEM "input_xkb"

struct kbd_desc {
	unsigned long ref;
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
};

struct kbd_dev {
	unsigned long ref;
	struct kbd_desc *desc;
	struct xkb_state *state;
};

int kbd_dev_new(struct kbd_dev **out, struct kbd_desc *desc)
{
	struct kbd_dev *kbd;

	kbd = malloc(sizeof(*kbd));
	if (!kbd)
		return -ENOMEM;

	memset(kbd, 0, sizeof(*kbd));
	kbd->ref = 1;
	kbd->desc = desc;

	kbd->state = xkb_state_new(desc->keymap);
	if (!kbd->state) {
		free(kbd);
		return -ENOMEM;
	}

	kbd_desc_ref(desc);
	*out = kbd;
	return 0;
}

void kbd_dev_ref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref)
		return;

	++kbd->ref;
}

void kbd_dev_unref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref || --kbd->ref)
		return;

	xkb_state_unref(kbd->state);
	kbd_desc_unref(kbd->desc);
	free(kbd);
}

#define EVDEV_KEYCODE_OFFSET 8
enum {
	KEY_RELEASED = 0,
	KEY_PRESSED = 1,
	KEY_REPEATED = 2,
};

static unsigned int get_effective_modmask(struct xkb_state *state)
{
	unsigned int mods = 0;

	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
						XKB_STATE_EFFECTIVE))
	    mods |= UTERM_SHIFT_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS,
						XKB_STATE_EFFECTIVE))
	    mods |= UTERM_LOCK_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
						XKB_STATE_EFFECTIVE))
	    mods |= UTERM_CONTROL_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,
						XKB_STATE_EFFECTIVE))
	    mods |= UTERM_MOD1_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO,
						XKB_STATE_EFFECTIVE))
	    mods |= UTERM_MOD4_MASK;

	return mods;
}

int kbd_dev_process_key(struct kbd_dev *kbd,
			uint16_t key_state,
			uint16_t code,
			struct uterm_input_event *out)
{
	struct xkb_state *state;
	struct xkb_keymap *keymap;
	xkb_keycode_t keycode;
	const xkb_keysym_t *keysyms;
	int num_keysyms;

	if (!kbd)
		return -EINVAL;

	state = kbd->state;
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

	if (num_keysyms < 0)
		return -ENOKEY;

	/*
	 * TODO: xkbcommon actually supports multiple keysyms
	 * per key press. Here we're just using the first one,
	 * but we might want to support this feature.
	 */
	out->keycode = code;
	out->keysym = keysyms[0];
	out->mods = get_effective_modmask(state);;
	out->unicode = xkb_keysym_to_utf32(out->keysym) ?: UTERM_INPUT_INVALID;

	return 0;
}

/*
 * Call this when we regain control of the keyboard after losing it.
 * We don't reset the locked group, this should survive a VT switch, etc. The
 * locked modifiers are reset according to the keyboard LEDs.
 */
void kbd_dev_reset(struct kbd_dev *kbd, const unsigned long *ledbits)
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

	if (!kbd)
		return;

	state = kbd->state;

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

	(void)state;
}

int kbd_desc_new(struct kbd_desc **out,
			const char *layout,
			const char *variant,
			const char *options)
{
	int ret;
	struct kbd_desc *desc;
	struct xkb_rule_names rmlvo = {
		.rules = "evdev",
		.model = "evdev",
		.layout = layout,
		.variant = variant,
		.options = options,
	};

	if (!out)
		return -EINVAL;

	desc = malloc(sizeof(*desc));
	if (!desc)
		return -ENOMEM;

	memset(desc, 0, sizeof(*desc));
	desc->ref = 1;

	desc->ctx = xkb_context_new(0);
	if (!desc->ctx) {
		ret = -ENOMEM;
		goto err_desc;
	}

	desc->keymap = xkb_map_new_from_names(desc->ctx, &rmlvo, 0);
	if (!desc->keymap) {
		log_warn("failed to create keymap (%s, %s, %s), "
			 "reverting to default US keymap",
			 layout, variant, options);

		rmlvo.layout = "us";
		rmlvo.variant = "";
		rmlvo.options = "";

		desc->keymap = xkb_map_new_from_names(desc->ctx, &rmlvo, 0);
		if (!desc->keymap) {
			log_warn("failed to create keymap");
			ret = -EFAULT;
			goto err_ctx;
		}
	}

	log_debug("new keyboard description (%s, %s, %s)",
			layout, variant, options);
	*out = desc;
	return 0;

err_ctx:
	xkb_context_unref(desc->ctx);
err_desc:
	free(desc);
	return ret;
}

void kbd_desc_ref(struct kbd_desc *desc)
{
	if (!desc || !desc->ref)
		return;

	++desc->ref;
}

void kbd_desc_unref(struct kbd_desc *desc)
{
	if (!desc || !desc->ref || --desc->ref)
		return;

	log_debug("destroying keyboard description");
	xkb_map_unref(desc->keymap);
	xkb_context_unref(desc->ctx);
	free(desc);
}

void kbd_keysym_to_string(uint32_t keysym, char *str, size_t size)
{
	xkb_keysym_get_name(keysym, str, size);
}
