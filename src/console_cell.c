/*
 * kmscon - Console Buffer and Cell Objects
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
 * Console Buffer and Cell Objects
 * A console buffer contains all the characters that are printed to the screen
 * and the whole scrollback buffer. It has a fixed height and width measured in
 * characters which can be changed on the fly.
 * The buffer is a linked list of lines. The tail of the list is the current
 * screen buffer which can be modified by the application. The rest of the list
 * is the scrollback-buffer.
 * The linked-list allows fast rotations but prevents fast access. Therefore,
 * modifications of the scrollback-buffer is prohibited.
 * The current screen position can be any line of the scrollback-buffer.
 *
 * Cells
 * A single cell describes a single character that is printed in that cell. The
 * character itself is a kmscon_char unicode character. The cell also contains
 * the color of the character and some other metadata.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>

#include "console.h"
#include "log.h"

#define DEFAULT_WIDTH 80
#define DEFAULT_HEIGHT 24
#define DEFAULT_SCROLLBACK 128

struct cell {
	struct kmscon_char *ch;
};

struct line {
	struct line *next;
	struct line *prev;

	unsigned int num;
	unsigned int size;
	struct cell *cells;
};

struct kmscon_buffer {
	unsigned long ref;

	unsigned int count;
	struct line *first;
	struct line *last;
	unsigned int max_scrollback;

	unsigned int size_x;
	unsigned int size_y;
	struct line *current;
};

static void destroy_cell(struct cell *cell)
{
	if (!cell)
		return;

	kmscon_char_free(cell->ch);
}

static int init_cell(struct cell *cell)
{
	int ret = 0;

	if (!cell)
		return -EINVAL;

	if (cell->ch)
		kmscon_char_reset(cell->ch);
	else
		ret = kmscon_char_new(&cell->ch);

	return ret;
}

static void free_line(struct line *line)
{
	unsigned int i;

	if (!line)
		return;

	for (i = 0; i < line->size; ++i)
		destroy_cell(&line->cells[i]);

	free(line->cells);
	free(line);
}

static int new_line(struct line **out)
{
	struct line *line;

	if (!out)
		return -EINVAL;

	line = malloc(sizeof(*line));
	if (!line)
		return -ENOMEM;

	memset(line, 0, sizeof(*line));
	*out = line;
	return 0;
}

static int resize_line(struct line *line, unsigned int width)
{
	struct cell *tmp;
	unsigned int i;
	int ret;

	if (!line)
		return -EINVAL;

	if (!width)
		width = DEFAULT_WIDTH;

	if (line->size < width) {
		tmp = realloc(line->cells, width * sizeof(struct cell));
		if (!tmp)
			return -ENOMEM;

		memset(&tmp[line->size], 0,
				(width - line->size) * sizeof(struct cell));
		line->cells = tmp;
		line->size = width;
	}

	for (i = 0; i < width; ++i) {
		ret = init_cell(&line->cells[i]);
		if (ret)
			return ret;
	}

	line->num = width;
	return 0;
}

/*
 * Allocates a new line of width \width and pushes it to the tail of the buffer.
 * The line is filled with blanks. If the maximum number of lines is already
 * reached, the first line is removed and pushed to the tail.
 */
static int push_line(struct kmscon_buffer *buf, unsigned int width)
{
	struct line *line;
	int ret;

	if (!buf)
		return -EINVAL;

	if (buf->count > (buf->size_y + buf->max_scrollback)) {
		line = buf->first;

		buf->first = line->next;
		buf->first->prev = NULL;

		if (buf->current == line)
			buf->current = buf->first;

		line->next = NULL;
		line->prev = NULL;
		--buf->count;
	} else {
		ret = new_line(&line);
		if (ret)
			return ret;
	}

	ret = resize_line(line, width);
	if (ret)
		goto err_free;

	if (buf->last) {
		line->prev = buf->last;
		buf->last->next = line;
		buf->last = line;
	} else {
		buf->first = line;
		buf->last = line;
	}
	++buf->count;

	return 0;

err_free:
	free_line(line);
	return ret;
}

int kmscon_buffer_new(struct kmscon_buffer **out, uint32_t x, uint32_t y)
{
	struct kmscon_buffer *buf;
	int ret;

	if (!out)
		return -EINVAL;

	buf = malloc(sizeof(*buf));
	if (!buf)
		return -ENOMEM;

	memset(buf, 0, sizeof(*buf));
	buf->ref = 1;
	buf->max_scrollback = DEFAULT_SCROLLBACK;

	ret = kmscon_buffer_resize(buf, x, y);
	if (ret)
		goto err_free;

	*out = buf;
	return 0;

err_free:
	free(buf);
	return ret;
}

void kmscon_buffer_ref(struct kmscon_buffer *buf)
{
	if (!buf)
		return;

	++buf->ref;
}

void kmscon_buffer_unref(struct kmscon_buffer *buf)
{
	struct line *iter, *tmp;

	if (!buf || !buf->ref)
		return;

	if (--buf->ref)
		return;

	for (iter = buf->first; iter; ) {
		tmp = iter;
		iter = iter->next;
		free_line(tmp);
	}

	free(buf);
}

/*
 * Resize the current console buffer
 * This adjusts the x/y size of the viewable part of the buffer. It does never
 * modify the scroll-back buffer as this would take too long.
 *
 * y-resize:
 * We simply move the \current position up in the scroll-back buffer so resizing
 * looks like scrolling up in the buffer. If there are no more scroll-back
 * lines, we push empty lines to the bottom so no scrolling appears.
 * If pushing a line fails we simply leave the buffer in the current position so
 * only partial resizing appeared but the buffer is still fully operational.
 *
 * x-resize:
 * We only resize the visible lines to have at least width \x. If resizing fails
 * we leave the buffer in the current state. This may make some lines shorter
 * than others but there is currently no better way to handle memory failures
 * here.
 */
int kmscon_buffer_resize(struct kmscon_buffer *buf, uint32_t x, uint32_t y)
{
	unsigned int i;
	struct line *iter;
	int ret;

	if (!buf)
		return -EINVAL;

	if (!x)
		x = DEFAULT_WIDTH;
	if (!y)
		y = DEFAULT_HEIGHT;

	while (buf->count < y) {
		ret = push_line(buf, x);
		if (ret)
			return ret;
	}

	if (buf->size_x != x) {
		iter = buf->last;
		for (i = 0; i < buf->size_y; ++i) {
			ret = resize_line(iter, x);
			if (ret)
				return ret;
			iter = iter->prev;
		}
	}

	buf->size_x = x;
	buf->size_y = y;
	return 0;
}

void kmscon_buffer_draw(struct kmscon_buffer *buf, struct kmscon_font *font,
			void *dcr, unsigned int width, unsigned int height)
{
	cairo_t *cr;
	double xs, ys, cx, cy;
	unsigned int i, j;
	struct line *iter;
	struct cell *cell;
	struct kmscon_char *ch;

	if (!buf || !font || !dcr)
		return;

	cr = dcr;
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);

	xs = width / (double)buf->size_x;
	ys = height / (double)buf->size_y;

	iter = buf->last;
	cy = (buf->size_y - 1) * ys;
	for (i = 0; i < buf->size_y; ++i) {
		cx = 0;
		for (j = 0; j < iter->num; ++j) {
			cell = &iter->cells[j];
			ch = cell->ch;

			kmscon_font_draw(font, ch, cr, cx, cy);
			cx += xs;
		}
		cy -= ys;
		iter = iter->prev;
	}
}
