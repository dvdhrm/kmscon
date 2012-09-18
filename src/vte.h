/*
 * TSM - VT Emulator
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
 * screen state-machine as output and is tightly bound to it.
 */

#ifndef TSM_VTE_H
#define TSM_VTE_H

#include <stdlib.h>
#include "tsm_screen.h"
#include "tsm_unicode.h"

/* available character sets */

typedef tsm_symbol_t tsm_vte_charset[96];

extern tsm_vte_charset tsm_vte_unicode_lower;
extern tsm_vte_charset tsm_vte_unicode_upper;
extern tsm_vte_charset tsm_vte_dec_supplemental_graphics;
extern tsm_vte_charset tsm_vte_dec_special_graphics;

/* virtual terminal emulator */

struct kmscon_vte;

enum tsm_vte_keyboard_action {
	tsm_vte_DROP,
	tsm_vte_SEND,
};

#define TSM_VTE_INVALID 0xffffffff

typedef void (*tsm_vte_write_cb) (struct kmscon_vte *vte,
				     const char *u8,
				     size_t len,
				     void *data);

int tsm_vte_new(struct kmscon_vte **out, struct tsm_screen *con,
		   tsm_vte_write_cb write_cb, void *data);
void tsm_vte_ref(struct kmscon_vte *vte);
void tsm_vte_unref(struct kmscon_vte *vte);

void tsm_vte_reset(struct kmscon_vte *vte);
void tsm_vte_input(struct kmscon_vte *vte, const char *u8, size_t len);
bool tsm_vte_handle_keyboard(struct kmscon_vte *vte, uint32_t keysym,
				unsigned int mods, uint32_t unicode);

#endif /* TSM_VTE_H */
