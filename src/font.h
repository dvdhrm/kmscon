/*
 * kmscon - Font Management
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * The output of a console is a fixed-size table. That is, it consists of many
 * cells where each character is printed either into a single cell or spread
 * across multiple cells. However, there will never be multiple characters in a
 * single cell so cell indexes are the smallest position information.
 * The classic console uses one character per cell. Newer consoles may allow
 * widened characters, though. Common are characters that are double-width and
 * characters that are double-width+double-height.
 * If you mix many different widths/heights then this might get very
 * memory-consuming as we need to have one loaded font for each size to get
 * decent results. Therefore, avoid widths/heights other than the ones
 * mentioned.
 *
 * Therefore, this layer does not provide the classic font APIs, instead it
 * offers a font_screen object which represents the whole screen. You specify
 * the x/y coordinates of your framebuffer/target and the font plus point-size
 * that you want to use. This layer automatically computes the pixel size and
 * resulting row/column counts.
 * For reversed logic you can also specify row/column counts and the API
 * calculates the required font-point-size.
 * In both situations you never have to deal with font related details! The only
 * thing you need to know is the row/column count of the resulting table and
 * all the characters in the table.
 *
 * When drawing a screen you need to tell the font layer where to draw the
 * characters. For performance reasons this is split into several tasks:
 *   1: Start a drawing operation. This resets the screen and prepares the font
 *      for drawing. It clears all previous entries.
 *   2: Add each character you want to draw to the font_screen object with its
 *      cell position and cell width. The width is probably always 1/1 but for
 *      multi-cell characters you can specify other widths/heights.
 *   3: Perform the drawing operation. This instructs the font-layer to actually
 *      draw all the added characters to your surface.
 * You need to perform all 3 steps for every frame you render.
 */

#ifndef FONT_FONT_H
#define FONT_FONT_H

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

/* font attributes */

enum font_style {
	FONT_NORMAL,
	FONT_ITALIC,
};

struct font_attr {
	const char *name;	/* use NULL for default */
	unsigned int points;
	unsigned int dpi;	/* use 0 for default */
	bool bold;
	enum font_style style;
};

#define FONT_ATTR(_name, _points, _dpi) &(const struct font_attr){ \
		.name = (_name), \
		.points = (_points), \
		.dpi = (_dpi), \
		.bold = false, \
		.style = FONT_NORMAL, \
	}

struct font_char_attr {
	uint8_t fr;			/* foreground red */
	uint8_t fg;			/* foreground green */
	uint8_t fb;			/* foreground blue */
	uint8_t br;			/* background red */
	uint8_t bg;			/* background green */
	uint8_t bb;			/* background blue */
	unsigned int bold : 1;		/* bold character */
	unsigned int underline : 1;	/* underlined character */
	unsigned int inverse : 1;	/* inverse colors */
};

/* font draw/assemble buffers */

struct font_buffer {
	unsigned int width;
	unsigned int stride;
	unsigned int height;
	char *data;
};

int font_buffer_new(struct font_buffer **out, unsigned int width,
			unsigned int height);
void font_buffer_free(struct font_buffer *buf);

/* font screens */

struct font_screen;

int font_screen_new(struct font_screen **out, struct font_buffer *buf,
			const struct font_attr *attr,
			struct gl_shader *shader);
int font_screen_new_fixed(struct font_screen **out, struct font_buffer *buf,
			const struct font_attr *attr,
			unsigned int cols, unsigned int rows,
			struct gl_shader *shader);
void font_screen_free(struct font_screen *screen);

unsigned int font_screen_columns(struct font_screen *screen);
unsigned int font_screen_rows(struct font_screen *screen);
unsigned int font_screen_points(struct font_screen *screen);
unsigned int font_screen_width(struct font_screen *screen);
unsigned int font_screen_height(struct font_screen *screen);

int font_screen_draw_start(struct font_screen *screen);
int font_screen_draw_char(struct font_screen *screen, kmscon_symbol_t ch,
				const struct font_char_attr *attr,
				unsigned int cellx, unsigned int celly,
				unsigned int width, unsigned int height);
int font_screen_draw_perform(struct font_screen *screen, float *m);

#endif /* FONT_FONT_H */
