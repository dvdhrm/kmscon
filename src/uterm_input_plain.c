/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
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

/*
 * This is a very "dumb" and simple fallback backend for keycodes
 * interpretation. It uses direct mapping from kernel keycodes to X keysyms
 * according to a basic US PC keyboard. It is not configurable and does not
 * support unicode or other languages.
 *
 * The key interpretation is affected by the following modifiers: Numlock,
 * Shift, Capslock, and "Normal" (no mofifiers) in that order. If a keycode is
 * not affected by one of these depressed modifiers, the next matching one is
 * attempted.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include "imKStoUCS.h"
#include "log.h"
#include "uterm.h"
#include "uterm_input.h"

#define LOG_SUBSYSTEM "input_plain"

/*
 * These tables do not contain all possible keys from linux/input.h.
 * If a keycode does not appear, it is mapped to keysym 0 and regarded as not
 * found.
 */

static const uint32_t keytab_normal[] = {
	[KEY_ESC]         =  XKB_KEY_Escape,
	[KEY_1]           =  XKB_KEY_1,
	[KEY_2]           =  XKB_KEY_2,
	[KEY_3]           =  XKB_KEY_3,
	[KEY_4]           =  XKB_KEY_4,
	[KEY_5]           =  XKB_KEY_5,
	[KEY_6]           =  XKB_KEY_6,
	[KEY_7]           =  XKB_KEY_7,
	[KEY_8]           =  XKB_KEY_8,
	[KEY_9]           =  XKB_KEY_9,
	[KEY_0]           =  XKB_KEY_0,
	[KEY_MINUS]       =  XKB_KEY_minus,
	[KEY_EQUAL]       =  XKB_KEY_equal,
	[KEY_BACKSPACE]   =  XKB_KEY_BackSpace,
	[KEY_TAB]         =  XKB_KEY_Tab,
	[KEY_Q]           =  XKB_KEY_q,
	[KEY_W]           =  XKB_KEY_w,
	[KEY_E]           =  XKB_KEY_e,
	[KEY_R]           =  XKB_KEY_r,
	[KEY_T]           =  XKB_KEY_t,
	[KEY_Y]           =  XKB_KEY_y,
	[KEY_U]           =  XKB_KEY_u,
	[KEY_I]           =  XKB_KEY_i,
	[KEY_O]           =  XKB_KEY_o,
	[KEY_P]           =  XKB_KEY_p,
	[KEY_LEFTBRACE]   =  XKB_KEY_bracketleft,
	[KEY_RIGHTBRACE]  =  XKB_KEY_bracketright,
	[KEY_ENTER]       =  XKB_KEY_Return,
	[KEY_LEFTCTRL]    =  XKB_KEY_Control_L,
	[KEY_A]           =  XKB_KEY_a,
	[KEY_S]           =  XKB_KEY_s,
	[KEY_D]           =  XKB_KEY_d,
	[KEY_F]           =  XKB_KEY_f,
	[KEY_G]           =  XKB_KEY_g,
	[KEY_H]           =  XKB_KEY_h,
	[KEY_J]           =  XKB_KEY_j,
	[KEY_K]           =  XKB_KEY_k,
	[KEY_L]           =  XKB_KEY_l,
	[KEY_SEMICOLON]   =  XKB_KEY_semicolon,
	[KEY_APOSTROPHE]  =  XKB_KEY_apostrophe,
	[KEY_GRAVE]       =  XKB_KEY_grave,
	[KEY_LEFTSHIFT]   =  XKB_KEY_Shift_L,
	[KEY_BACKSLASH]   =  XKB_KEY_backslash,
	[KEY_Z]           =  XKB_KEY_z,
	[KEY_X]           =  XKB_KEY_x,
	[KEY_C]           =  XKB_KEY_c,
	[KEY_V]           =  XKB_KEY_v,
	[KEY_B]           =  XKB_KEY_b,
	[KEY_N]           =  XKB_KEY_n,
	[KEY_M]           =  XKB_KEY_m,
	[KEY_COMMA]       =  XKB_KEY_comma,
	[KEY_DOT]         =  XKB_KEY_period,
	[KEY_SLASH]       =  XKB_KEY_slash,
	[KEY_RIGHTSHIFT]  =  XKB_KEY_Shift_R,
	[KEY_KPASTERISK]  =  XKB_KEY_KP_Multiply,
	[KEY_LEFTALT]     =  XKB_KEY_Alt_L,
	[KEY_SPACE]       =  XKB_KEY_space,
	[KEY_CAPSLOCK]    =  XKB_KEY_Caps_Lock,
	[KEY_F1]          =  XKB_KEY_F1,
	[KEY_F2]          =  XKB_KEY_F2,
	[KEY_F3]          =  XKB_KEY_F3,
	[KEY_F4]          =  XKB_KEY_F4,
	[KEY_F5]          =  XKB_KEY_F5,
	[KEY_F6]          =  XKB_KEY_F6,
	[KEY_F7]          =  XKB_KEY_F7,
	[KEY_F8]          =  XKB_KEY_F8,
	[KEY_F9]          =  XKB_KEY_F9,
	[KEY_F10]         =  XKB_KEY_F10,
	[KEY_NUMLOCK]     =  XKB_KEY_Num_Lock,
	[KEY_SCROLLLOCK]  =  XKB_KEY_Scroll_Lock,
	[KEY_KP7]         =  XKB_KEY_KP_Home,
	[KEY_KP8]         =  XKB_KEY_KP_Up,
	[KEY_KP9]         =  XKB_KEY_KP_Page_Up,
	[KEY_KPMINUS]     =  XKB_KEY_KP_Subtract,
	[KEY_KP4]         =  XKB_KEY_KP_Left,
	[KEY_KP5]         =  XKB_KEY_KP_Begin,
	[KEY_KP6]         =  XKB_KEY_KP_Right,
	[KEY_KPPLUS]      =  XKB_KEY_KP_Add,
	[KEY_KP1]         =  XKB_KEY_KP_End,
	[KEY_KP2]         =  XKB_KEY_KP_Down,
	[KEY_KP3]         =  XKB_KEY_KP_Page_Down,
	[KEY_KP0]         =  XKB_KEY_KP_Insert,
	[KEY_KPDOT]       =  XKB_KEY_KP_Delete,
	[KEY_F11]         =  XKB_KEY_F11,
	[KEY_F12]         =  XKB_KEY_F12,
	[KEY_KPENTER]     =  XKB_KEY_KP_Enter,
	[KEY_RIGHTCTRL]   =  XKB_KEY_Control_R,
	[KEY_KPSLASH]     =  XKB_KEY_KP_Divide,
	[KEY_RIGHTALT]    =  XKB_KEY_Alt_R,
	[KEY_LINEFEED]    =  XKB_KEY_Linefeed,
	[KEY_HOME]        =  XKB_KEY_Home,
	[KEY_UP]          =  XKB_KEY_Up,
	[KEY_PAGEUP]      =  XKB_KEY_Page_Up,
	[KEY_LEFT]        =  XKB_KEY_Left,
	[KEY_RIGHT]       =  XKB_KEY_Right,
	[KEY_END]         =  XKB_KEY_End,
	[KEY_DOWN]        =  XKB_KEY_Down,
	[KEY_PAGEDOWN]    =  XKB_KEY_Page_Down,
	[KEY_INSERT]      =  XKB_KEY_Insert,
	[KEY_DELETE]      =  XKB_KEY_Delete,
	[KEY_KPEQUAL]     =  XKB_KEY_KP_Equal,
	[KEY_LEFTMETA]    =  XKB_KEY_Meta_L,
	[KEY_RIGHTMETA]   =  XKB_KEY_Meta_R,
};

#define KEYTAB_SIZE (KEY_RIGHTMETA + 1)

#ifdef BUILD_HAVE_STATIC_ASSERT
_Static_assert(
	(KEYTAB_SIZE == sizeof(keytab_normal) / sizeof(*keytab_normal)),
	"The KEYTAB_SIZE #define is incorrect!"
);
#endif

static const uint32_t keytab_numlock[KEYTAB_SIZE] = {
	[KEY_KP7]         =  XKB_KEY_KP_7,
	[KEY_KP8]         =  XKB_KEY_KP_8,
	[KEY_KP9]         =  XKB_KEY_KP_9,
	[KEY_KP4]         =  XKB_KEY_KP_4,
	[KEY_KP5]         =  XKB_KEY_KP_5,
	[KEY_KP6]         =  XKB_KEY_KP_6,
	[KEY_KP1]         =  XKB_KEY_KP_1,
	[KEY_KP2]         =  XKB_KEY_KP_2,
	[KEY_KP3]         =  XKB_KEY_KP_3,
	[KEY_KP0]         =  XKB_KEY_KP_0,
};

static const uint32_t keytab_shift[KEYTAB_SIZE] = {
	[KEY_1]           =  XKB_KEY_exclam,
	[KEY_2]           =  XKB_KEY_at,
	[KEY_3]           =  XKB_KEY_numbersign,
	[KEY_4]           =  XKB_KEY_dollar,
	[KEY_5]           =  XKB_KEY_percent,
	[KEY_6]           =  XKB_KEY_asciicircum,
	[KEY_7]           =  XKB_KEY_ampersand,
	[KEY_8]           =  XKB_KEY_asterisk,
	[KEY_9]           =  XKB_KEY_parenleft,
	[KEY_0]           =  XKB_KEY_parenright,
	[KEY_MINUS]       =  XKB_KEY_underscore,
	[KEY_EQUAL]       =  XKB_KEY_plus,
	[KEY_Q]           =  XKB_KEY_Q,
	[KEY_W]           =  XKB_KEY_W,
	[KEY_E]           =  XKB_KEY_E,
	[KEY_R]           =  XKB_KEY_R,
	[KEY_T]           =  XKB_KEY_T,
	[KEY_Y]           =  XKB_KEY_Y,
	[KEY_U]           =  XKB_KEY_U,
	[KEY_I]           =  XKB_KEY_I,
	[KEY_O]           =  XKB_KEY_O,
	[KEY_P]           =  XKB_KEY_P,
	[KEY_LEFTBRACE]   =  XKB_KEY_braceleft,
	[KEY_RIGHTBRACE]  =  XKB_KEY_braceright,
	[KEY_A]           =  XKB_KEY_A,
	[KEY_S]           =  XKB_KEY_S,
	[KEY_D]           =  XKB_KEY_D,
	[KEY_F]           =  XKB_KEY_F,
	[KEY_G]           =  XKB_KEY_G,
	[KEY_H]           =  XKB_KEY_H,
	[KEY_J]           =  XKB_KEY_J,
	[KEY_K]           =  XKB_KEY_K,
	[KEY_L]           =  XKB_KEY_L,
	[KEY_SEMICOLON]   =  XKB_KEY_colon,
	[KEY_APOSTROPHE]  =  XKB_KEY_quotedbl,
	[KEY_GRAVE]       =  XKB_KEY_asciitilde,
	[KEY_BACKSLASH]   =  XKB_KEY_bar,
	[KEY_Z]           =  XKB_KEY_Z,
	[KEY_X]           =  XKB_KEY_X,
	[KEY_C]           =  XKB_KEY_C,
	[KEY_V]           =  XKB_KEY_V,
	[KEY_B]           =  XKB_KEY_B,
	[KEY_N]           =  XKB_KEY_N,
	[KEY_M]           =  XKB_KEY_M,
	[KEY_COMMA]       =  XKB_KEY_less,
	[KEY_DOT]         =  XKB_KEY_greater,
	[KEY_SLASH]       =  XKB_KEY_question,
};

static const uint32_t keytab_capslock[KEYTAB_SIZE] = {
	[KEY_Q]           =  XKB_KEY_Q,
	[KEY_W]           =  XKB_KEY_W,
	[KEY_E]           =  XKB_KEY_E,
	[KEY_R]           =  XKB_KEY_R,
	[KEY_T]           =  XKB_KEY_T,
	[KEY_Y]           =  XKB_KEY_Y,
	[KEY_U]           =  XKB_KEY_U,
	[KEY_I]           =  XKB_KEY_I,
	[KEY_O]           =  XKB_KEY_O,
	[KEY_P]           =  XKB_KEY_P,
	[KEY_A]           =  XKB_KEY_A,
	[KEY_S]           =  XKB_KEY_S,
	[KEY_D]           =  XKB_KEY_D,
	[KEY_F]           =  XKB_KEY_F,
	[KEY_G]           =  XKB_KEY_G,
	[KEY_H]           =  XKB_KEY_H,
	[KEY_J]           =  XKB_KEY_J,
	[KEY_K]           =  XKB_KEY_K,
	[KEY_L]           =  XKB_KEY_L,
	[KEY_Z]           =  XKB_KEY_Z,
	[KEY_X]           =  XKB_KEY_X,
	[KEY_C]           =  XKB_KEY_C,
	[KEY_V]           =  XKB_KEY_V,
	[KEY_B]           =  XKB_KEY_B,
	[KEY_N]           =  XKB_KEY_N,
	[KEY_M]           =  XKB_KEY_M,
};

static const struct {
	unsigned int mod;
	enum {
		MOD_NORMAL = 1,
		MOD_LOCK,
	} type;
} modmap[KEYTAB_SIZE] = {
	[KEY_LEFTCTRL]    =  {  UTERM_CONTROL_MASK,  MOD_NORMAL  },
	[KEY_LEFTSHIFT]   =  {  UTERM_SHIFT_MASK,    MOD_NORMAL  },
	[KEY_RIGHTSHIFT]  =  {  UTERM_SHIFT_MASK,    MOD_NORMAL  },
	[KEY_LEFTALT]     =  {  UTERM_ALT_MASK,      MOD_NORMAL  },
	[KEY_CAPSLOCK]    =  {  UTERM_LOCK_MASK,     MOD_LOCK    },
	[KEY_RIGHTCTRL]   =  {  UTERM_CONTROL_MASK,  MOD_NORMAL  },
	[KEY_RIGHTALT]    =  {  UTERM_ALT_MASK,      MOD_NORMAL  },
	[KEY_LEFTMETA]    =  {  UTERM_LOGO_MASK,     MOD_NORMAL  },
	[KEY_RIGHTMETA]   =  {  UTERM_LOGO_MASK,     MOD_NORMAL  },
};

static void plain_dev_ref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref)
		return;

	++kbd->ref;
}

static void plain_dev_unref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref || --kbd->ref)
		return;

	free(kbd);
}

static void plain_dev_reset(struct kbd_dev *kbd, const unsigned long *ledbits)
{
	if (!kbd)
		return;

	kbd->plain.mods = 0;

	if (input_bit_is_set(ledbits, LED_CAPSL))
		kbd->plain.mods |= UTERM_LOCK_MASK;
}

static int plain_dev_process(struct kbd_dev *kbd,
			     uint16_t key_state,
			     uint16_t code,
			     struct uterm_input_event *out)
{
	uint32_t keysym;
	unsigned int mod;
	int mod_type;

	if (!kbd)
		return -EINVAL;

	/* Ignore unknown keycodes. */
	if (code >= KEYTAB_SIZE)
		return -ENOKEY;

	if (modmap[code].mod) {
		mod = modmap[code].mod;
		mod_type = modmap[code].type;

		/*
		 * We release locked modifiers on key press, like the kernel,
		 * but unlike XKB.
		 */
		if (key_state == 1) {
			if (mod_type == MOD_NORMAL)
				kbd->plain.mods |= mod;
			else if (mod_type == MOD_LOCK)
				kbd->plain.mods ^= mod;
		} else if (key_state == 0) {
			if (mod_type == MOD_NORMAL)
				kbd->plain.mods &= ~mod;
		}

		/* Don't deliver events purely for modifiers. */
		return -ENOKEY;
	}

	if (key_state == 0)
		return -ENOKEY;

	keysym = 0;

	if (!keysym && kbd->plain.mods & UTERM_SHIFT_MASK)
		keysym = keytab_shift[code];
	if (!keysym && kbd->plain.mods & UTERM_LOCK_MASK)
		keysym = keytab_capslock[code];
	if (!keysym)
		keysym = keytab_normal[code];

	if (!keysym)
		return -ENOKEY;

	out->keycode = code;
	out->keysym = keysym;
	out->unicode = KeysymToUcs4(keysym) ?: UTERM_INPUT_INVALID;
	out->mods = kbd->plain.mods;

	return 0;
}

static int plain_desc_init(struct kbd_desc **out,
			   const char *layout,
			   const char *variant,
			   const char *options)
{
	struct kbd_desc *desc;

	if (!out)
		return -EINVAL;

	desc = malloc(sizeof(*desc));
	if (!desc)
		return -ENOMEM;
	memset(desc, 0, sizeof(*desc));
	desc->ops = &plain_desc_ops;

	log_debug("new keyboard description (%s, %s, %s)",
		  layout, variant, options);
	*out = desc;
	return 0;
}

static void plain_desc_ref(struct kbd_desc *desc)
{
	if (!desc || !desc->ref)
		return;

	++desc->ref;
}

static void plain_desc_unref(struct kbd_desc *desc)
{
	if (!desc || !desc->ref || --desc->ref)
		return;

	log_debug("destroying keyboard description");
	free(desc);
}

static int plain_desc_alloc(struct kbd_desc *desc, struct kbd_dev **out)
{
	struct kbd_dev *kbd;

	kbd = malloc(sizeof(*kbd));
	if (!kbd)
		return -ENOMEM;
	memset(kbd, 0, sizeof(*kbd));
	kbd->ref = 1;
	kbd->ops = &plain_dev_ops;

	*out = kbd;
	return 0;
}

static void plain_keysym_to_string(uint32_t keysym, char *str, size_t size)
{
	snprintf(str, size, "%#x", keysym);
}

int plain_string_to_keysym(const char *n, uint32_t *out)
{
	/* TODO: we really need to implement this; maybe use a hashtable similar
	 * to the Xlib? */
	return -EOPNOTSUPP;
}

const struct kbd_desc_ops plain_desc_ops = {
	.init = plain_desc_init,
	.ref = plain_desc_ref,
	.unref = plain_desc_unref,
	.alloc = plain_desc_alloc,
	.keysym_to_string = plain_keysym_to_string,
	.string_to_keysym = plain_string_to_keysym,
};

const struct kbd_dev_ops plain_dev_ops = {
	.ref = plain_dev_ref,
	.unref = plain_dev_unref,
	.reset = plain_dev_reset,
	.process = plain_dev_process,
};
