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
#include "log.h"
#include "text.h"
#include "unicode.h"

#define LOG_SUBSYSTEM "console"

struct cell {
	kmscon_symbol_t ch;
	struct font_char_attr attr;
};

struct line {
	struct line *next;
	struct line *prev;

	unsigned int size;
	struct cell *cells;
};

struct kmscon_console {
	size_t ref;
	unsigned int flags;

	/* default attributes for new cells */
	struct font_char_attr def_attr;

	/* current buffer */
	unsigned int size_x;
	unsigned int size_y;
	unsigned int margin_top;
	unsigned int margin_bottom;
	unsigned int line_num;
	struct line **lines;

	/* scroll-back buffer */
	unsigned int sb_count;		/* number of lines in sb */
	struct line *sb_first;		/* first line; was moved first */
	struct line *sb_last;		/* last line; was moved last*/
	unsigned int sb_max;		/* max-limit of lines in sb */
	struct line *sb_pos;		/* current position in sb or NULL */

	/* cursor */
	unsigned int cursor_x;
	unsigned int cursor_y;

	/* tab ruler */
	bool *tab_ruler;
};

static void cell_init(struct kmscon_console *con, struct cell *cell)
{
	cell->ch = 0;
	memcpy(&cell->attr, &con->def_attr, sizeof(cell->attr));
}

static int line_new(struct kmscon_console *con, struct line **out,
		    unsigned int width)
{
	struct line *line;
	unsigned int i;

	if (!width)
		return -EINVAL;

	line = malloc(sizeof(*line));
	if (!line)
		return -ENOMEM;
	line->next = NULL;
	line->prev = NULL;
	line->size = width;

	line->cells = malloc(sizeof(struct cell) * width);
	if (!line->cells) {
		free(line);
		return -ENOMEM;
	}

	for (i = 0; i < width; ++i)
		cell_init(con, &line->cells[i]);

	*out = line;
	return 0;
}

static void line_free(struct line *line)
{
	free(line->cells);
	free(line);
}

static int line_resize(struct kmscon_console *con, struct line *line,
		       unsigned int width)
{
	struct cell *tmp;

	if (!line || !width)
		return -EINVAL;

	if (line->size < width) {
		tmp = realloc(line->cells, width * sizeof(struct cell));
		if (!tmp)
			return -ENOMEM;

		line->cells = tmp;
		line->size = width;

		while (line->size < width) {
			cell_init(con, &line->cells[line->size]);
			++line->size;
		}
	}

	return 0;
}

/* This links the given line into the scrollback-buffer */
static void link_to_scrollback(struct kmscon_console *con, struct line *line)
{
	struct line *tmp;

	if (con->sb_max == 0) {
		line_free(line);
		return;
	}

	/* Remove a line from the scrollback buffer if it reaches its maximum.
	 * We must take care to correctly keep the current position as the new
	 * line is linked in after we remove the top-most line here.
	 * sb_max == 0 is tested earlier so we can assume sb_max > 0 here. In
	 * other words, buf->sb_first is a valid line if sb_count >= sb_max. */
	if (con->sb_count >= con->sb_max) {
		tmp = con->sb_first;
		con->sb_first = tmp->next;
		if (tmp->next)
			tmp->next->prev = NULL;
		else
			con->sb_last = NULL;
		--con->sb_count;

		/* (position == tmp && !next) means we have sb_max=1 so set
		 * position to the new line. Otherwise, set to new first line.
		 * If position!=tmp and we have a fixed-position then nothing
		 * needs to be done because we can stay at the same line. If we
		 * have no fixed-position, we need to set the position to the
		 * next inserted line, which can be "line", too. */
		if (con->sb_pos) {
			if (con->sb_pos == tmp ||
			    !(con->flags & KMSCON_CONSOLE_FIXED_POS)) {
				if (con->sb_pos->next)
					con->sb_pos = con->sb_pos->next;
				else
					con->sb_pos = line;
			}
		}
		line_free(tmp);
	}

	line->next = NULL;
	line->prev = con->sb_last;
	if (con->sb_last)
		con->sb_last->next = line;
	else
		con->sb_first = line;
	con->sb_last = line;
	++con->sb_count;
}

/* Unlinks last line from the scrollback buffer, Returns NULL if it is empty */
static struct line *get_from_scrollback(struct kmscon_console *con)
{
	struct line *line;

	if (!con->sb_last)
		return NULL;

	line = con->sb_last;
	con->sb_last = line->prev;
	if (line->prev)
		line->prev->next = NULL;
	else
		con->sb_first = NULL;
	con->sb_count--;

	/* correctly move the current position if it is set in the sb */
	if (con->sb_pos) {
		if (con->flags & KMSCON_CONSOLE_FIXED_POS ||
		    !con->sb_pos->prev) {
			if (con->sb_pos == line)
				con->sb_pos = NULL;
		} else {
			con->sb_pos = con->sb_pos->prev;
		}
	}

	line->next = NULL;
	line->prev = NULL;
	return line;
}

static void console_scroll_up(struct kmscon_console *con, unsigned int num)
{
	unsigned int i, j, max;
	int ret;

	if (!num)
		return;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* We cache lines on the stack to speed up the scrolling. However, if
	 * num is too big we might get overflows here so use recursion if num
	 * exceeds a hard-coded limit.
	 * 128 seems to be a sane limit that should never be reached but should
	 * also be small enough so we do not get stack overflows. */
	if (num > 128) {
		console_scroll_up(con, 128);
		return console_scroll_up(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		ret = line_new(con, &cache[i], con->size_x);
		if (!ret) {
			link_to_scrollback(con,
					   con->lines[con->margin_top + i]);
		} else {
			cache[i] = con->lines[con->margin_top + i];
			for (j = 0; j < con->size_x; ++j)
				cell_init(con, &cache[i]->cells[j]);
		}
	}

	if (num < max) {
		memmove(&con->lines[con->margin_top],
			&con->lines[con->margin_top + num],
			(max - num) * sizeof(struct line*));
	}

	memcpy(&con->lines[con->margin_top + (max - num)],
	       cache, num * sizeof(struct line*));
}

static void console_scroll_down(struct kmscon_console *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!num)
		return;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* see console_scroll_up() for an explanation */
	if (num > 128) {
		console_scroll_down(con, 128);
		return console_scroll_down(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->margin_bottom - i];
		for (j = 0; j < con->size_x; ++j)
			cell_init(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->margin_top + num],
			&con->lines[con->margin_top],
			(max - num) * sizeof(struct line*));
	}

	memcpy(&con->lines[con->margin_top],
	       cache, num * sizeof(struct line*));
}

static void console_write(struct kmscon_console *con, unsigned int x,
			  unsigned int y, kmscon_symbol_t ch,
			  const struct font_char_attr *attr)
{
	struct line *line;

	if (x >= con->size_x || y >= con->size_y) {
		log_warn("writing beyond buffer boundary");
		return;
	}

	line = con->lines[y];

	if ((con->flags & KMSCON_CONSOLE_INSERT_MODE) && x < (con->size_x - 1))
		memmove(&line->cells[x + 1], &line->cells[x],
			sizeof(struct cell) * (con->size_x - 1 - x));
	line->cells[x].ch = ch;
	memcpy(&line->cells[x].attr, attr, sizeof(*attr));
}

static void console_erase_region(struct kmscon_console *con,
				 unsigned int x_from,
				 unsigned int y_from,
				 unsigned int x_to,
				 unsigned int y_to,
				 bool protect)
{
	unsigned int to;
	struct line *line;

	if (y_to >= con->size_y)
		y_to = con->size_y - 1;
	if (x_to >= con->size_x)
		x_to = con->size_x - 1;

	for ( ; y_from <= y_to; ++y_from) {
		line = con->lines[y_from];
		if (!line) {
			x_from = 0;
			continue;
		}

		if (y_from == y_to)
			to = x_to;
		else
			to = con->size_x - 1;
		for ( ; x_from <= to; ++x_from) {
			if (protect && line->cells[x_from].attr.protect)
				continue;

			cell_init(con, &line->cells[x_from]);
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
	if (!(con->flags & KMSCON_CONSOLE_REL_ORIGIN))
		return y;

	return con->margin_top + y;
}

int kmscon_console_new(struct kmscon_console **out)
{
	struct kmscon_console *con;
	int ret;
	unsigned int i;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	con->ref = 1;
	con->def_attr.fr = 255;
	con->def_attr.fg = 255;
	con->def_attr.fb = 255;

	ret = kmscon_console_resize(con, 80, 24);
	if (ret)
		goto err_free;

	log_debug("new console");
	*out = con;

	return 0;

err_free:
	for (i = 0; i < con->line_num; ++i)
		line_free(con->lines[i]);
	free(con->lines);
	free(con);
	return ret;
}

void kmscon_console_ref(struct kmscon_console *con)
{
	if (!con)
		return;

	++con->ref;
}

void kmscon_console_unref(struct kmscon_console *con)
{
	unsigned int i;

	if (!con || !con->ref || --con->ref)
		return;

	log_debug("destroying console");

	for (i = 0; i < con->line_num; ++i)
		line_free(con->lines[i]);
	free(con->lines);
	free(con->tab_ruler);
	free(con);
}

unsigned int kmscon_console_get_width(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->size_x;
}

unsigned int kmscon_console_get_height(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->size_y;
}

int kmscon_console_resize(struct kmscon_console *con, unsigned int x,
			  unsigned int y)
{
	struct line **cache;
	unsigned int i, j, width;
	int ret;
	bool *tab_ruler;

	if (!con || !x || !y)
		return -EINVAL;

	if (con->size_x == x && con->size_y == y)
		return 0;

	/* First make sure the line buffer is big enough for our new console.
	 * That is, allocate all new lines and make sure each line has enough
	 * cells to hold the new console or the current console. If we fail, we
	 * can safely return -ENOMEM and the buffer is still valid. We must
	 * allocate the new lines to at least the same size as the current
	 * lines. Otherwise, if this function fails in later turns, we will have
	 * invalid lines in the buffer. */
	if (y > con->line_num) {
		cache = realloc(con->lines, sizeof(struct line*) * y);
		if (!cache)
			return -ENOMEM;

		con->lines = cache;
		if (x > con->size_x)
			width = x;
		else
			width = con->size_x;

		while (con->line_num < y) {
			ret = line_new(con, &cache[con->line_num], width);
			if (ret)
				return ret;
			++con->line_num;
		}
	}

	/* Resize all lines in the buffer if we increase console width. This
	 * will guarantee that all lines are big enough so we can resize the
	 * buffer without reallocating them later. */
	if (x > con->size_x) {
		tab_ruler = realloc(con->tab_ruler, sizeof(bool) * x);
		if (!tab_ruler)
			return -ENOMEM;
		con->tab_ruler = tab_ruler;

		for (i = 0; i < con->line_num; ++i) {
			ret = line_resize(con, con->lines[i], x);
			if (ret)
				return ret;
		}
	}

	/* When resizing, we need to reset all the new cells, otherwise, the old
	 * data that was written there will reoccur on the screen. */
	for (j = 0; j < y; ++j) {
		for (i = con->size_x; i < x; ++i)
			cell_init(con, &con->lines[j]->cells[i]);
	}

	/* xterm destroys margins on resize, so do we */
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;

	/* scroll buffer if console height shrinks */
	if (con->size_y != 0 && y < con->size_y)
		console_scroll_up(con, con->size_y - y);

	/* reset tabs */
	for (i = 0; i < x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}

	con->size_x = x;
	con->size_y = y;
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	return 0;
}

int kmscon_console_set_margins(struct kmscon_console *con,
			       unsigned int top, unsigned int bottom)
{
	unsigned int upper, lower;

	if (!con)
		return -EINVAL;

	if (!top)
		top = 1;

	if (bottom <= top) {
		upper = 0;
		lower = con->size_y - 1;
	} else if (bottom > con->size_y) {
		upper = 0;
		lower = con->size_y - 1;
	} else {
		upper = top - 1;
		lower = bottom - 1;
	}

	con->margin_top = upper;
	con->margin_bottom = lower;
	return 0;
}

/* set maximum scrollback buffer size */
void kmscon_console_set_max_sb(struct kmscon_console *con,
			       unsigned int max)
{
	struct line *line;

	if (!con)
		return;

	while (con->sb_count > max) {
		line = con->sb_first;
		con->sb_first = line->next;
		if (line->next)
			line->next->prev = NULL;
		else
			con->sb_last = NULL;
		con->sb_count--;

		/* We treat fixed/unfixed position the same here because we
		 * remove lines from the TOP of the scrollback buffer. */
		if (con->sb_pos == line)
			con->sb_pos = con->sb_first;

		line_free(line);
	}

	con->sb_max = max;
}

/* clear scrollback buffer */
void kmscon_console_clear_sb(struct kmscon_console *con)
{
	struct line *iter, *tmp;

	if (!con)
		return;

	for (iter = con->sb_first; iter; ) {
		tmp = iter;
		iter = iter->next;
		line_free(tmp);
	}

	con->sb_first = NULL;
	con->sb_last = NULL;
	con->sb_count = 0;
	con->sb_pos = NULL;
}

void kmscon_console_sb_up(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	while (num--) {
		if (con->sb_pos) {
			if (!con->sb_pos->prev)
				return;

			con->sb_pos = con->sb_pos->prev;
		} else if (!con->sb_last) {
			return;
		} else {
			con->sb_pos = con->sb_last;
		}
	}
}

void kmscon_console_sb_down(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	while (num--) {
		if (con->sb_pos) {
			con->sb_pos = con->sb_pos->next;
			if (!con->sb_pos)
				return;
		} else {
			return;
		}
	}
}

void kmscon_console_sb_page_up(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	kmscon_console_sb_up(con, num * con->size_y);
}

void kmscon_console_sb_page_down(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	kmscon_console_sb_down(con, num * con->size_y);
}

void kmscon_console_sb_reset(struct kmscon_console *con)
{
	if (!con)
		return;

	con->sb_pos = NULL;
}

void kmscon_console_set_def_attr(struct kmscon_console *con,
				 const struct font_char_attr *attr)
{
	if (!con || !attr)
		return;

	memcpy(&con->def_attr, attr, sizeof(*attr));
}

void kmscon_console_reset(struct kmscon_console *con)
{
	unsigned int i;

	if (!con)
		return;

	con->flags = 0;
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;

	for (i = 0; i < con->size_x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}
}

void kmscon_console_set_flags(struct kmscon_console *con, unsigned int flags)
{
	if (!con || !flags)
		return;

	con->flags |= flags;
}

void kmscon_console_reset_flags(struct kmscon_console *con, unsigned int flags)
{
	if (!con || !flags)
		return;

	con->flags &= ~flags;
}

unsigned int kmscon_console_get_flags(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->flags;
}

unsigned int kmscon_console_get_cursor_x(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->cursor_x;
}

unsigned int kmscon_console_get_cursor_y(struct kmscon_console *con)
{
	if (!con)
		return 0;

	return con->cursor_y;
}

void kmscon_console_set_tabstop(struct kmscon_console *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = true;
}

void kmscon_console_reset_tabstop(struct kmscon_console *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = false;
}

void kmscon_console_reset_all_tabstops(struct kmscon_console *con)
{
	unsigned int i;

	if (!con)
		return;

	for (i = 0; i < con->size_x; ++i)
		con->tab_ruler[i] = false;
}

void kmscon_console_draw(struct kmscon_console *con, struct kmscon_text *txt)
{
	unsigned int cur_x, cur_y;
	unsigned int i, j, k;
	struct line *iter, *line = NULL;
	struct cell *cell;
	struct font_char_attr attr;
	bool cursor_done = false;
	int ret;

	if (!con || !txt)
		return;

	cur_x = con->cursor_x;
	if (con->cursor_x >= con->size_x)
		cur_x = con->size_x - 1;
	cur_y = con->cursor_y;
	if (con->cursor_y >= con->size_y)
		cur_y = con->size_y - 1;

	ret = kmscon_text_prepare(txt);
	if (ret) {
		log_warning("cannot prepare text-renderer for rendering");
		return;
	}

	iter = con->sb_pos;
	k = 0;
	for (i = 0; i < con->size_y; ++i) {
		if (iter) {
			line = iter;
			iter = iter->next;
		} else {
			line = con->lines[k];
			k++;
		}

		for (j = 0; j < con->size_x; ++j) {
			cell = &line->cells[j];
			memcpy(&attr, &cell->attr, sizeof(attr));

			if (k == cur_y + 1 &&
			    j == cur_x) {
				cursor_done = true;
				if (!(con->flags & KMSCON_CONSOLE_HIDE_CURSOR))
					attr.inverse = !attr.inverse;
			}

			/* TODO: do some more sophisticated inverse here. When
			 * INVERSE mode is set, we should instead just select
			 * inverse colors instead of switching background and
			 * foreground */
			if (con->flags & KMSCON_CONSOLE_INVERSE)
				attr.inverse = !attr.inverse;

			ret = kmscon_text_draw(txt, cell->ch, j, i, &attr);
			if (ret)
				log_debug("cannot draw glyph at %ux%u via text-renderer",
					  j, i);
		}

		if (k == cur_y + 1 && !cursor_done) {
			cursor_done = true;
			if (!(con->flags & KMSCON_CONSOLE_HIDE_CURSOR)) {
				if (!(con->flags & KMSCON_CONSOLE_INVERSE))
					attr.inverse = !attr.inverse;
				kmscon_text_draw(txt, 0, cur_x, i, &attr);
			}
		}
	}

	ret = kmscon_text_render(txt);
	if (ret)
		log_warning("cannot render via text-renderer");
}

void kmscon_console_write(struct kmscon_console *con, kmscon_symbol_t ch,
			  const struct font_char_attr *attr)
{
	unsigned int last;

	if (!con)
		return;

	if (con->cursor_y <= con->margin_bottom ||
	    con->cursor_y >= con->size_y)
		last = con->margin_bottom;
	else
		last = con->size_y - 1;

	if (con->cursor_x >= con->size_x) {
		if (con->flags & KMSCON_CONSOLE_AUTO_WRAP) {
			con->cursor_x = 0;
			++con->cursor_y;
		} else {
			con->cursor_x = con->size_x - 1;
		}
	}

	if (con->cursor_y > last) {
		con->cursor_y = last;
		console_scroll_up(con, 1);
	}

	console_write(con, con->cursor_x, con->cursor_y, ch, attr);
	++con->cursor_x;
}

void kmscon_console_newline(struct kmscon_console *con)
{
	if (!con)
		return;

	kmscon_console_move_down(con, 1, true);
	kmscon_console_move_line_home(con);
}

void kmscon_console_scroll_up(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	console_scroll_up(con, num);
}

void kmscon_console_scroll_down(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	console_scroll_down(con, num);
}

void kmscon_console_move_to(struct kmscon_console *con, unsigned int x,
			    unsigned int y)
{
	unsigned int last;

	if (!con)
		return;

	if (con->flags & KMSCON_CONSOLE_REL_ORIGIN)
		last = con->margin_bottom;
	else
		last = con->size_y - 1;

	con->cursor_x = to_abs_x(con, x);
	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;

	con->cursor_y = to_abs_y(con, y);
	if (con->cursor_y > last)
		con->cursor_y = last;
}

void kmscon_console_move_up(struct kmscon_console *con, unsigned int num,
			    bool scroll)
{
	unsigned int diff, size;

	if (!con || !num)
		return;

	if (con->cursor_y >= con->margin_top)
		size = con->margin_top;
	else
		size = 0;

	diff = con->cursor_y - size;
	if (num > diff) {
		num -= diff;
		if (scroll)
			console_scroll_down(con, num);
		con->cursor_y = size;
	} else {
		con->cursor_y -= num;
	}
}

void kmscon_console_move_down(struct kmscon_console *con, unsigned int num,
			      bool scroll)
{
	unsigned int diff, size;

	if (!con || !num)
		return;

	if (con->cursor_y <= con->margin_bottom)
		size = con->margin_bottom + 1;
	else
		size = con->size_y;

	diff = size - con->cursor_y - 1;
	if (num > diff) {
		num -= diff;
		if (scroll)
			console_scroll_up(con, num);
		con->cursor_y = size - 1;
	} else {
		con->cursor_y += num;
	}
}

void kmscon_console_move_left(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	if (num > con->size_x)
		num = con->size_x;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;

	if (num > con->cursor_x)
		con->cursor_x = 0;
	else
		con->cursor_x -= num;
}

void kmscon_console_move_right(struct kmscon_console *con, unsigned int num)
{
	if (!con || !num)
		return;

	if (num > con->size_x)
		num = con->size_x;

	if (num + con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	else
		con->cursor_x += num;
}

void kmscon_console_move_line_end(struct kmscon_console *con)
{
	if (!con)
		return;

	con->cursor_x = con->size_x - 1;
}

void kmscon_console_move_line_home(struct kmscon_console *con)
{
	if (!con)
		return;

	con->cursor_x = 0;
}

void kmscon_console_tab_right(struct kmscon_console *con, unsigned int num)
{
	unsigned int i, j;

	if (!con || !num)
		return;

	for (i = 0; i < num; ++i) {
		for (j = con->cursor_x + 1; j < con->size_x; ++j) {
			if (con->tab_ruler[j])
				break;
		}

		con->cursor_x = j;
		if (con->cursor_x + 1 >= con->size_x)
			break;
	}

	/* tabs never cause pending new-lines */
	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
}

void kmscon_console_tab_left(struct kmscon_console *con, unsigned int num)
{
	unsigned int i;
	int j;

	if (!con || !num)
		return;

	for (i = 0; i < num; ++i) {
		for (j = con->cursor_x - 1; j > 0; --j) {
			if (con->tab_ruler[j])
				break;
		}

		if (j <= 0) {
			con->cursor_x = 0;
			break;
		}
		con->cursor_x = j;
	}
}

void kmscon_console_insert_lines(struct kmscon_console *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!con || !num)
		return;

	if (con->cursor_y < con->margin_top ||
	    con->cursor_y > con->margin_bottom)
		return;

	max = con->margin_bottom - con->cursor_y + 1;
	if (num > max)
		num = max;

	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->margin_bottom - i];
		for (j = 0; j < con->size_x; ++j)
			cell_init(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->cursor_y + num],
			&con->lines[con->cursor_y],
			(max - num) * sizeof(struct line*));

		memcpy(&con->lines[con->cursor_y],
		       cache, num * sizeof(struct line*));
	}

	con->cursor_x = 0;
}

void kmscon_console_delete_lines(struct kmscon_console *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!con || !num)
		return;

	if (con->cursor_y < con->margin_top ||
	    con->cursor_y > con->margin_bottom)
		return;

	max = con->margin_bottom - con->cursor_y + 1;
	if (num > max)
		num = max;

	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		cache[i] = con->lines[con->cursor_y + i];
		for (j = 0; j < con->size_x; ++j)
			cell_init(con, &cache[i]->cells[j]);
	}

	if (num < max) {
		memmove(&con->lines[con->cursor_y],
			&con->lines[con->cursor_y + num],
			(max - num) * sizeof(struct line*));

		memcpy(&con->lines[con->cursor_y + (max - num)],
		       cache, num * sizeof(struct line*));
	}

	con->cursor_x = 0;
}

void kmscon_console_insert_chars(struct kmscon_console *con, unsigned int num)
{
	struct cell *cells;
	unsigned int max, mv, i;

	if (!con || !num || !con->size_y || !con->size_x)
		return;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	max = con->size_x - con->cursor_x;
	if (num > max)
		num = max;
	mv = max - num;

	cells = con->lines[con->cursor_y]->cells;
	if (mv)
		memmove(&cells[con->cursor_x + num],
			&cells[con->cursor_x],
			mv * sizeof(*cells));

	for (i = 0; i < num; ++i) {
		cell_init(con, &cells[con->cursor_x + i]);
	}
}

void kmscon_console_delete_chars(struct kmscon_console *con, unsigned int num)
{
	struct cell *cells;
	unsigned int max, mv, i;

	if (!con || !num || !con->size_y || !con->size_x)
		return;

	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	max = con->size_x - con->cursor_x;
	if (num > max)
		num = max;
	mv = max - num;

	cells = con->lines[con->cursor_y]->cells;
	if (mv)
		memmove(&cells[con->cursor_x],
			&cells[con->cursor_x + num],
			mv * sizeof(*cells));

	for (i = 0; i < num; ++i) {
		cell_init(con, &cells[con->cursor_x + mv + i]);
	}
}

void kmscon_console_erase_cursor(struct kmscon_console *con)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	console_erase_region(con, x, con->cursor_y, x, con->cursor_y, false);
}

void kmscon_console_erase_chars(struct kmscon_console *con, unsigned int num)
{
	unsigned int x;

	if (!con || !num)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	console_erase_region(con, x, con->cursor_y, x + num - 1, con->cursor_y,
			     false);
}

void kmscon_console_erase_cursor_to_end(struct kmscon_console *con,
				        bool protect)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	console_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

void kmscon_console_erase_home_to_cursor(struct kmscon_console *con,
					 bool protect)
{
	if (!con)
		return;

	console_erase_region(con, 0, con->cursor_y, con->cursor_x,
			     con->cursor_y, protect);
}

void kmscon_console_erase_current_line(struct kmscon_console *con,
				       bool protect)
{
	if (!con)
		return;

	console_erase_region(con, 0, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

void kmscon_console_erase_screen_to_cursor(struct kmscon_console *con,
					   bool protect)
{
	if (!con)
		return;

	console_erase_region(con, 0, 0, con->cursor_x, con->cursor_y, protect);
}

void kmscon_console_erase_cursor_to_screen(struct kmscon_console *con,
					   bool protect)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	console_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->size_y - 1, protect);
}

void kmscon_console_erase_screen(struct kmscon_console *con, bool protect)
{
	if (!con)
		return;

	console_erase_region(con, 0, 0, con->size_x - 1, con->size_y - 1,
			     protect);
}
