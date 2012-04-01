/*
 * kmscon - Console Management
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
 * Console Management
 * This provides the console drawing and manipulation functions. It does not
 * provide the terminal emulation. It is just an abstraction layer to draw text
 * to a framebuffer as used by terminals and consoles.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "console.h"
#include "font.h"
#include "gl.h"
#include "log.h"
#include "unicode.h"

#define LOG_SUBSYSTEM "console"

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

	unsigned int scroll_y;
	unsigned int scroll_fill;
	struct line **scroll_buf;

	unsigned int mtop_y;
	struct line **mtop_buf;
	unsigned int mbottom_y;
	struct line **mbottom_buf;

	struct gl_m4_stack *stack;
};

struct kmscon_console {
	size_t ref;
	struct kmscon_font_factory *ff;

	/* font */
	unsigned int res_x;
	unsigned int res_y;
	struct kmscon_font *font;

	/* console cells */
	struct kmscon_buffer *cells;
	unsigned int cells_x;
	unsigned int cells_y;
	unsigned int margin_top;	/* idx of first scroll line */
	unsigned int margin_bottom;	/* idx of last scroll line */
	bool rel_addr;			/* is relative addressing used? */
	bool auto_wrap;			/* auto wrap on end of line? */

	/* cursor */
	unsigned int cursor_x;
	unsigned int cursor_y;
};

/*
 * Console Buffer and Cell Objects
 * A console buffer maintains an array of the lines of the current screen buffer
 * and a list of lines in the scrollback buffer. The screen buffer can be
 * modified, the scrollback buffer is constant.
 *
 * Buffers:
 * The buffer object consists of three buffers. The top margin, the bottom
 * margin and the scroll buffer. The margins are non-existent by default. The
 * scroll buffer is the main buffer. If the terminal is rotated then the lines
 * of this buffer go to the scroll-back buffer and vice versa.
 * The margin buffers are static. They can be modified but they are never
 * rotated. If the margins are created, the lines are copied from the scroll
 * buffer and then removed from the scroll buffer. If the margins are removed,
 * the lines are linked back into the scroll buffer at the current position.
 * Each buffer is an array of lines. The lines can be modified very fast and
 * rotations do not require heavy memory-moves. If a line is NULL we consider
 * this line empty so we can resize the buffer without reallocating new lines.
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

static void reset_cell(struct cell *cell)
{
	if (!cell)
		return;

	memset(cell, 0, sizeof(*cell));
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

static struct line *get_line(struct kmscon_buffer *buf, unsigned int y)
{
	if (y < buf->mtop_y) {
		return buf->mtop_buf[y];
	} else if (y < buf->mtop_y + buf->scroll_y) {
		y -= buf->mtop_y;
		return buf->scroll_buf[y];
	} else if (y < buf->mtop_y + buf->scroll_y + buf->mbottom_y) {
		y = y - buf->mtop_y - buf->scroll_y;
		return buf->mbottom_buf[y];
	}

	return NULL;
}

/*
 * This links the given line into the scrollback-buffer. This always succeeds.
 */
static void link_to_scrollback(struct kmscon_buffer *buf, struct line *line)
{
	struct line *tmp;
	int ret;

	if (!buf)
		return;

	if (buf->sb_max == 0) {
		free_line(line);
		return;
	}

	if (!line) {
		ret = new_line(&line);
		if (ret) {
			log_warn("cannot allocate line (%d); dropping scrollback-buffer line", ret);
			return;
		}
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

/*
 * Resize scroll buffer. Despite being used for scroll region only, it is kept
 * big enough to hold both margins too. We do this to allow fast merges of
 * margins and scroll buffer.
 */
static int resize_scrollbuf(struct kmscon_buffer *buf, unsigned int y)
{
	unsigned int fill, i, siz;
	struct line *iter, **cache;

	/* Resize y size by adjusting the scroll-buffer size */
	if (y < buf->scroll_y) {
		/*
		 * Shrink scroll-buffer. First move enough elements from the
		 * scroll-buffer into the scroll-back buffer so we can shrink
		 * it without loosing data.
		 * Then reallocate the buffer (we shrink it so we never fail
		 * here) and correctly set values in \buf. If the buffer has
		 * unused lines, we can shrink it down without moving lines into
		 * the scrollback-buffer so first calculate the current fill of
		 * the buffer and then move appropriate amount of elements to
		 * the scrollback buffer.
		 */

		if (buf->scroll_fill > y) {
			for (i = y; i < buf->scroll_fill; ++i)
				link_to_scrollback(buf, buf->scroll_buf[i - y]);

			memmove(buf->scroll_buf,
				&buf->scroll_buf[buf->scroll_fill - y],
				y * sizeof(struct line*));
		}

		siz = buf->mtop_y + buf->mbottom_y + y;
		buf->scroll_buf = realloc(buf->scroll_buf,
						siz * sizeof(struct line*));
		buf->scroll_y = y;
		if (buf->scroll_fill > y)
			buf->scroll_fill = y;
	} else if (y > buf->scroll_y) {
		/*
		 * Increase scroll-buffer to new size. Reset all new elements
		 * to NULL so they are empty. Copy existing buffer into new
		 * buffer and correctly set values in \buf.
		 * If we have more space in the buffer, we simply move lines
		 * from the scroll-back buffer into our scroll-buffer if they
		 * are available. Otherwise, we simply add NULL lines.
		 */

		siz = buf->mtop_y + buf->mbottom_y + y;
		cache = malloc(sizeof(struct line*) * siz);
		if (!cache)
			return -ENOMEM;

		memset(cache, 0, sizeof(struct line*) * siz);
		fill = y - buf->scroll_y;

		for (i = 0; i < fill; ++i) {
			iter = get_from_scrollback(buf);
			if (!iter)
				break;

			cache[y - i - 1] = iter;
		}
		buf->scroll_fill += i;
		memmove(cache, &cache[y - i], i * sizeof(struct line*));
		memset(&cache[i + buf->scroll_y], 0,
					(fill - i) * sizeof(struct line*));

		if (buf->scroll_y)
			memcpy(&cache[i], buf->scroll_buf,
					sizeof(struct line*) * buf->scroll_y);

		free(buf->scroll_buf);
		buf->scroll_buf = cache;
		buf->scroll_y = y;
	}

	return 0;
}

static int resize_mtop(struct kmscon_buffer *buf, unsigned int y)
{
	unsigned int mv;
	struct line **cache;

	if (y == buf->mtop_y)
		return 0;

	if (y < buf->mtop_y) {
		mv = buf->mtop_y - y;
		memmove(&buf->scroll_buf[mv], buf->scroll_buf,
					buf->scroll_y * sizeof(struct line*));
		memcpy(buf->scroll_buf, &buf->mtop_buf[y],
						mv * sizeof(struct line*));
		buf->scroll_fill += mv;
		buf->scroll_y += mv;
		buf->mtop_y -= mv;
	} else {
		mv = y - buf->mtop_y;
		if (mv >= buf->scroll_y) {
			log_debug("setting margin size above buffer size; trimming margin");
			if (buf->scroll_y <= 1)
				return 0;
			mv = buf->scroll_y - 1;
			y = buf->mtop_y + mv;
		}

		cache = malloc(y * sizeof(struct line*));
		if (!cache)
			return -ENOMEM;

		memcpy(cache, buf->mtop_buf,
					buf->mtop_y * sizeof(struct line*));
		memcpy(&cache[buf->mtop_y], buf->scroll_buf,
						mv * sizeof(struct line*));
		memmove(buf->scroll_buf, &buf->scroll_buf[mv],
				(buf->scroll_y - mv) * sizeof(struct line*));
		if (buf->scroll_fill > mv)
			buf->scroll_fill -= mv;
		else
			buf->scroll_fill = 0;
		buf->scroll_y -= mv;
		buf->mtop_y += mv;
		free(buf->mtop_buf);
		buf->mtop_buf = cache;
	}

	return 0;
}

static int resize_mbottom(struct kmscon_buffer *buf, unsigned int y)
{
	unsigned int mv;
	struct line **cache;

	if (y == buf->mbottom_y)
		return 0;

	if (y < buf->mbottom_y) {
		mv = buf->mbottom_y - y;
		memcpy(&buf->scroll_buf[buf->scroll_y], buf->mbottom_buf,
						mv * sizeof(struct line*));
		memmove(buf->mbottom_buf, &buf->mbottom_buf[mv],
				(buf->mbottom_y - mv) * sizeof(struct line*));
		buf->scroll_y += mv;
		buf->scroll_fill = buf->scroll_y;
		buf->mbottom_y -= mv;
	} else {
		mv = y - buf->mbottom_y;
		if (mv >= buf->scroll_y) {
			log_debug("setting margin size above buffer size; trimming margin");
			if (buf->scroll_y <= 1)
				return 0;
			mv = buf->scroll_y - 1;
			y = buf->mbottom_y + mv;
		}

		cache = malloc(y * sizeof(struct line*));
		if (!cache)
			return -ENOMEM;

		memcpy(&cache[mv], buf->mbottom_buf,
					buf->mbottom_y * sizeof(struct line*));
		memcpy(cache, &buf->scroll_buf[buf->scroll_y - mv],
						mv * sizeof(struct line*));
		buf->scroll_y -= mv;
		if (buf->scroll_fill > buf->scroll_y)
			buf->scroll_fill = buf->scroll_y;
		buf->mbottom_y += mv;
		free(buf->mbottom_buf);
		buf->mbottom_buf = cache;
	}

	return 0;
}

/*
 * Resize the current console buffer
 * This resizes the current buffer. We do not resize the lines or modify them in
 * any way. This would take too long if multiple resize-operations are
 * performed.
 */
static int kmscon_buffer_resize(struct kmscon_buffer *buf, unsigned int x,
								unsigned int y)
{
	int ret;
	unsigned int margin;

	if (!buf)
		return -EINVAL;

	if (!x)
		x = DEFAULT_WIDTH;
	if (!y)
		y = DEFAULT_HEIGHT;

	if (buf->size_x == x && buf->size_y == y)
		return 0;

	margin = buf->mtop_y + buf->mbottom_y;
	if (y <= margin) {
		log_debug("reducing buffer size below margin size; destroying margins");
		resize_mtop(buf, 0);
		resize_mbottom(buf, 0);
	}

	ret = resize_scrollbuf(buf, buf->scroll_y + (y - (int)buf->size_y));
	if (ret)
		return ret;
	buf->size_y = y;

	/* Adjust x size by simply setting the new value */
	buf->size_x = x;

	log_debug("resize buffer to %ux%u", x, y);

	return 0;
}

/* set maximum scrollback buffer size */
static void kmscon_buffer_set_max_sb(struct kmscon_buffer *buf,
					unsigned int max)
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
static void kmscon_buffer_clear_sb(struct kmscon_buffer *buf)
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

static int kmscon_buffer_new(struct kmscon_buffer **out, unsigned int x,
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

	ret = gl_m4_stack_new(&buf->stack);
	if (ret)
		goto err_free;

	ret = kmscon_buffer_resize(buf, x, y);
	if (ret)
		goto err_stack;

	log_debug("new buffer object");
	*out = buf;
	return 0;

err_stack:
	gl_m4_stack_free(buf->stack);
err_free:
	free(buf);
	return ret;
}

static void kmscon_buffer_ref(struct kmscon_buffer *buf)
{
	if (!buf)
		return;

	++buf->ref;
}

static void kmscon_buffer_unref(struct kmscon_buffer *buf)
{
	unsigned int i;

	if (!buf || !buf->ref)
		return;

	if (--buf->ref)
		return;

	log_debug("destroying buffer object");
	kmscon_buffer_clear_sb(buf);

	for (i = 0; i < buf->scroll_y; ++i)
		free_line(buf->scroll_buf[i]);
	for (i = 0; i < buf->mtop_y; ++i)
		free_line(buf->mtop_buf[i]);
	for (i = 0; i < buf->mbottom_y; ++i)
		free_line(buf->mbottom_buf[i]);

	free(buf->scroll_buf);
	free(buf->mtop_buf);
	free(buf->mbottom_buf);
	gl_m4_stack_free(buf->stack);
	free(buf);
}

static int kmscon_buffer_set_margins(struct kmscon_buffer *buf,
					unsigned int top,
					unsigned int bottom)
{
	int ret;

	if (!buf)
		return -EINVAL;

	if (top < buf->mtop_y) {
		ret = resize_mtop(buf, top);
		if (ret)
			return ret;
		return resize_mbottom(buf, bottom);
	} else {
		ret = resize_mbottom(buf, bottom);
		if (ret)
			return ret;
		return resize_mtop(buf, top);
	}
}

static unsigned int kmscon_buffer_get_mtop(struct kmscon_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->mtop_y;
}

static unsigned int kmscon_buffer_get_mbottom(struct kmscon_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->mbottom_y;
}

static void kmscon_buffer_draw(struct kmscon_buffer *buf,
				struct kmscon_font *font,
				struct gl_shader *shader)
{
	float xs, ys;
	unsigned int i, j, k, num;
	struct line *iter, *line = NULL;
	struct cell *cell;
	float *m;
	int idx;

	if (!buf || !font)
		return;

	m = gl_m4_stack_tip(buf->stack);
	gl_m4_identity(m);

	xs = 1.0 / buf->size_x;
	ys = 1.0 / buf->size_y;
	gl_m4_scale(m, 2, 2, 1);
	gl_m4_translate(m, -0.5, -0.5, 0);
	gl_m4_scale(m, xs, ys, 1);

	iter = buf->position;
	k = 0;
	idx = 0;

	for (i = 0; i < buf->size_y; ++i) {
		if (iter) {
			line = iter;
			iter = iter->next;
		} else {
			if (idx == 0) {
				if (k < buf->mtop_y) {
					line = buf->mtop_buf[k];
				} else {
					k = 0;
					idx = 1;
				}
			}
			if (idx == 1) {
				if (k < buf->scroll_y) {
					line = buf->scroll_buf[k];
				} else {
					k = 0;
					idx = 2;
				}
			}
			if (idx == 2) {
				if (k < buf->mbottom_y)
					line = buf->mbottom_buf[k];
				else
					break;
			}
			k++;
		}

		if (!line)
			continue;

		if (line->size < buf->size_x)
			num = line->size;
		else
			num = buf->size_x;

		for (j = 0; j < num; ++j) {
			cell = &line->cells[j];

			m = gl_m4_stack_push(buf->stack);
			if (!m) {
				log_warn("cannot push matrix");
				break;
			}

			gl_m4_translate(m, j, i, 0);
			kmscon_font_draw(font, cell->ch, m, shader);
			m = gl_m4_stack_pop(buf->stack);
		}
	}
}

static unsigned int kmscon_buffer_get_width(struct kmscon_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->size_x;
}

static unsigned int kmscon_buffer_get_height(struct kmscon_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->size_y;
}

static void kmscon_buffer_write(struct kmscon_buffer *buf, unsigned int x,
				unsigned int y, kmscon_symbol_t ch)
{
	struct line *line, **slot;
	int ret;
	bool scroll = false;

	if (!buf)
		return;

	if (x >= buf->size_x || y >= buf->size_y) {
		log_warn("writing beyond buffer boundary");
		return;
	}

	if (y < buf->mtop_y) {
		slot = &buf->mtop_buf[y];
	} else if (y < buf->mtop_y + buf->scroll_y) {
		y -= buf->mtop_y;
		slot = &buf->scroll_buf[y];
		scroll = true;
	} else if (y < buf->mtop_y + buf->scroll_y + buf->mbottom_y) {
		y = y - buf->mtop_y - buf->scroll_y;
		slot = &buf->mbottom_buf[y];
	} else {
		log_warn("writing to invalid buffer space");
		return;
	}

	line = *slot;
	if (!line) {
		ret = new_line(&line);
		if (ret) {
			log_warn("cannot allocate line (%d); dropping input", ret);
			return;
		}

		*slot = line;
		if (scroll && buf->scroll_fill <= y)
			buf->scroll_fill = y + 1;
	}

	if (x >= line->size) {
		ret = resize_line(line, buf->size_x);
		if (ret) {
			log_warn("cannot resize line (%d); dropping input", ret);
			return;
		}
	}

	line->cells[x].ch = ch;
}

static kmscon_symbol_t kmscon_buffer_read(struct kmscon_buffer *buf,
						unsigned int x,
						unsigned int y)
{
	struct line *line;

	if (!buf)
		return kmscon_symbol_default;

	if (x >= buf->size_x || y >= buf->size_y) {
		log_warn("reading out of buffer bounds");
		return kmscon_symbol_default;
	}

	if (y < buf->mtop_y) {
		line = buf->mtop_buf[y];
	} else if (y < buf->mtop_y + buf->scroll_y) {
		y -= buf->mtop_y;
		line = buf->scroll_buf[y];
	} else if (y < buf->mtop_y + buf->scroll_y + buf->mbottom_y) {
		y = y - buf->mtop_y - buf->scroll_y;
		line = buf->mbottom_buf[y];
	} else {
		log_warn("reading from invalid buffer space");
		return kmscon_symbol_default;
	}

	if (!line)
		return kmscon_symbol_default;

	if (x >= line->size)
		return kmscon_symbol_default;

	return line->cells[x].ch;
}

static void kmscon_buffer_scroll_down(struct kmscon_buffer *buf,
					unsigned int num)
{
	unsigned int i;

	if (!buf || !num)
		return;

	if (num > buf->scroll_y)
		num = buf->scroll_y;

	for (i = 0; i < num; ++i)
		free_line(buf->scroll_buf[buf->scroll_y - i - 1]);

	memmove(&buf->scroll_buf[num], buf->scroll_buf,
			(buf->scroll_y - num) * sizeof(struct line*));
	memset(buf->scroll_buf, 0, num * sizeof(struct line*));
	buf->scroll_fill = buf->scroll_y;
}

static void kmscon_buffer_scroll_up(struct kmscon_buffer *buf,
					unsigned int num)
{
	unsigned int i;

	if (!buf || !num)
		return;

	if (num > buf->scroll_y)
		num = buf->scroll_y;

	for (i = 0; i < num; ++i)
		link_to_scrollback(buf, buf->scroll_buf[i]);

	memmove(buf->scroll_buf, &buf->scroll_buf[num],
				(buf->scroll_y - num) * sizeof(struct line*));
	memset(&buf->scroll_buf[buf->scroll_y - num], 0,
						num * sizeof(struct line*));
	buf->scroll_fill = buf->scroll_y;
}

static void kmscon_buffer_erase_region(struct kmscon_buffer *buf,
					unsigned int x_from,
					unsigned int y_from,
					unsigned int x_to,
					unsigned int y_to)
{
	unsigned int to;
	struct line *line;

	if (!buf)
		return;

	if (y_to >= buf->size_y)
		y_to = buf->size_y - 1;
	if (x_to >= buf->size_x)
		x_to = buf->size_x - 1;

	for ( ; y_from <= y_to; ++y_from) {
		line = get_line(buf, y_from);
		if (!line) {
			x_from = 0;
			continue;
		}

		if (y_from == y_to)
			to = x_to;
		else
			to = buf->size_x - 1;
		for ( ; x_from <= to; ++x_from) {
			if (x_from >= line->size)
				break;
			reset_cell(&line->cells[x_from]);
		}
		x_from = 0;
	}
}

static inline unsigned int to_abs_x(struct kmscon_console *con, unsigned int x)
{
	return x;
}

static inline unsigned int to_abs_y(struct kmscon_console *con, unsigned int y)
{
	if (!con->rel_addr)
		return y;

	return con->margin_top + y;
}

int kmscon_console_new(struct kmscon_console **out,
			struct kmscon_font_factory *ff)
{
	struct kmscon_console *con;
	int ret;
	unsigned int num;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	con->ref = 1;
	con->auto_wrap = true;
	con->ff = ff;

	ret = kmscon_buffer_new(&con->cells, 0, 0);
	if (ret)
		goto err_free;

	con->cells_x = kmscon_buffer_get_width(con->cells);
	con->cells_y = kmscon_buffer_get_height(con->cells);
	con->margin_top = kmscon_buffer_get_mtop(con->cells);
	num = kmscon_buffer_get_mbottom(con->cells);
	con->margin_bottom = con->cells_y - 1 - num;

	log_debug("new console");
	kmscon_font_factory_ref(con->ff);
	*out = con;

	return 0;

err_free:
	free(con);
	return ret;
}

void kmscon_console_ref(struct kmscon_console *con)
{
	if (!con)
		return;

	++con->ref;
}

/*
 * Drops one reference. If this is the last reference, the whole console is
 * freed and the associated render-images are destroyed.
 */
void kmscon_console_unref(struct kmscon_console *con)
{
	if (!con || !con->ref)
		return;

	if (--con->ref)
		return;

	log_debug("destroying console");
	kmscon_font_unref(con->font);
	kmscon_buffer_unref(con->cells);
	kmscon_font_factory_unref(con->ff);
	free(con);
}

unsigned int kmscon_console_get_width(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->cells_x;
}

unsigned int kmscon_console_get_height(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->cells_y;
}

/*
 * Resize console to \x and \y. The \height argument is just a quality hint for
 * internal rendering. It is supposed to be the maximal height in pixels of your
 * output. The internal texture will have this height (the width is computed
 * automatically from the font and height). You can still use *_map() to map
 * this texture to arbitrary outputs but if you have huge resolutions, this
 * would result in bad quality if you do not specify a proper height here.
 *
 * You need to have an active GL context when calling this. You must call this
 * before calling *_draw(). Otherwise *_draw() will not work.
 * Pass 0 for each parameter if you want to use the current value. Therefore:
 * kmscon_console_resize(con, 0, 0, 0) has no effect as it doesn't change
 * anything.
 * If you called this once you must make sure that the GL context stays alive
 * for as long as this console object does. Otherwise, on deinitialization we
 * may call invalid OpenGL functions.
 * TODO: Use proper dependencies here. Maybe pass in a kmscon_output or similar
 * so we correctly activate GL contexts.
 */
int kmscon_console_resize(struct kmscon_console *con, unsigned int x,
					unsigned int y, unsigned int height)
{
	int ret;
	unsigned int num;
	struct kmscon_font *font;

	if (!con)
		return -EINVAL;

	if (!x)
		x = con->cells_x;
	if (!y)
		y = con->cells_y;
	if (!height)
		height = con->res_y;

	if (x == con->cells_x && y == con->cells_y && height == con->res_y)
		return 0;

	log_debug("resizing to %ux%u:%u", x, y, height);

	ret = kmscon_buffer_resize(con->cells, x, y);
	if (ret)
		return ret;

	con->cells_x = kmscon_buffer_get_width(con->cells);
	con->cells_y = kmscon_buffer_get_height(con->cells);
	con->margin_top = kmscon_buffer_get_mtop(con->cells);
	num = kmscon_buffer_get_mbottom(con->cells);
	con->margin_bottom = con->cells_y - 1 - num;

	kmscon_console_move_to(con, con->cursor_x, con->cursor_y);

	ret = kmscon_font_factory_load(con->ff, &font, 0,
							height / con->cells_y);
	if (ret) {
		log_err("cannot create new font: %d", ret);
		return ret;
	}

	kmscon_font_unref(con->font);
	con->font = font;
	con->res_x = con->cells_x * kmscon_font_get_width(con->font);
	con->res_y = height;
	log_debug("new resolution %ux%u", con->res_x, con->res_y);

	return 0;
}

/*
 * This maps the console onto the current GL framebuffer. It expects the
 * framebuffer to have 0/0 in the middle, -1/-1 in the upper left and 1/1 in the
 * lower right (default GL settings).
 * This does not clear the screen, nor does it paint the background. Instead the
 * background is transparent and blended on top of the framebuffer.
 *
 * You must have called kmscon_console_draw() before, otherwise this will map an
 * empty image onto the screen.
 */
void kmscon_console_map(struct kmscon_console *con, struct gl_shader *shader)
{
	if (!con)
		return;

	kmscon_buffer_draw(con->cells, con->font, shader);
}

void kmscon_console_write(struct kmscon_console *con, kmscon_symbol_t ch)
{
	if (!con)
		return;

	if (con->cursor_x >= con->cells_x) {
		if (con->auto_wrap) {
			con->cursor_x = 0;
			con->cursor_y++;
			if (con->cursor_y > con->margin_bottom) {
				con->cursor_y--;
				kmscon_buffer_scroll_up(con->cells, 1);
			}
		} else {
			con->cursor_x = con->cells_x - 1;
		}
	}

	kmscon_buffer_write(con->cells, con->cursor_x, con->cursor_y, ch);
	con->cursor_x++;
}

void kmscon_console_newline(struct kmscon_console *con)
{
	if (!con)
		return;

	con->cursor_x = 0;
	con->cursor_y++;
	if (con->cursor_y > con->margin_bottom) {
		con->cursor_y--;
		kmscon_buffer_scroll_up(con->cells, 1);
	}
}

void kmscon_console_backspace(struct kmscon_console *con)
{
	if (!con)
		return;

	if (con->cursor_x >= con->cells_x) {
		con->cursor_x = con->cells_x - 2;
	} else if (con->cursor_x > 0) {
		con->cursor_x--;
	} else if (con->auto_wrap) {
		con->cursor_x = con->cells_x - 1;
		kmscon_console_move_up(con, 1, true);
	}
}

void kmscon_console_move_to(struct kmscon_console *con, unsigned int x,
							unsigned int y)
{
	if (!con)
		return;

	con->cursor_x = to_abs_x(con, x);
	if (con->cursor_x >= con->cells_x)
		con->cursor_x = con->cells_x - 1;

	con->cursor_y = to_abs_y(con, y);
	if (con->cursor_y > con->margin_bottom) {
		if (con->rel_addr)
			con->cursor_y = con->margin_bottom;
		else if (con->cursor_y >= con->cells_y)
			con->cursor_y = con->cells_y - 1;
	}
}

void kmscon_console_move_up(struct kmscon_console *con, unsigned int num,
								bool scroll)
{
	unsigned int diff;

	if (!con || !num)
		return;

	if (num > con->cells_y)
		num = con->cells_y;

	if (con->rel_addr) {
		diff = con->cursor_y - con->margin_top;
		if (num > diff) {
			num -= diff;
			if (scroll)
				kmscon_buffer_scroll_down(con->cells, num);
			con->cursor_y = con->margin_top;
		} else {
			con->cursor_y -= num;
		}
	} else {
		if (num > con->cursor_y) {
			num -= con->cursor_y;
			if (scroll)
				kmscon_buffer_scroll_down(con->cells, num);
			con->cursor_y = 0;
		} else {
			con->cursor_y -= num;
		}
	}
}

void kmscon_console_move_down(struct kmscon_console *con, unsigned int num,
								bool scroll)
{
	unsigned int diff;

	if (!con || !num)
		return;

	if (num > con->cells_y)
		num = con->cells_y;

	if (con->rel_addr) {
		diff = con->margin_bottom - con->cursor_y;
		if (num > diff) {
			num -= diff;
			if (scroll)
				kmscon_buffer_scroll_up(con->cells, num);
			con->cursor_y = con->margin_bottom;
		} else {
			con->cursor_y += num;
		}
	} else {
		diff = con->cells_y - con->cursor_y - 1;
		if (num > diff) {
			num -= diff;
			if (scroll)
				kmscon_buffer_scroll_up(con->cells, num);
			con->cursor_y = con->cells_y - 1;
		} else {
			con->cursor_y += num;
		}
	}
}

void kmscon_console_move_left(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	if (num > con->cells_x)
		num = con->cells_x;

	if (con->cursor_x >= con->cells_x)
		con->cursor_x = con->cells_x - 1;

	if (num > con->cursor_x)
		con->cursor_x = 0;
	else
		con->cursor_x -= num;
}

void kmscon_console_move_right(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	if (num > con->cells_x)
		num = con->cells_x;

	if (num + con->cursor_x >= con->cells_x)
		con->cursor_x = con->cells_x - 1;
	else
		con->cursor_x += num;
}

void kmscon_console_move_line_end(struct kmscon_console *con)
{
	if (!con)
		return;

	con->cursor_x = con->cells_x - 1;
}

void kmscon_console_move_line_home(struct kmscon_console *con)
{
	if (!con)
		return;

	con->cursor_x = 0;
}

void kmscon_console_erase_cursor(struct kmscon_console *con)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->cells_x)
		x = con->cells_x - 1;
	else
		x = con->cursor_x;

	kmscon_buffer_erase_region(con->cells, x, con->cursor_y,
							x, con->cursor_y);
}

void kmscon_console_erase_cursor_to_end(struct kmscon_console *con)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->cells_x)
		x = con->cells_x - 1;
	else
		x = con->cursor_x;

	kmscon_buffer_erase_region(con->cells, x, con->cursor_y,
					con->cells_x - 1, con->cursor_y);
}

void kmscon_console_erase_home_to_cursor(struct kmscon_console *con)
{
	if (!con)
		return;

	kmscon_buffer_erase_region(con->cells, 0, con->cursor_y,
					con->cursor_x, con->cursor_y);
}

void kmscon_console_erase_current_line(struct kmscon_console *con)
{
	if (!con)
		return;

	kmscon_buffer_erase_region(con->cells, 0, con->cursor_y,
					con->cells_x - 1, con->cursor_y);
}

void kmscon_console_erase_screen_to_cursor(struct kmscon_console *con)
{
	if (!con)
		return;

	kmscon_buffer_erase_region(con->cells, 0, 0,
						con->cursor_x, con->cursor_y);
}

void kmscon_console_erase_cursor_to_screen(struct kmscon_console *con)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->cells_x)
		x = con->cells_x - 1;
	else
		x = con->cursor_x;

	kmscon_buffer_erase_region(con->cells, x, con->cursor_y,
					con->cells_x - 1, con->cells_y - 1);
}

void kmscon_console_erase_screen(struct kmscon_console *con)
{
	if (!con)
		return;

	kmscon_buffer_erase_region(con->cells, 0, 0,
					con->cells_x - 1, con->cells_y - 1);
}
