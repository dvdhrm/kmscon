/*
 * kmscon - VT Emulator
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * Virtual Terminal Emulator
 * This is a vt100 implementation. It is written from scratch. It uses the
 * console subsystem as output and is tightly bound to it.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

#include "console.h"
#include "input.h"
#include "log.h"
#include "unicode.h"
#include "vte.h"

struct kmscon_vte {
	unsigned long ref;
	struct kmscon_symbol_table *st;
	struct kmscon_console *con;

	const char *kbd_sym;
	struct kmscon_utf8_mach *mach;
};

int kmscon_vte_new(struct kmscon_vte **out, struct kmscon_symbol_table *st)
{
	struct kmscon_vte *vte;
	int ret;

	if (!out)
		return -EINVAL;

	log_debug("vte: new vte object\n");

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->ref = 1;
	vte->st = st;

	ret = kmscon_utf8_mach_new(&vte->mach);
	if (ret)
		goto err_free;

	kmscon_symbol_table_ref(vte->st);
	*out = vte;
	return 0;

err_free:
	free(vte);
	return ret;
}

void kmscon_vte_ref(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

void kmscon_vte_unref(struct kmscon_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	kmscon_console_unref(vte->con);
	kmscon_utf8_mach_free(vte->mach);
	kmscon_symbol_free_u8(vte->kbd_sym);
	kmscon_symbol_table_unref(vte->st);
	free(vte);
	log_debug("vte: destroying vte object\n");
}

void kmscon_vte_bind(struct kmscon_vte *vte, struct kmscon_console *con)
{
	if (!vte)
		return;

	kmscon_console_unref(vte->con);
	vte->con = con;
	kmscon_console_ref(vte->con);
}

void kmscon_vte_input(struct kmscon_vte *vte, const char *u8, size_t len)
{
	int state, i;
	uint32_t ucs4;
	kmscon_symbol_t sym;

	if (!vte || !vte->con)
		return;

	for (i = 0; i < len; ++i) {
		state = kmscon_utf8_mach_feed(vte->mach, u8[i]);
		if (state == KMSCON_UTF8_ACCEPT ||
				state == KMSCON_UTF8_REJECT) {
			ucs4 = kmscon_utf8_mach_get(vte->mach);
			if (ucs4 == '\n') {
				kmscon_console_newline(vte->con);
			} else {
				sym = kmscon_symbol_make(ucs4);
				kmscon_console_write(vte->con, sym);
			}
		}
	}
}

int kmscon_vte_handle_keyboard(struct kmscon_vte *vte,
	const struct kmscon_input_event *ev, const char **u8, size_t *len)
{
	kmscon_symbol_t sym;

	switch (ev->keysym) {
		case XK_BackSpace:
			*u8 = "\x08";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Tab:
		case XK_KP_Tab:
			*u8 = "\x09";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Linefeed:
			*u8 = "\x0a";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Clear:
			*u8 = "\x0b";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Pause:
			*u8 = "\x13";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Scroll_Lock:
			/* TODO: do we need scroll lock impl.? */
			*u8 = "\x14";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Sys_Req:
			*u8 = "\x15";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Escape:
			*u8 = "\x1b";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Return:
		case XK_KP_Enter:
			/* TODO: im CR/LF mode send \x0d\x0a */
			*u8 = "\x0d";
			*len = 1;
			return KMSCON_VTE_SEND;
	}

	if (ev->unicode != KMSCON_INPUT_INVALID) {
		kmscon_symbol_free_u8(vte->kbd_sym);
		sym = kmscon_symbol_make(ev->unicode);
		vte->kbd_sym = kmscon_symbol_get_u8(vte->st, sym, len);
		*u8 = vte->kbd_sym;
		return KMSCON_VTE_SEND;
	}

	return KMSCON_VTE_DROP;
}
