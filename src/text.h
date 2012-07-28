/*
 * kmscon - Text Renderer
 *
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
 * Text Renderer
 * The Text-Renderer subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 * The system is split into:
 *  - Font renderer: The font renderer allows selecting fonts and rendering
 *    single glyphs into memory-buffers
 *  - Text renderer: The text renderer uses the font renderer to draw a whole
 *    console into a framebuffer.
 */

#ifndef KMSCON_TEXT_H
#define KMSCON_TEXT_H

#include <stdlib.h>
#include "unicode.h"
#include "uterm.h"

/* fonts */

struct kmscon_font_attr;
struct kmscon_glyph;
struct kmscon_font;
struct kmscon_font_ops;

#define KMSCON_FONT_MAX_NAME 128
#define KMSCON_FONT_DEFAULT_NAME "monospace"
#define KMSCON_FONT_DEFAULT_PPI 72

struct kmscon_font_attr {
	char name[KMSCON_FONT_MAX_NAME];
	unsigned int ppi;
	unsigned int points;
	bool bold;
	bool italic;
	unsigned int height;
	unsigned int width;
};

void kmscon_font_attr_normalize(struct kmscon_font_attr *attr);
bool kmscon_font_attr_match(const struct kmscon_font_attr *a1,
			    const struct kmscon_font_attr *a2);

struct kmscon_glyph {
	unsigned int ascent;
	unsigned int descent;
	struct uterm_video_buffer buf;
};

struct kmscon_font {
	unsigned long ref;
	const struct kmscon_font_ops *ops;
	struct kmscon_font_attr attr;
	void *data;
};

struct kmscon_font_ops {
	const char *name;
	int (*init) (struct kmscon_font *out,
		     const struct kmscon_font_attr *attr);
	void (*destroy) (struct kmscon_font *font);
	int (*render) (struct kmscon_font *font, kmscon_symbol_t sym,
		       const struct kmscon_glyph **out);
	void (*drop) (struct kmscon_font *font,
		      const struct kmscon_glyph *glyph);
};

int kmscon_font_register(const struct kmscon_font_ops *ops);
void kmscon_font_unregister(const char *name);

int kmscon_font_find(struct kmscon_font **out,
		     const struct kmscon_font_attr *attr,
		     const char *backend);
void kmscon_font_ref(struct kmscon_font *font);
void kmscon_font_unref(struct kmscon_font *font);

int kmscon_font_render(struct kmscon_font *font, kmscon_symbol_t sym,
		       const struct kmscon_glyph **out);
void kmscon_font_drop(struct kmscon_font *font,
		      const struct kmscon_glyph *glyph);

#endif /* KMSCON_TEXT_H */
