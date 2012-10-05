/*
 * TSM - Screen Management
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
 * Screen Management
 * This provides the screen drawing and manipulation functions. It does not
 * provide the terminal emulation. It is just an abstraction layer to draw text
 * to a framebuffer as used by terminals and consoles.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_llog.h"
#include "shl_timer.h"
#include "tsm_screen.h"
#include "tsm_unicode.h"

#define LLOG_SUBSYSTEM "tsm_screen"

struct cell {
	tsm_symbol_t ch;
	struct tsm_screen_attr attr;
};

struct line {
	struct line *next;
	struct line *prev;

	unsigned int size;
	struct cell *cells;
	uint64_t sb_id;
};

#define SELECTION_TOP -1
struct selection_pos {
	struct line *line;
	unsigned int x;
	int y;
};

struct tsm_screen {
	size_t ref;
	llog_submit_t llog;
	unsigned int opts;
	unsigned int flags;
	struct shl_timer *timer;

	/* default attributes for new cells */
	struct tsm_screen_attr def_attr;

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
	uint64_t sb_last_id;		/* last id given to sb-line */

	/* cursor */
	unsigned int cursor_x;
	unsigned int cursor_y;

	/* tab ruler */
	bool *tab_ruler;

	/* selection */
	bool sel_active;
	struct selection_pos sel_start;
	struct selection_pos sel_end;
};

static void cell_init(struct tsm_screen *con, struct cell *cell)
{
	cell->ch = 0;
	memcpy(&cell->attr, &con->def_attr, sizeof(cell->attr));
}

static int line_new(struct tsm_screen *con, struct line **out,
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

static int line_resize(struct tsm_screen *con, struct line *line,
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
static void link_to_scrollback(struct tsm_screen *con, struct line *line)
{
	struct line *tmp;

	if (con->sb_max == 0) {
		if (con->sel_active) {
			if (con->sel_start.line == line) {
				con->sel_start.line = NULL;
				con->sel_start.y = SELECTION_TOP;
			}
			if (con->sel_end.line == line) {
				con->sel_end.line = NULL;
				con->sel_end.y = SELECTION_TOP;
			}
		}
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
			    !(con->flags & TSM_SCREEN_FIXED_POS)) {
				if (con->sb_pos->next)
					con->sb_pos = con->sb_pos->next;
				else
					con->sb_pos = line;
			}
		}

		if (con->sel_active) {
			if (con->sel_start.line == tmp) {
				con->sel_start.line = NULL;
				con->sel_start.y = SELECTION_TOP;
			}
			if (con->sel_end.line == tmp) {
				con->sel_end.line = NULL;
				con->sel_end.y = SELECTION_TOP;
			}
		}
		line_free(tmp);
	}

	line->sb_id = ++con->sb_last_id;
	line->next = NULL;
	line->prev = con->sb_last;
	if (con->sb_last)
		con->sb_last->next = line;
	else
		con->sb_first = line;
	con->sb_last = line;
	++con->sb_count;
}

static void screen_scroll_up(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max, pos;
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
		screen_scroll_up(con, 128);
		return screen_scroll_up(con, num - 128);
	}
	struct line *cache[num];

	for (i = 0; i < num; ++i) {
		pos = con->margin_top + i;
		ret = line_new(con, &cache[i], con->size_x);
		if (!ret) {
			link_to_scrollback(con, con->lines[pos]);
		} else {
			cache[i] = con->lines[pos];
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

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y >= 0) {
			con->sel_start.y -= num;
			if (con->sel_start.y < 0) {
				con->sel_start.line = con->sb_last;
				while (con->sel_start.line && ++con->sel_start.y < 0)
					con->sel_start.line = con->sel_start.line->prev;
				con->sel_start.y = SELECTION_TOP;
			}
		}
		if (!con->sel_end.line && con->sel_end.y >= 0) {
			con->sel_end.y -= num;
			if (con->sel_end.y < 0) {
				con->sel_end.line = con->sb_last;
				while (con->sel_end.line && ++con->sel_end.y < 0)
					con->sel_end.line = con->sel_end.line->prev;
				con->sel_end.y = SELECTION_TOP;
			}
		}
	}
}

static void screen_scroll_down(struct tsm_screen *con, unsigned int num)
{
	unsigned int i, j, max;

	if (!num)
		return;

	max = con->margin_bottom + 1 - con->margin_top;
	if (num > max)
		num = max;

	/* see screen_scroll_up() for an explanation */
	if (num > 128) {
		screen_scroll_down(con, 128);
		return screen_scroll_down(con, num - 128);
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

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y >= 0)
			con->sel_start.y += num;
		if (!con->sel_end.line && con->sel_end.y >= 0)
			con->sel_end.y += num;
	}
}

static void screen_write(struct tsm_screen *con, unsigned int x,
			  unsigned int y, tsm_symbol_t ch,
			  const struct tsm_screen_attr *attr)
{
	struct line *line;

	if (x >= con->size_x || y >= con->size_y) {
		llog_warn(con, "writing beyond buffer boundary");
		return;
	}

	line = con->lines[y];

	if ((con->flags & TSM_SCREEN_INSERT_MODE) && x < (con->size_x - 1))
		memmove(&line->cells[x + 1], &line->cells[x],
			sizeof(struct cell) * (con->size_x - 1 - x));
	line->cells[x].ch = ch;
	memcpy(&line->cells[x].attr, attr, sizeof(*attr));
}

static void screen_erase_region(struct tsm_screen *con,
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

static inline unsigned int to_abs_x(struct tsm_screen *con, unsigned int x)
{
	return x;
}

static inline unsigned int to_abs_y(struct tsm_screen *con, unsigned int y)
{
	if (!(con->flags & TSM_SCREEN_REL_ORIGIN))
		return y;

	return con->margin_top + y;
}

int tsm_screen_new(struct tsm_screen **out, tsm_log_t log)
{
	struct tsm_screen *con;
	int ret;
	unsigned int i;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	con->ref = 1;
	con->llog = log;
	con->def_attr.fr = 255;
	con->def_attr.fg = 255;
	con->def_attr.fb = 255;

	ret = shl_timer_new(&con->timer);
	if (ret)
		goto err_free;

	ret = tsm_screen_resize(con, 80, 24);
	if (ret)
		goto err_timer;

	llog_debug(con, "new screen");
	*out = con;

	return 0;

err_timer:
	shl_timer_free(con->timer);
	for (i = 0; i < con->line_num; ++i)
		line_free(con->lines[i]);
	free(con->lines);
	free(con->tab_ruler);
err_free:
	free(con);
	return ret;
}

void tsm_screen_ref(struct tsm_screen *con)
{
	if (!con)
		return;

	++con->ref;
}

void tsm_screen_unref(struct tsm_screen *con)
{
	unsigned int i;

	if (!con || !con->ref || --con->ref)
		return;

	llog_debug(con, "destroying screen");

	for (i = 0; i < con->line_num; ++i)
		line_free(con->lines[i]);
	free(con->lines);
	free(con->tab_ruler);
	shl_timer_free(con->timer);
	free(con);
}

void tsm_screen_set_opts(struct tsm_screen *scr, unsigned int opts)
{
	if (!scr || !opts)
		return;

	scr->opts |= opts;
}

void tsm_screen_reset_opts(struct tsm_screen *scr, unsigned int opts)
{
	if (!scr || !opts)
		return;

	scr->opts &= ~opts;
}

unsigned int tsm_screen_get_opts(struct tsm_screen *scr)
{
	if (!scr)
		return 0;

	return scr->opts;
}

unsigned int tsm_screen_get_width(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->size_x;
}

unsigned int tsm_screen_get_height(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->size_y;
}

int tsm_screen_resize(struct tsm_screen *con, unsigned int x,
			  unsigned int y)
{
	struct line **cache;
	unsigned int i, j, width, diff;
	int ret;
	bool *tab_ruler;

	if (!con || !x || !y)
		return -EINVAL;

	if (con->size_x == x && con->size_y == y)
		return 0;

	/* First make sure the line buffer is big enough for our new screen.
	 * That is, allocate all new lines and make sure each line has enough
	 * cells to hold the new screen or the current screen. If we fail, we
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

	/* Resize all lines in the buffer if we increase screen width. This
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
	 * data that was written there will reoccur on the screen.
	 * TODO: we overwrite way to much here; most of it should already be
	 * cleaned. Maybe it does more sense cleaning when _allocating_ or when
	 * _shrinking_, then we never clean twice (for performance reasons). */
	for (j = 0; j < con->line_num; ++j) {
		if (j >= con->size_y)
			i = 0;
		else
			i = con->size_x;
		if (x < con->lines[j]->size)
			width = x;
		else
			width = con->lines[j]->size;
		for (; i < width; ++i)
			cell_init(con, &con->lines[j]->cells[i]);
	}

	/* xterm destroys margins on resize, so do we */
	con->margin_top = 0;
	con->margin_bottom = con->size_y - 1;

	/* reset tabs */
	for (i = 0; i < x; ++i) {
		if (i % 8 == 0)
			con->tab_ruler[i] = true;
		else
			con->tab_ruler[i] = false;
	}

	/* We need to adjust x-size first as screen_scroll_up() and friends may
	 * have to reallocate lines. The y-size is adjusted after them to avoid
	 * missing lines when shrinking y-size.
	 * We need to carefully look for the functions that we call here as they
	 * have stronger invariants as when called normally. */

	con->size_x = x;
	if (con->cursor_x >= con->size_x)
		con->cursor_x = con->size_x - 1;

	/* scroll buffer if screen height shrinks */
	if (con->size_y != 0 && y < con->size_y) {
		diff = con->size_y - y;
		screen_scroll_up(con, diff);
		if (con->cursor_y > diff)
			con->cursor_y -= diff;
		else
			con->cursor_y = 0;
	}

	con->size_y = y;
	con->margin_bottom = con->size_y - 1;
	if (con->cursor_y >= con->size_y)
		con->cursor_y = con->size_y - 1;

	return 0;
}

int tsm_screen_set_margins(struct tsm_screen *con,
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
void tsm_screen_set_max_sb(struct tsm_screen *con,
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

		if (con->sel_active) {
			if (con->sel_start.line == line) {
				con->sel_start.line = NULL;
				con->sel_start.y = SELECTION_TOP;
			}
			if (con->sel_end.line == line) {
				con->sel_end.line = NULL;
				con->sel_end.y = SELECTION_TOP;
			}
		}
		line_free(line);
	}

	con->sb_max = max;
}

/* clear scrollback buffer */
void tsm_screen_clear_sb(struct tsm_screen *con)
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

	if (con->sel_active) {
		if (con->sel_start.line) {
			con->sel_start.line = NULL;
			con->sel_start.y = SELECTION_TOP;
		}
		if (con->sel_end.line) {
			con->sel_end.line = NULL;
			con->sel_end.y = SELECTION_TOP;
		}
	}
}

void tsm_screen_sb_up(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_sb_down(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_sb_page_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	tsm_screen_sb_up(con, num * con->size_y);
}

void tsm_screen_sb_page_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	tsm_screen_sb_down(con, num * con->size_y);
}

void tsm_screen_sb_reset(struct tsm_screen *con)
{
	if (!con)
		return;

	con->sb_pos = NULL;
}

void tsm_screen_set_def_attr(struct tsm_screen *con,
				 const struct tsm_screen_attr *attr)
{
	if (!con || !attr)
		return;

	memcpy(&con->def_attr, attr, sizeof(*attr));
}

void tsm_screen_reset(struct tsm_screen *con)
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

void tsm_screen_set_flags(struct tsm_screen *con, unsigned int flags)
{
	if (!con || !flags)
		return;

	con->flags |= flags;
}

void tsm_screen_reset_flags(struct tsm_screen *con, unsigned int flags)
{
	if (!con || !flags)
		return;

	con->flags &= ~flags;
}

unsigned int tsm_screen_get_flags(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->flags;
}

unsigned int tsm_screen_get_cursor_x(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cursor_x;
}

unsigned int tsm_screen_get_cursor_y(struct tsm_screen *con)
{
	if (!con)
		return 0;

	return con->cursor_y;
}

void tsm_screen_set_tabstop(struct tsm_screen *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = true;
}

void tsm_screen_reset_tabstop(struct tsm_screen *con)
{
	if (!con || con->cursor_x >= con->size_x)
		return;

	con->tab_ruler[con->cursor_x] = false;
}

void tsm_screen_reset_all_tabstops(struct tsm_screen *con)
{
	unsigned int i;

	if (!con)
		return;

	for (i = 0; i < con->size_x; ++i)
		con->tab_ruler[i] = false;
}

void tsm_screen_write(struct tsm_screen *con, tsm_symbol_t ch,
			  const struct tsm_screen_attr *attr)
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
		if (con->flags & TSM_SCREEN_AUTO_WRAP) {
			con->cursor_x = 0;
			++con->cursor_y;
		} else {
			con->cursor_x = con->size_x - 1;
		}
	}

	if (con->cursor_y > last) {
		con->cursor_y = last;
		screen_scroll_up(con, 1);
	}

	screen_write(con, con->cursor_x, con->cursor_y, ch, attr);
	++con->cursor_x;
}

void tsm_screen_newline(struct tsm_screen *con)
{
	if (!con)
		return;

	tsm_screen_move_down(con, 1, true);
	tsm_screen_move_line_home(con);
}

void tsm_screen_scroll_up(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_scroll_up(con, num);
}

void tsm_screen_scroll_down(struct tsm_screen *con, unsigned int num)
{
	if (!con || !num)
		return;

	screen_scroll_down(con, num);
}

void tsm_screen_move_to(struct tsm_screen *con, unsigned int x,
			    unsigned int y)
{
	unsigned int last;

	if (!con)
		return;

	if (con->flags & TSM_SCREEN_REL_ORIGIN)
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

void tsm_screen_move_up(struct tsm_screen *con, unsigned int num,
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
			screen_scroll_down(con, num);
		con->cursor_y = size;
	} else {
		con->cursor_y -= num;
	}
}

void tsm_screen_move_down(struct tsm_screen *con, unsigned int num,
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
			screen_scroll_up(con, num);
		con->cursor_y = size - 1;
	} else {
		con->cursor_y += num;
	}
}

void tsm_screen_move_left(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_move_right(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_move_line_end(struct tsm_screen *con)
{
	if (!con)
		return;

	con->cursor_x = con->size_x - 1;
}

void tsm_screen_move_line_home(struct tsm_screen *con)
{
	if (!con)
		return;

	con->cursor_x = 0;
}

void tsm_screen_tab_right(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_tab_left(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_insert_lines(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_delete_lines(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_insert_chars(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_delete_chars(struct tsm_screen *con, unsigned int num)
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

void tsm_screen_erase_cursor(struct tsm_screen *con)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, x, con->cursor_y, false);
}

void tsm_screen_erase_chars(struct tsm_screen *con, unsigned int num)
{
	unsigned int x;

	if (!con || !num)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, x + num - 1, con->cursor_y,
			     false);
}

void tsm_screen_erase_cursor_to_end(struct tsm_screen *con,
				        bool protect)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

void tsm_screen_erase_home_to_cursor(struct tsm_screen *con,
					 bool protect)
{
	if (!con)
		return;

	screen_erase_region(con, 0, con->cursor_y, con->cursor_x,
			     con->cursor_y, protect);
}

void tsm_screen_erase_current_line(struct tsm_screen *con,
				       bool protect)
{
	if (!con)
		return;

	screen_erase_region(con, 0, con->cursor_y, con->size_x - 1,
			     con->cursor_y, protect);
}

void tsm_screen_erase_screen_to_cursor(struct tsm_screen *con,
					   bool protect)
{
	if (!con)
		return;

	screen_erase_region(con, 0, 0, con->cursor_x, con->cursor_y, protect);
}

void tsm_screen_erase_cursor_to_screen(struct tsm_screen *con,
					   bool protect)
{
	unsigned int x;

	if (!con)
		return;

	if (con->cursor_x >= con->size_x)
		x = con->size_x - 1;
	else
		x = con->cursor_x;

	screen_erase_region(con, x, con->cursor_y, con->size_x - 1,
			     con->size_y - 1, protect);
}

void tsm_screen_erase_screen(struct tsm_screen *con, bool protect)
{
	if (!con)
		return;

	screen_erase_region(con, 0, 0, con->size_x - 1, con->size_y - 1,
			     protect);
}

/*
 * Selection Code
 * If a running pty-client does not support mouse-tracking extensions, a
 * terminal can manually mark selected areas if it does mouse-tracking itself.
 * This tracking is slightly different than the integrated client-tracking:
 *
 * Initial state is no-selection. At any time selection_reset() can be called to
 * clear the selection and go back to initial state.
 * If the user presses a mouse-button, the terminal can calculate the selected
 * cell and call selection_start() to notify the terminal that the user started
 * the selection. While the mouse-button is held down, the terminal should call
 * selection_target() whenever a mouse-event occurs. This will tell the screen
 * layer to draw the selection from the initial start up to the last given
 * target.
 * Please note that the selection-start cannot be modified by the terminal
 * during a selection. Instead, the screen-layer automatically moves it along
 * with any scroll-operations or inserts/deletes. This also means, the terminal
 * must _not_ cache the start-position itself as it may change under the hood.
 * This selection takes also care of scrollback-buffer selections and correctly
 * moves selection state along.
 *
 * Please note that this is not the kind of selection that some PTY applications
 * support. If the client supports the mouse-protocol, then it can also control
 * a separate screen-selection which is always inside of the actual screen. This
 * is a totally different selection.
 */

static void selection_set(struct tsm_screen *con, struct selection_pos *sel,
			  unsigned int x, unsigned int y)
{
	struct line *pos;

	sel->line = NULL;
	pos = con->sb_pos;

	while (y && pos) {
		--y;
		pos = pos->next;
	}

	if (pos)
		sel->line = pos;

	sel->x = x;
	sel->y = y;
}

void tsm_screen_selection_reset(struct tsm_screen *con)
{
	if (!con)
		return;

	con->sel_active = false;
}

void tsm_screen_selection_start(struct tsm_screen *con,
				unsigned int posx,
				unsigned int posy)
{
	if (!con)
		return;

	con->sel_active = true;
	selection_set(con, &con->sel_start, posx, posy);
	memcpy(&con->sel_end, &con->sel_start, sizeof(con->sel_end));
}

void tsm_screen_selection_target(struct tsm_screen *con,
				 unsigned int posx,
				 unsigned int posy)
{
	if (!con || !con->sel_active)
		return;

	selection_set(con, &con->sel_end, posx, posy);
}

void tsm_screen_draw(struct tsm_screen *con,
			 tsm_screen_prepare_cb prepare_cb,
			 tsm_screen_draw_cb draw_cb,
			 tsm_screen_render_cb render_cb,
			 void *data)
{
	unsigned int cur_x, cur_y;
	unsigned int i, j, k;
	struct line *iter, *line = NULL;
	struct cell *cell;
	struct tsm_screen_attr attr;
	bool cursor_done = false;
	int ret, warned = 0;
	uint64_t time_prep = 0, time_draw = 0, time_rend = 0;
	const uint32_t *ch;
	size_t len;
	struct cell empty;
	bool in_sel = false, sel_start = false, sel_end = false;
	bool was_sel = false;

	if (!con || !draw_cb)
		return;

	cell_init(con, &empty);

	cur_x = con->cursor_x;
	if (con->cursor_x >= con->size_x)
		cur_x = con->size_x - 1;
	cur_y = con->cursor_y;
	if (con->cursor_y >= con->size_y)
		cur_y = con->size_y - 1;

	/* render preparation */

	if (prepare_cb) {
		if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
			shl_timer_reset(con->timer);

		ret = prepare_cb(con, data);
		if (ret) {
			llog_warning(con,
				     "cannot prepare text-renderer for rendering");
			return;
		}

		if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
			time_prep = shl_timer_elapsed(con->timer);
	} else {
		time_prep = 0;
	}

	/* push each character into rendering pipeline */

	if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
		shl_timer_reset(con->timer);

	iter = con->sb_pos;
	k = 0;

	if (con->sel_active) {
		if (!con->sel_start.line && con->sel_start.y == SELECTION_TOP)
			in_sel = !in_sel;
		if (!con->sel_end.line && con->sel_end.y == SELECTION_TOP)
			in_sel = !in_sel;

		if (con->sel_start.line &&
		    (!iter || con->sel_start.line->sb_id < iter->sb_id))
			in_sel = !in_sel;
		if (con->sel_end.line &&
		    (!iter || con->sel_end.line->sb_id < iter->sb_id))
			in_sel = !in_sel;
	}

	for (i = 0; i < con->size_y; ++i) {
		if (iter) {
			line = iter;
			iter = iter->next;
		} else {
			line = con->lines[k];
			k++;
		}

		if (con->sel_active) {
			if (con->sel_start.line == line ||
			    (!con->sel_start.line &&
			     con->sel_start.y == k - 1))
				sel_start = true;
			else
				sel_start = false;
			if (con->sel_end.line == line ||
			    (!con->sel_end.line &&
			     con->sel_end.y == k - 1))
				sel_end = true;
			else
				sel_end = false;

			was_sel = false;
		}

		for (j = 0; j < con->size_x; ++j) {
			if (j < line->size)
				cell = &line->cells[j];
			else
				cell = &empty;
			memcpy(&attr, &cell->attr, sizeof(attr));

			if (con->sel_active) {
				if (sel_start &&
				    j == con->sel_start.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
				if (sel_end &&
				    j == con->sel_end.x) {
					was_sel = in_sel;
					in_sel = !in_sel;
				}
			}

			if (k == cur_y + 1 &&
			    j == cur_x) {
				cursor_done = true;
				if (!(con->flags & TSM_SCREEN_HIDE_CURSOR))
					attr.inverse = !attr.inverse;
			}

			/* TODO: do some more sophisticated inverse here. When
			 * INVERSE mode is set, we should instead just select
			 * inverse colors instead of switching background and
			 * foreground */
			if (con->flags & TSM_SCREEN_INVERSE)
				attr.inverse = !attr.inverse;

			if (in_sel || was_sel) {
				was_sel = false;
				attr.inverse = !attr.inverse;
			}

			ch = tsm_symbol_get(NULL, &cell->ch, &len);
			if (cell->ch == ' ' || cell->ch == 0)
				len = 0;
			ret = draw_cb(con, cell->ch, ch, len, j, i, &attr,
				      data);
			if (ret && warned++ < 3) {
				llog_debug(con,
					   "cannot draw glyph at %ux%u via text-renderer",
					   j, i);
				if (warned == 3)
					llog_debug(con,
						   "suppressing further warnings during this rendering round");
			}
		}

		if (k == cur_y + 1 && !cursor_done) {
			cursor_done = true;
			if (!(con->flags & TSM_SCREEN_HIDE_CURSOR)) {
				if (!(con->flags & TSM_SCREEN_INVERSE))
					attr.inverse = !attr.inverse;
				draw_cb(con, 0, NULL, 0, cur_x, i, &attr, data);
			}
		}
	}

	if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
		time_draw = shl_timer_elapsed(con->timer);

	/* perform final rendering steps */

	if (render_cb) {
		if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
			shl_timer_reset(con->timer);

		ret = render_cb(con, data);
		if (ret)
			llog_warning(con,
				     "cannot render via text-renderer");

		if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
			time_rend = shl_timer_elapsed(con->timer);
	} else {
		time_rend = 0;
	}

	if (con->opts & TSM_SCREEN_OPT_RENDER_TIMING)
		llog_debug(con,
			   "timing: sum: %" PRIu64 " prepare: %" PRIu64 " draw: %" PRIu64 " render: %" PRIu64,
			   time_prep + time_draw + time_rend,
			   time_prep, time_draw, time_rend);
}
