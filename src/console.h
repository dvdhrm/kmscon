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
 * This console does not emulate any terminal at all. This subsystem just
 * provides functions to draw a console to a framebuffer and modifying the state
 * of it.
 */

#ifndef KMSCON_CONSOLE_H
#define KMSCON_CONSOLE_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include "unicode.h"

struct kmscon_console;

/* console objects */

#define KMSCON_CONSOLE_INSERT_MODE	0x01
#define KMSCON_CONSOLE_AUTO_WRAP	0x02
#define KMSCON_CONSOLE_REL_ORIGIN	0x04
#define KMSCON_CONSOLE_INVERSE		0x08
#define KMSCON_CONSOLE_HIDE_CURSOR	0x10
#define KMSCON_CONSOLE_FIXED_POS	0x20

struct kmscon_console_attr {
	int8_t fccode;			/* foreground color code or <0 for rgb */
	int8_t bccode;			/* background color code or <0 for rgb */
	uint8_t fr;			/* foreground red */
	uint8_t fg;			/* foreground green */
	uint8_t fb;			/* foreground blue */
	uint8_t br;			/* background red */
	uint8_t bg;			/* background green */
	uint8_t bb;			/* background blue */
	unsigned int bold : 1;		/* bold character */
	unsigned int underline : 1;	/* underlined character */
	unsigned int inverse : 1;	/* inverse colors */
	unsigned int protect : 1;	/* cannot be erased */
};

typedef int (*kmscon_console_prepare_cb) (struct kmscon_console *con,
					  void *data);
typedef int (*kmscon_console_draw_cb) (struct kmscon_console *con,
				       tsm_symbol_t ch,
				       unsigned int posx,
				       unsigned int posy,
				       const struct kmscon_console_attr *attr,
				       void *data);
typedef int (*kmscon_console_render_cb) (struct kmscon_console *con,
					 void *data);

int kmscon_console_new(struct kmscon_console **out);
void kmscon_console_ref(struct kmscon_console *con);
void kmscon_console_unref(struct kmscon_console *con);

unsigned int kmscon_console_get_width(struct kmscon_console *con);
unsigned int kmscon_console_get_height(struct kmscon_console *con);
int kmscon_console_resize(struct kmscon_console *con, unsigned int x,
			  unsigned int y);
int kmscon_console_set_margins(struct kmscon_console *con,
			       unsigned int top, unsigned int bottom);
void kmscon_console_set_max_sb(struct kmscon_console *con, unsigned int max);
void kmscon_console_clear_sb(struct kmscon_console *con);

void kmscon_console_sb_up(struct kmscon_console *con, unsigned int num);
void kmscon_console_sb_down(struct kmscon_console *con, unsigned int num);
void kmscon_console_sb_page_up(struct kmscon_console *con, unsigned int num);
void kmscon_console_sb_page_down(struct kmscon_console *con, unsigned int num);
void kmscon_console_sb_reset(struct kmscon_console *con);

void kmscon_console_set_def_attr(struct kmscon_console *con,
				 const struct kmscon_console_attr *attr);
void kmscon_console_reset(struct kmscon_console *con);
void kmscon_console_set_flags(struct kmscon_console *con, unsigned int flags);
void kmscon_console_reset_flags(struct kmscon_console *con, unsigned int flags);
unsigned int kmscon_console_get_flags(struct kmscon_console *con);

unsigned int kmscon_console_get_cursor_x(struct kmscon_console *con);
unsigned int kmscon_console_get_cursor_y(struct kmscon_console *con);

void kmscon_console_set_tabstop(struct kmscon_console *con);
void kmscon_console_reset_tabstop(struct kmscon_console *con);
void kmscon_console_reset_all_tabstops(struct kmscon_console *con);

void kmscon_console_write(struct kmscon_console *con, tsm_symbol_t ch,
			  const struct kmscon_console_attr *attr);
void kmscon_console_newline(struct kmscon_console *con);
void kmscon_console_scroll_up(struct kmscon_console *con, unsigned int num);
void kmscon_console_scroll_down(struct kmscon_console *con, unsigned int num);
void kmscon_console_move_to(struct kmscon_console *con, unsigned int x,
			    unsigned int y);
void kmscon_console_move_up(struct kmscon_console *con, unsigned int num,
			    bool scroll);
void kmscon_console_move_down(struct kmscon_console *con, unsigned int num,
			      bool scroll);
void kmscon_console_move_left(struct kmscon_console *con, unsigned int num);
void kmscon_console_move_right(struct kmscon_console *con, unsigned int num);
void kmscon_console_move_line_end(struct kmscon_console *con);
void kmscon_console_move_line_home(struct kmscon_console *con);
void kmscon_console_tab_right(struct kmscon_console *con, unsigned int num);
void kmscon_console_tab_left(struct kmscon_console *con, unsigned int num);
void kmscon_console_insert_lines(struct kmscon_console *con, unsigned int num);
void kmscon_console_delete_lines(struct kmscon_console *con, unsigned int num);
void kmscon_console_insert_chars(struct kmscon_console *con, unsigned int num);
void kmscon_console_delete_chars(struct kmscon_console *con, unsigned int num);
void kmscon_console_erase_cursor(struct kmscon_console *con);
void kmscon_console_erase_chars(struct kmscon_console *con, unsigned int num);
void kmscon_console_erase_cursor_to_end(struct kmscon_console *con,
					bool protect);
void kmscon_console_erase_home_to_cursor(struct kmscon_console *con,
					 bool protect);
void kmscon_console_erase_current_line(struct kmscon_console *con,
				       bool protect);
void kmscon_console_erase_screen_to_cursor(struct kmscon_console *con,
					   bool protect);
void kmscon_console_erase_cursor_to_screen(struct kmscon_console *con,
					   bool protect);
void kmscon_console_erase_screen(struct kmscon_console *con, bool protect);

void kmscon_console_draw(struct kmscon_console *con,
			 kmscon_console_prepare_cb prepare_cb,
			 kmscon_console_draw_cb draw_cb,
			 kmscon_console_render_cb render_cb,
			 void *data);

#endif /* KMSCON_CONSOLE_H */
