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

#ifndef KMSCON_VTE_H
#define KMSCON_VTE_H

#include <stdlib.h>
#include "console.h"
#include "input.h"
#include "unicode.h"

struct kmscon_vte;

enum kmscon_vte_keyboard_action {
	KMSCON_VTE_DROP,
	KMSCON_VTE_SEND,
};

int kmscon_vte_new(struct kmscon_vte **out, struct kmscon_symbol_table *st);
void kmscon_vte_ref(struct kmscon_vte *vte);
void kmscon_vte_unref(struct kmscon_vte *vte);

void kmscon_vte_bind(struct kmscon_vte *vte, struct kmscon_console *con);
void kmscon_vte_input(struct kmscon_vte *vte, const char *u8, size_t len);
int kmscon_vte_handle_keyboard(struct kmscon_vte *vte,
	const struct kmscon_input_event *ev, const char **u8, size_t *len);

#endif /* KMSCON_VTE_H */
