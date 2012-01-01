/*
 * kmscon - Unicode Handling
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
 * Unicode Handling
 * The main goal of the symbol(_table) functions is to provide a datatype which
 * can contain the representation of any printable character. This includes all
 * basic Unicode characters but also combined characters.
 * To avoid all the memory management we still represent a character as a single
 * integer value (kmscon_symbol_t) but internally we allocate a string which is
 * represented by this value.
 *
 * A kmscon_symbol_t is an integer which represents a single character point.
 * For most Unicode characters this is simply the UCS4 representation. In fact,
 * every UCS4 characters is a valid kmscon_symbol_t object.
 * However, Unicode standard allows combining marks. Therefore, some characters
 * consists of more than one Unicode character.
 * A kmscon_symbol_table object provides all those combined characters as single
 * integers. You simply create a valid base character and append your combining
 * marks and the table will return a new valid kmscon_symbol_t. It is no longer
 * a valid UCS4 value, though. But no memory management is needed as all
 * kmscon_symbol_t objects are simple integers.
 */

#ifndef KMSCON_UNICODE_H
#define KMSCON_UNICODE_H

#include <inttypes.h>
#include <stdlib.h>

struct kmscon_symbol_table;
typedef uint32_t kmscon_symbol_t;

int kmscon_symbol_table_new(struct kmscon_symbol_table **out);
void kmscon_symbol_table_ref(struct kmscon_symbol_table *st);
void kmscon_symbol_table_unref(struct kmscon_symbol_table *st);

kmscon_symbol_t kmscon_symbol_make(uint32_t ucs4);
kmscon_symbol_t kmscon_symbol_append(struct kmscon_symbol_table *st,
					kmscon_symbol_t sym, uint32_t ucs4);
const uint32_t *kmscon_symbol_get(const struct kmscon_symbol_table *st,
					kmscon_symbol_t *sym, size_t *size);

#endif /* KMSCON_UNICODE_H */
