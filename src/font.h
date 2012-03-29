/*
 * kmscon - Font Management
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
 * Font Management
 * A font factory helps loading and initializing fonts. The font object is used
 * to draw glyphs onto the screen.
 * Efficient caching is used to allow fast drawing operations.
 */

#ifndef KMSCON_FONT_H
#define KMSCON_FONT_H

#include <stdlib.h>
#include "gl.h"
#include "unicode.h"

struct kmscon_font_factory;
struct kmscon_font;

int kmscon_font_factory_new(struct kmscon_font_factory **out);
void kmscon_font_factory_ref(struct kmscon_font_factory *ff);
void kmscon_font_factory_unref(struct kmscon_font_factory *ff);

int kmscon_font_factory_load(struct kmscon_font_factory *ff,
	struct kmscon_font **out, unsigned int width, unsigned int height);

void kmscon_font_ref(struct kmscon_font *font);
void kmscon_font_unref(struct kmscon_font *font);

unsigned int kmscon_font_get_height(struct kmscon_font *font);
unsigned int kmscon_font_get_width(struct kmscon_font *font);
int kmscon_font_draw(struct kmscon_font *font, kmscon_symbol_t ch, float *m,
			struct gl_shader *shader);

#endif /* KMSCON_FONT_H */
