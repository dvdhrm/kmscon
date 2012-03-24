/*
 * kmscon - translating key presses to input events using keycodes
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <X11/keysym.h>

#include "input.h"
#include "kbd.h"
#include "log.h"
#include "imKStoUCS.h"

#define LOG_SUBSYSTEM "kbd_dumb"

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

struct kmscon_kbd_desc {
	unsigned long ref;

	/*
	 * There is no need for this structure here currently. It can contain
	 * pointers to alternative keytabs and modmaps, if we ever want this
	 * backend to support different languages, etc.
	 */
};

struct kmscon_kbd {
	unsigned long ref;
	struct kmscon_kbd_desc *desc;

	unsigned int mods;
};

/*
 * These tables do not contain all possible keys from linux/input.h.
 * If a keycode does not appear, it is mapped to keysym 0 and regarded as not
 * found.
 */

static const uint32_t keytab_normal[] = {
	[KEY_ESC]         =  XK_Escape,
	[KEY_1]           =  XK_1,
	[KEY_2]           =  XK_2,
	[KEY_3]           =  XK_3,
	[KEY_4]           =  XK_4,
	[KEY_5]           =  XK_5,
	[KEY_6]           =  XK_6,
	[KEY_7]           =  XK_7,
	[KEY_8]           =  XK_8,
	[KEY_9]           =  XK_9,
	[KEY_0]           =  XK_0,
	[KEY_MINUS]       =  XK_minus,
	[KEY_EQUAL]       =  XK_equal,
	[KEY_BACKSPACE]   =  XK_BackSpace,
	[KEY_TAB]         =  XK_Tab,
	[KEY_Q]           =  XK_q,
	[KEY_W]           =  XK_w,
	[KEY_E]           =  XK_e,
	[KEY_R]           =  XK_r,
	[KEY_T]           =  XK_t,
	[KEY_Y]           =  XK_y,
	[KEY_U]           =  XK_u,
	[KEY_I]           =  XK_i,
	[KEY_O]           =  XK_o,
	[KEY_P]           =  XK_p,
	[KEY_LEFTBRACE]   =  XK_bracketleft,
	[KEY_RIGHTBRACE]  =  XK_bracketright,
	[KEY_ENTER]       =  XK_Return,
	[KEY_LEFTCTRL]    =  XK_Control_L,
	[KEY_A]           =  XK_a,
	[KEY_S]           =  XK_s,
	[KEY_D]           =  XK_d,
	[KEY_F]           =  XK_f,
	[KEY_G]           =  XK_g,
	[KEY_H]           =  XK_h,
	[KEY_J]           =  XK_j,
	[KEY_K]           =  XK_k,
	[KEY_L]           =  XK_l,
	[KEY_SEMICOLON]   =  XK_semicolon,
	[KEY_APOSTROPHE]  =  XK_apostrophe,
	[KEY_GRAVE]       =  XK_grave,
	[KEY_LEFTSHIFT]   =  XK_Shift_L,
	[KEY_BACKSLASH]   =  XK_backslash,
	[KEY_Z]           =  XK_z,
	[KEY_X]           =  XK_x,
	[KEY_C]           =  XK_c,
	[KEY_V]           =  XK_v,
	[KEY_B]           =  XK_b,
	[KEY_N]           =  XK_n,
	[KEY_M]           =  XK_m,
	[KEY_COMMA]       =  XK_comma,
	[KEY_DOT]         =  XK_period,
	[KEY_SLASH]       =  XK_slash,
	[KEY_RIGHTSHIFT]  =  XK_Shift_R,
	[KEY_KPASTERISK]  =  XK_KP_Multiply,
	[KEY_LEFTALT]     =  XK_Alt_L,
	[KEY_SPACE]       =  XK_space,
	[KEY_CAPSLOCK]    =  XK_Caps_Lock,
	[KEY_F1]          =  XK_F1,
	[KEY_F2]          =  XK_F2,
	[KEY_F3]          =  XK_F3,
	[KEY_F4]          =  XK_F4,
	[KEY_F5]          =  XK_F5,
	[KEY_F6]          =  XK_F6,
	[KEY_F7]          =  XK_F7,
	[KEY_F8]          =  XK_F8,
	[KEY_F9]          =  XK_F9,
	[KEY_F10]         =  XK_F10,
	[KEY_NUMLOCK]     =  XK_Num_Lock,
	[KEY_SCROLLLOCK]  =  XK_Scroll_Lock,
	[KEY_KP7]         =  XK_KP_Home,
	[KEY_KP8]         =  XK_KP_Up,
	[KEY_KP9]         =  XK_KP_Page_Up,
	[KEY_KPMINUS]     =  XK_KP_Subtract,
	[KEY_KP4]         =  XK_KP_Left,
	[KEY_KP5]         =  XK_KP_Begin,
	[KEY_KP6]         =  XK_KP_Right,
	[KEY_KPPLUS]      =  XK_KP_Add,
	[KEY_KP1]         =  XK_KP_End,
	[KEY_KP2]         =  XK_KP_Down,
	[KEY_KP3]         =  XK_KP_Page_Down,
	[KEY_KP0]         =  XK_KP_Insert,
	[KEY_KPDOT]       =  XK_KP_Delete,
	[KEY_F11]         =  XK_F11,
	[KEY_F12]         =  XK_F12,
	[KEY_KPENTER]     =  XK_KP_Enter,
	[KEY_RIGHTCTRL]   =  XK_Control_R,
	[KEY_KPSLASH]     =  XK_KP_Divide,
	[KEY_RIGHTALT]    =  XK_Alt_R,
	[KEY_LINEFEED]    =  XK_Linefeed,
	[KEY_HOME]        =  XK_Home,
	[KEY_UP]          =  XK_Up,
	[KEY_PAGEUP]      =  XK_Page_Up,
	[KEY_LEFT]        =  XK_Left,
	[KEY_RIGHT]       =  XK_Right,
	[KEY_END]         =  XK_End,
	[KEY_DOWN]        =  XK_Down,
	[KEY_PAGEDOWN]    =  XK_Page_Down,
	[KEY_INSERT]      =  XK_Insert,
	[KEY_DELETE]      =  XK_Delete,
	[KEY_KPEQUAL]     =  XK_KP_Equal,
	[KEY_LEFTMETA]    =  XK_Meta_L,
	[KEY_RIGHTMETA]   =  XK_Meta_R,
};
#define KEYTAB_SIZE (KEY_RIGHTMETA + 1)
_Static_assert(
	(KEYTAB_SIZE == sizeof(keytab_normal) / sizeof(*keytab_normal)),
	"The KEYTAB_SIZE #define is incorrect!"
);

static const uint32_t keytab_numlock[KEYTAB_SIZE] = {
	[KEY_KP7]         =  XK_KP_7,
	[KEY_KP8]         =  XK_KP_8,
	[KEY_KP9]         =  XK_KP_9,
	[KEY_KP4]         =  XK_KP_4,
	[KEY_KP5]         =  XK_KP_5,
	[KEY_KP6]         =  XK_KP_6,
	[KEY_KP1]         =  XK_KP_1,
	[KEY_KP2]         =  XK_KP_2,
	[KEY_KP3]         =  XK_KP_3,
	[KEY_KP0]         =  XK_KP_0,
};

static const uint32_t keytab_shift[KEYTAB_SIZE] = {
	[KEY_1]           =  XK_exclam,
	[KEY_2]           =  XK_at,
	[KEY_3]           =  XK_numbersign,
	[KEY_4]           =  XK_dollar,
	[KEY_5]           =  XK_percent,
	[KEY_6]           =  XK_asciicircum,
	[KEY_7]           =  XK_ampersand,
	[KEY_8]           =  XK_asterisk,
	[KEY_9]           =  XK_parenleft,
	[KEY_0]           =  XK_parenright,
	[KEY_MINUS]       =  XK_underscore,
	[KEY_EQUAL]       =  XK_plus,
	[KEY_Q]           =  XK_Q,
	[KEY_W]           =  XK_W,
	[KEY_E]           =  XK_E,
	[KEY_R]           =  XK_R,
	[KEY_T]           =  XK_T,
	[KEY_Y]           =  XK_Y,
	[KEY_U]           =  XK_U,
	[KEY_I]           =  XK_I,
	[KEY_O]           =  XK_O,
	[KEY_P]           =  XK_P,
	[KEY_LEFTBRACE]   =  XK_braceleft,
	[KEY_RIGHTBRACE]  =  XK_braceright,
	[KEY_A]           =  XK_A,
	[KEY_S]           =  XK_S,
	[KEY_D]           =  XK_D,
	[KEY_F]           =  XK_F,
	[KEY_G]           =  XK_G,
	[KEY_H]           =  XK_H,
	[KEY_J]           =  XK_J,
	[KEY_K]           =  XK_K,
	[KEY_L]           =  XK_L,
	[KEY_SEMICOLON]   =  XK_colon,
	[KEY_APOSTROPHE]  =  XK_quotedbl,
	[KEY_GRAVE]       =  XK_asciitilde,
	[KEY_BACKSLASH]   =  XK_bar,
	[KEY_Z]           =  XK_Z,
	[KEY_X]           =  XK_X,
	[KEY_C]           =  XK_C,
	[KEY_V]           =  XK_V,
	[KEY_B]           =  XK_B,
	[KEY_N]           =  XK_N,
	[KEY_M]           =  XK_M,
	[KEY_COMMA]       =  XK_less,
	[KEY_DOT]         =  XK_greater,
	[KEY_SLASH]       =  XK_question,
};

static const uint32_t keytab_capslock[KEYTAB_SIZE] = {
	[KEY_Q]           =  XK_Q,
	[KEY_W]           =  XK_W,
	[KEY_E]           =  XK_E,
	[KEY_R]           =  XK_R,
	[KEY_T]           =  XK_T,
	[KEY_Y]           =  XK_Y,
	[KEY_U]           =  XK_U,
	[KEY_I]           =  XK_I,
	[KEY_O]           =  XK_O,
	[KEY_P]           =  XK_P,
	[KEY_A]           =  XK_A,
	[KEY_S]           =  XK_S,
	[KEY_D]           =  XK_D,
	[KEY_F]           =  XK_F,
	[KEY_G]           =  XK_G,
	[KEY_H]           =  XK_H,
	[KEY_J]           =  XK_J,
	[KEY_K]           =  XK_K,
	[KEY_L]           =  XK_L,
	[KEY_Z]           =  XK_Z,
	[KEY_X]           =  XK_X,
	[KEY_C]           =  XK_C,
	[KEY_V]           =  XK_V,
	[KEY_B]           =  XK_B,
	[KEY_N]           =  XK_N,
	[KEY_M]           =  XK_M,
};

static const struct {
	enum kmscon_modifier mod;
	enum {
		MOD_NORMAL = 1,
		MOD_LOCK,
	} type;
} modmap[KEYTAB_SIZE] = {
	[KEY_LEFTCTRL]    =  {  KMSCON_CONTROL_MASK,  MOD_NORMAL  },
	[KEY_LEFTSHIFT]   =  {  KMSCON_SHIFT_MASK,    MOD_NORMAL  },
	[KEY_RIGHTSHIFT]  =  {  KMSCON_SHIFT_MASK,    MOD_NORMAL  },
	[KEY_LEFTALT]     =  {  KMSCON_MOD1_MASK,     MOD_NORMAL  },
	[KEY_CAPSLOCK]    =  {  KMSCON_LOCK_MASK,     MOD_LOCK    },
	[KEY_NUMLOCK]     =  {  KMSCON_MOD2_MASK,     MOD_LOCK    },
	[KEY_RIGHTCTRL]   =  {  KMSCON_CONTROL_MASK,  MOD_NORMAL  },
	[KEY_RIGHTALT]    =  {  KMSCON_MOD1_MASK,     MOD_NORMAL  },
	[KEY_LEFTMETA]    =  {  KMSCON_MOD4_MASK,     MOD_NORMAL  },
	[KEY_RIGHTMETA]   =  {  KMSCON_MOD4_MASK,     MOD_NORMAL  },
};

int kmscon_kbd_new(struct kmscon_kbd **out, struct kmscon_kbd_desc *desc)
{
	struct kmscon_kbd *kbd;

	kbd = malloc(sizeof(*kbd));
	if (!kbd)
		return -ENOMEM;

	memset(kbd, 0, sizeof(*kbd));
	kbd->ref = 1;

	kbd->desc = desc;
	kmscon_kbd_desc_ref(desc);

	*out = kbd;
	return 0;
}

void kmscon_kbd_ref(struct kmscon_kbd *kbd)
{
	if (!kbd)
		return;

	++kbd->ref;
}

void kmscon_kbd_unref(struct kmscon_kbd *kbd)
{
	if (!kbd || !kbd->ref)
		return;

	if (--kbd->ref)
		return;

	kmscon_kbd_desc_unref(kbd->desc);
	free(kbd);
}

void kmscon_kbd_reset(struct kmscon_kbd *kbd, const unsigned long *ledbits)
{
	if (!kbd)
		return;

	kbd->mods = 0;

	if (kmscon_evdev_bit_is_set(ledbits, LED_NUML))
		kbd->mods |= KMSCON_MOD2_MASK;
	if (kmscon_evdev_bit_is_set(ledbits, LED_CAPSL))
		kbd->mods |= KMSCON_LOCK_MASK;
}

int kmscon_kbd_process_key(struct kmscon_kbd *kbd,
					enum kmscon_key_state key_state,
					uint16_t code,
					struct kmscon_input_event *out)
{
	uint32_t keysym;
	enum kmscon_modifier mod;
	int mod_type;

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
		if (key_state == KMSCON_KEY_PRESSED) {
			if (mod_type == MOD_NORMAL)
				kbd->mods |= mod;
			else if (mod_type == MOD_LOCK)
				kbd->mods ^= mod;
		} else if (key_state == KMSCON_KEY_RELEASED) {
			if (mod_type == MOD_NORMAL)
				kbd->mods &= ~mod;
		}

		/* Don't deliver events purely for modifiers. */
		return -ENOKEY;
	}

	if (key_state == KMSCON_KEY_RELEASED)
		return -ENOKEY;

	keysym = 0;

	if (!keysym && kbd->mods & KMSCON_MOD2_MASK)
		keysym = keytab_numlock[code];
	if (!keysym && kbd->mods & KMSCON_SHIFT_MASK)
		keysym = keytab_shift[code];
	if (!keysym && kbd->mods & KMSCON_LOCK_MASK)
		keysym = keytab_capslock[code];
	if (!keysym)
		keysym = keytab_normal[code];

	if (!keysym)
		return -ENOKEY;

	out->keycode = code;
	out->keysym = keysym;
	out->unicode = KeysymToUcs4(keysym) ?: KMSCON_INPUT_INVALID;
	out->mods = kbd->mods;

	return 0;
}

int kmscon_kbd_desc_new(struct kmscon_kbd_desc **out, const char *layout,
				const char *variant, const char *options)
{
	struct kmscon_kbd_desc *desc;

	if (!out)
		return -EINVAL;

	desc = malloc(sizeof(*desc));
	if (!desc)
		return -ENOMEM;

	memset(desc, 0, sizeof(*desc));
	desc->ref = 1;

	log_debug("new keyboard description (%s, %s, %s)",
						layout, variant, options);
	*out = desc;
	return 0;
}

void kmscon_kbd_desc_ref(struct kmscon_kbd_desc *desc)
{
	if (!desc)
		return;

	++desc->ref;
}

void kmscon_kbd_desc_unref(struct kmscon_kbd_desc *desc)
{
	if (!desc || !desc->ref)
		return;

	if (--desc->ref)
		return;

	log_debug("destroying keyboard description");

	free(desc);
}

void kmscon_kbd_keysym_to_string(uint32_t keysym, char *str, size_t size)
{
	snprintf(str, size, "%#x", keysym);
}
