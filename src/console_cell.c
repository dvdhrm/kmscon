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
 * For fast access to the current screen buffer, we use an array (cache) of
 * pointers to the first n lines.
 * The current screen position can be any line of the scrollback-buffer.
 *
 * Y-resize simply adjusts the cache to point to the new lines. X-resize only
 * modifies the current screen buffer. The scrollback-buffer is not modified to
 * improve performance.
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

	unsigned int size_x;
	unsigned int size_y;
	unsigned int max_scrollback;
	struct line **cache;
	struct line *current;
};

static void free_line(struct line *line)
{
	unsigned int i;

	if (!line)
		return;

	for (i = 0; i < line->size; ++i) {
		kmscon_char_free(line->cells[i].ch);
	}

	free(line->cells);
	free(line);
}

/*
 * Allocates a new line of width \width and pushes it to the tail of the buffer.
 * The line is filled with blanks. If the maximum number of lines is already
 * reached, the first line is removed and pushed to the tail.
 */
static int push_line(struct kmscon_buffer *buf)
{
	struct line *line;
	struct cell *tmp;
	unsigned int i, width;
	int ret;

	if (!buf)
		return -EINVAL;

	width = buf->size_x;
	if (!width)
		width = DEFAULT_WIDTH;

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
		line = malloc(sizeof(*line));
		if (!line)
			return -ENOMEM;

		memset(line, 0, sizeof(*line));
	}

	if (line->size < width) {
		tmp = realloc(line->cells, width * sizeof(struct cell));
		if (!tmp)
			goto err_free;
		memset(&tmp[line->size], 0,
				(width - line->size) * sizeof(struct cell));
		line->cells = tmp;
		line->size = width;
	}

	line->num = width;
	for (i = 0; i < line->num; ++i) {
		if (line->cells[i].ch) {
			ret = kmscon_char_set_u8(line->cells[i].ch, "?", 1);
		} else {
			ret = kmscon_char_new_u8(&line->cells[i].ch, "?", 1);
		}

		if (ret)
			goto err_free;
	}

	if (buf->last) {
		line->prev = buf->last;
		buf->last->next = line;
		buf->last = line;
	} else {
		buf->first = line;
		buf->last = line;
	}
	++buf->count;

	if (buf->cache) {
		for (i = 0; i < buf->size_y; ++i)
			buf->cache[i] = buf->cache[i]->next;
	}

	return 0;

err_free:
	free_line(line);
	return -ENOMEM;
}

static int resize_line(struct line *line, unsigned int width)
{
	unsigned int i;
	struct cell *tmp;
	int ret;

	if (!line)
		return -EINVAL;

	if (!width)
		width = DEFAULT_WIDTH;

	if (line->size < width) {
		tmp = realloc(line->cells, sizeof(struct cell) * width);
		if (!tmp)
			return -ENOMEM;

		memset(&tmp[line->size], 0,
				(width - line->size) * sizeof(struct cell));
		line->cells = tmp;
		line->size = width;
	}

	for (i = line->num; i < width; ++i) {
		if (!line->cells[i].ch) {
			ret = kmscon_char_new_u8(&line->cells[i].ch, "?", 1);
			if (ret)
				return -ENOMEM;
		}
	}
	line->num = width;

	return 0;
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

	free(buf->cache);
	free(buf);
}

int kmscon_buffer_resize(struct kmscon_buffer *buf, uint32_t x, uint32_t y)
{
	struct line **cache, *iter;
	unsigned int old_x, old_y, i;
	int ret, j;

	if (!buf)
		return -EINVAL;

	if (!x)
		x = DEFAULT_WIDTH;
	if (!y)
		y = DEFAULT_HEIGHT;

	old_x = buf->size_x;
	old_y = buf->size_y;
	buf->size_x = x;
	buf->size_y = y;

	if (old_y != y) {
		while (buf->count < y) {
			ret = push_line(buf);
			if (ret)
				goto err_reset;
		}

		cache = realloc(buf->cache, sizeof(struct line*) * y);
		if (!cache) {
			ret = -ENOMEM;
			goto err_reset;
		}

		memset(cache, 0, sizeof(struct line*) * y);
		iter = buf->last;
		for (j = (y - 1); j >= 0; --j) {
			cache[j] = iter;
			iter = iter->prev;
		}

		buf->cache = cache;
	}

	if (old_x != x) {
		for (i = 0; i < buf->size_y; ++i) {
			ret = resize_line(buf->cache[i], x);
			if (ret)
				goto err_reset;
		}
	}

	return 0;

err_reset:
	buf->size_x = old_x;
	buf->size_y = old_y;
	/* TODO: improve error recovery and correctly reset the buffer */
	return ret;
}
