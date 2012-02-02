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
 * A console buffer maintains an array of the lines of the current screen buffer
 * and a list of lines in the scrollback buffer. The screen buffer can be
 * modified, the scrollback buffer is constant.
 *
 * Current buffer:
 * The current buffer is an array of lines. These lines can be modified by the
 * application and represent the screen. They are also normally displayed on the
 * screen if the user is not looking through the scrollback buffer currently.
 * The buffer may have empty lines. Those are simply NULL pointers. If the
 * buffer is not filled, yet, all lines following the first NULL line will also
 * be NULL.
 * When pushing new lines we simply  find the first NULL line and add a new line
 * there. If no NULL line is available (this is almost always the case) we
 * simply push the first line into the scrollback buffer and move every line one
 * line upwards. The new line will be added at the tail of the array.
 * The current buffer can be modified and accessed by the caller with x/y
 * coordinates. Modifications and access is supposed to be fast. Rotations are
 * kind of slow, though.
 *
 * Scrollback buffer:
 * The scrollback buffer contains all lines that were pushed out of the current
 * screen. It's a linked list of lines which cannot be accessed by the
 * application. It has an upper bound so we do not consume too much memory.
 * If the current buffer is resized to a bigger size, then lines from the
 * scrollback buffer may get back into the current buffer to fill the screen.
 *
 * Lines:
 * A single line is represented by a "struct line". It has an array of cells
 * which can be accessed directly. The length of each line may vary and for
 * faster resizing we also keep a \size member.
 * A line may be shorter than the current buffer width. We do not resize them to
 * speed up the buffer operations. If a line is printed which is longer or
 * shorted than the screen width, it is simply filled with spaces or truncated
 * to screen width.
 * If such a line is accessed outside the bounds, the line is resized to the
 * current screen width to allow access.
 *
 * Screen position:
 * The current screen position may be any line inside the scrollback buffer. If
 * it is NULL, the current position is set to the current screen buffer.
 * If it is non-NULL it will stick to the given line and will not scroll back on
 * new input.
 *
 * Cells
 * A single cell describes a single character that is printed in that cell. The
 * character itself is a kmscon_char unicode character. The cell also contains
 * the color of the character and some other metadata.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "log.h"
#include "unicode.h"

#define DEFAULT_WIDTH 80
#define DEFAULT_HEIGHT 24

struct cell {
	kmscon_symbol_t ch;
};

struct line {
	struct line *next;
	struct line *prev;

	unsigned int size;
	struct cell *cells;
};

struct kmscon_buffer {
	unsigned long ref;

	unsigned int sb_count;
	struct line *sb_first;
	struct line *sb_last;
	unsigned int sb_max;

	struct line *position;

	unsigned int size_x;
	unsigned int size_y;
	unsigned int fill;
	struct line **current;

	struct kmscon_m4_stack *stack;
};

static void destroy_cell(struct cell *cell)
{
	if (!cell)
		return;
}

static int init_cell(struct cell *cell)
{
	if (!cell)
		return -EINVAL;

	memset(cell, 0, sizeof(*cell));
	return 0;
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
	int ret;

	if (!line)
		return -EINVAL;

	if (!width)
		width = DEFAULT_WIDTH;

	if (line->size < width) {
		tmp = realloc(line->cells, width * sizeof(struct cell));
		if (!tmp)
			return -ENOMEM;

		line->cells = tmp;

		while (line->size < width) {
			ret = init_cell(&line->cells[line->size]);
			if (ret)
				return ret;
			line->size++;
		}
	} else if (line->size > width) {
		while (line->size > width) {
			line->size--;
			destroy_cell(&line->cells[line->size]);
		}
	}

	return 0;
}

int kmscon_buffer_new(struct kmscon_buffer **out, unsigned int x,
								unsigned int y)
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

	log_debug("console: new buffer object\n");

	ret = kmscon_m4_stack_new(&buf->stack);
	if (ret)
		goto err_free;

	ret = kmscon_buffer_resize(buf, x, y);
	if (ret)
		goto err_stack;

	*out = buf;
	return 0;

err_stack:
	kmscon_m4_stack_free(buf->stack);
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
	unsigned int i;

	if (!buf || !buf->ref)
		return;

	if (--buf->ref)
		return;

	kmscon_buffer_clear_sb(buf);

	for (i = 0; i < buf->size_y; ++i)
		free_line(buf->current[i]);

	free(buf->current);
	kmscon_m4_stack_free(buf->stack);
	free(buf);
	log_debug("console: destroying buffer object\n");
}

/*
 * This links the given line into the scrollback-buffer. This always succeeds.
 */
static void link_to_scrollback(struct kmscon_buffer *buf, struct line *line)
{
	struct line *tmp;

	if (!buf || !line)
		return;

	if (buf->sb_max == 0) {
		free_line(line);
		return;
	}

	if (buf->sb_count >= buf->sb_max) {
		tmp = buf->sb_first;
		buf->sb_first = tmp->next;
		if (tmp->next)
			tmp->next->prev = NULL;
		else
			buf->sb_last = NULL;
		buf->sb_count--;

		if (buf->position == tmp)
			buf->position = line;
		free_line(tmp);
	}

	line->next = NULL;
	line->prev = buf->sb_last;
	if (buf->sb_last)
		buf->sb_last->next = line;
	else
		buf->sb_first = line;
	buf->sb_last = line;
	buf->sb_count++;
}

/*
 * Unlinks last line from the scrollback buffer. Returns NULL if it is empty.
 */
static struct line *get_from_scrollback(struct kmscon_buffer *buf)
{
	struct line *line;

	if (!buf || !buf->sb_last)
		return NULL;

	line = buf->sb_last;
	buf->sb_last = line->prev;
	if (line->prev)
		line->prev->next = NULL;
	else
		buf->sb_first = NULL;
	buf->sb_count--;

	if (buf->position == line)
		buf->position = NULL;

	line->next = NULL;
	line->prev = NULL;
	return line;
}

/* set maximum scrollback buffer size */
void kmscon_buffer_set_max_sb(struct kmscon_buffer *buf, unsigned int max)
{
	struct line *line;

	if (!buf)
		return;

	while (buf->sb_count > max) {
		line = buf->sb_first;
		if (!line)
			break;

		buf->sb_first = line->next;
		if (line->next)
			line->next->prev = NULL;
		else
			buf->sb_last = NULL;
		buf->sb_count--;

		if (buf->position == line) {
			if (buf->sb_first)
				buf->position = buf->sb_first;
			else
				buf->position = NULL;
		}

		free_line(line);
	}

	buf->sb_max = max;
}

/* clear scrollback buffer */
void kmscon_buffer_clear_sb(struct kmscon_buffer *buf)
{
	struct line *iter, *tmp;

	if (!buf)
		return;

	for (iter = buf->sb_first; iter; ) {
		tmp = iter;
		iter = iter->next;
		free_line(tmp);
	}

	buf->sb_first = NULL;
	buf->sb_last = NULL;
	buf->sb_count = 0;
	buf->position = NULL;
}

/*
 * Resize the current console buffer
 * This resizes the current buffer. We do not resize the lines or modify them in
 * any way. This would take too long if multiple resize-operations are
 * performed.
 */
int kmscon_buffer_resize(struct kmscon_buffer *buf, unsigned int x,
								unsigned int y)
{
	unsigned int i, fill;
	struct line *iter, **cache;

	if (!buf)
		return -EINVAL;

	if (!x)
		x = DEFAULT_WIDTH;
	if (!y)
		y = DEFAULT_HEIGHT;

	if (buf->size_x == x && buf->size_y == y)
		return 0;

	/* Resize y size by adjusting the current buffer size */
	if (y < buf->size_y) {
		/*
		 * Shrink current buffer. First move enough elements from the
		 * current buffer into the scroll-back buffer so we can shrink
		 * it without loosing data.
		 * Then reallocate the buffer (we shrink it so we never fail
		 * here) and correctly set values in \buf. If the buffer has
		 * empty lines, we can shrink it down without moving lines into
		 * the scrollback-buffer so first calculate the current fill of
		 * the buffer and then move appropriate amount of elements to
		 * the scrollback buffer.
		 */

		if (buf->fill > y) {
			for (i = y; i < buf->fill; ++i)
				link_to_scrollback(buf, buf->current[i - y]);

			memmove(buf->current, &buf->current[buf->fill - y],
						y * sizeof(struct line*));
		}

		buf->current = realloc(buf->current, y * sizeof(struct line*));
		buf->size_y = y;
		if (buf->fill > y)
			buf->fill = y;
	} else if (y > buf->size_y) {
		/*
		 * Increase current buffer to new size. Reset all new elements
		 * to NULL so they are empty. Copy existing buffer into new
		 * buffer and correctly set values in \buf.
		 * If we have more space in the buffer, we simply move lines
		 * from the scroll-back buffer into our current buffer if they
		 * are available. Otherwise, we simply add NULL lines.
		 */

		cache = malloc(sizeof(struct line*) * y);
		if (!cache)
			return -ENOMEM;

		memset(cache, 0, sizeof(struct line*) * y);
		fill = y - buf->size_y;

		for (i = 0; i < fill; ++i) {
			iter = get_from_scrollback(buf);
			if (!iter)
				break;

			cache[y - i - 1] = iter;
		}
		buf->fill += i;
		memmove(cache, &cache[y - i], i * sizeof(struct line*));

		if (buf->size_y) {
			memcpy(&cache[i], buf->current,
					sizeof(struct line*) * buf->size_y);
			free(buf->current);
		}

		buf->current = cache;
		buf->size_y = y;
	}

	/* Adjust x size by simply setting the new value */
	buf->size_x = x;

	log_debug("console: resize buffer to %ux%u\n", x, y);

	return 0;
}

void kmscon_buffer_draw(struct kmscon_buffer *buf, struct kmscon_font *font)
{
	float xs, ys;
	unsigned int i, j, k, num;
	struct line *iter, *line;
	struct cell *cell;
	float *m;

	if (!buf || !font)
		return;

	m = kmscon_m4_stack_tip(buf->stack);
	kmscon_m4_identity(m);

	xs = 1.0 / buf->size_x;
	ys = 1.0 / buf->size_y;
	kmscon_m4_scale(m, 2, 2, 1);
	kmscon_m4_trans(m, -0.5, -0.5, 0);
	kmscon_m4_scale(m, xs, ys, 1);

	iter = buf->position;
	k = 0;

	for (i = 0; i < buf->size_y; ++i) {
		if (iter) {
			line = iter;
			iter = iter->next;
		} else {
			line = buf->current[k];
			k++;
		}

		if (!line)
			break;

		if (line->size < buf->size_x)
			num = line->size;
		else
			num = buf->size_x;

		for (j = 0; j < num; ++j) {
			cell = &line->cells[j];

			m = kmscon_m4_stack_push(buf->stack);
			if (!m) {
				log_warn("console: cannot push matrix\n");
				break;
			}

			kmscon_m4_trans(m, j, i, 0);
			kmscon_font_draw(font, cell->ch, m);
			m = kmscon_m4_stack_pop(buf->stack);
		}
	}
}

unsigned int kmscon_buffer_get_width(struct kmscon_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->size_x;
}

unsigned int kmscon_buffer_get_height(struct kmscon_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->size_y;
}

void kmscon_buffer_write(struct kmscon_buffer *buf, unsigned int x,
				unsigned int y, kmscon_symbol_t ch)
{
	struct line *line;
	int ret;

	if (!buf)
		return;

	if (x >= buf->size_x || y >= buf->size_y) {
		log_warn("console: writing beyond buffer boundary\n");
		return;
	}

	if (!buf->current[y]) {
		while (buf->fill <= y) {
			ret = new_line(&line);
			if (ret) {
				log_warn("console: cannot allocate line "
						"(%d); dropping input\n", ret);
				return;
			}
			buf->current[buf->fill] = line;
			buf->fill++;
		}
	}
	line = buf->current[y];

	if (x >= line->size) {
		ret = resize_line(line, buf->size_x);
		if (ret) {
			log_warn("console: cannot resize line (%d); "
						"dropping input\n", ret);
			return;
		}
	}

	line->cells[x].ch = ch;
}

kmscon_symbol_t kmscon_buffer_read(struct kmscon_buffer *buf, unsigned int x,
								unsigned int y)
{
	struct line *line;

	if (!buf)
		return kmscon_symbol_default;

	if (x >= buf->size_x || y >= buf->size_y) {
		log_warn("console: reading out of buffer bounds\n");
		return kmscon_symbol_default;
	}

	line = buf->current[y];
	if (!line)
		return kmscon_symbol_default;

	if (x >= line->size)
		return kmscon_symbol_default;

	return line->cells[x].ch;
}

void kmscon_buffer_newline(struct kmscon_buffer *buf)
{
	struct line *line, *nl;
	int ret;

	if (!buf)
		return;

	ret = new_line(&nl);
	if (ret) {
		log_warn("console: cannot allocate line (%d); "
						"dropping input\n", ret);
		return;
	}

	if (buf->fill >= buf->size_y) {
		line = buf->current[0];
		if (!line)
			return;

		link_to_scrollback(buf, line);
		memmove(buf->current, &buf->current[1],
				(buf->size_y - 1) * sizeof(struct line*));
		buf->current[buf->size_y - 1] = NULL;
		buf->fill--;
	}

	buf->current[buf->fill] = nl;
	buf->fill++;
}
