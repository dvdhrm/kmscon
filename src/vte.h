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
#include "unicode.h"
#include "eloop.h"

struct kmscon_vte;

typedef void (*kmscon_vte_changed_cb) (struct kmscon_vte *vte, void *data);
typedef void (*kmscon_vte_closed_cb) (struct kmscon_vte *vte, void *data);

int kmscon_vte_new(struct kmscon_vte **out,
				kmscon_vte_changed_cb changed_cb, void *data);
void kmscon_vte_ref(struct kmscon_vte *vte);
void kmscon_vte_unref(struct kmscon_vte *vte);

int kmscon_vte_open(struct kmscon_vte *vte, struct kmscon_eloop *eloop,
				kmscon_vte_closed_cb closed_cb, void *data);
void kmscon_vte_close(struct kmscon_vte *vte);

void kmscon_vte_bind(struct kmscon_vte *vte, struct kmscon_console *con);
void kmscon_vte_resize(struct kmscon_vte *vte);

void kmscon_vte_input(struct kmscon_vte *vte, struct kmscon_input_event *ev);
void kmscon_vte_putc(struct kmscon_vte *vte, kmscon_symbol_t ch);

#endif /* KMSCON_VTE_H */
