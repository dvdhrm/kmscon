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
#include <stdlib.h>
#include "font.h"
#include "gl.h"
#include "unicode.h"

struct kmscon_console;

/* console objects */

/* modes for kmscon_console_write() */
#define KMSCON_CONSOLE_INSERT		0x01
#define KMSCON_CONSOLE_WRAP		0x02
/* modes for kmscon_console_re/set() */
#define KMSCON_CONSOLE_REL_ORIGIN	0x04

int kmscon_console_new(struct kmscon_console **out);
void kmscon_console_ref(struct kmscon_console *con);
void kmscon_console_unref(struct kmscon_console *con);

unsigned int kmscon_console_get_width(struct kmscon_console *con);
unsigned int kmscon_console_get_height(struct kmscon_console *con);
int kmscon_console_resize(struct kmscon_console *con, unsigned int x,
					unsigned int y, unsigned int height);
void kmscon_console_reset(struct kmscon_console *con);
void kmscon_console_set_flags(struct kmscon_console *con, unsigned int flags);
void kmscon_console_reset_flags(struct kmscon_console *con, unsigned int flags);
unsigned int kmscon_console_get_flags(struct kmscon_console *con);

void kmscon_console_draw(struct kmscon_console *con, struct font_screen *fscr);

void kmscon_console_write(struct kmscon_console *con, kmscon_symbol_t ch,
			  const struct font_char_attr *attr,
			  unsigned int flags);
void kmscon_console_newline(struct kmscon_console *con);
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
void kmscon_console_erase_cursor(struct kmscon_console *con);
void kmscon_console_erase_cursor_to_end(struct kmscon_console *con);
void kmscon_console_erase_home_to_cursor(struct kmscon_console *con);
void kmscon_console_erase_current_line(struct kmscon_console *con);
void kmscon_console_erase_screen_to_cursor(struct kmscon_console *con);
void kmscon_console_erase_cursor_to_screen(struct kmscon_console *con);
void kmscon_console_erase_screen(struct kmscon_console *con);

#endif /* KMSCON_CONSOLE_H */
