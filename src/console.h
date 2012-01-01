/*
 * kmscon - Console Management
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
 * Console Management
 * The console management uses OpenGL, cairo and pango to draw a console to a
 * framebuffer. It is independent of the other subsystems and can also be used
 * in other applications.
 *
 * This console does not emulate any terminal at all. This subsystem just
 * provides functions to draw a console to a framebuffer and modifying the state
 * of it.
 */

#include <inttypes.h>
#include <stdlib.h>
#include "font.h"
#include "unicode.h"

struct kmscon_buffer;
struct kmscon_console;

/* console buffer with cell objects */

int kmscon_buffer_new(struct kmscon_buffer **out, unsigned int x,
							unsigned int y);
void kmscon_buffer_ref(struct kmscon_buffer *buf);
void kmscon_buffer_unref(struct kmscon_buffer *buf);

int kmscon_buffer_resize(struct kmscon_buffer *buf, unsigned int x,
							unsigned int y);
void kmscon_buffer_draw(struct kmscon_buffer *buf, struct kmscon_font *font,
			void *dcr, unsigned int width, unsigned int height);

unsigned int kmscon_buffer_get_width(struct kmscon_buffer *buf);
unsigned int kmscon_buffer_get_height(struct kmscon_buffer *buf);
void kmscon_buffer_write(struct kmscon_buffer *buf, unsigned int x,
					unsigned int y, kmscon_symbol_t ch);
kmscon_symbol_t kmscon_buffer_read(struct kmscon_buffer *buf, unsigned int x,
							unsigned int y);
void kmscon_buffer_rotate(struct kmscon_buffer *buf);

/* console objects */

int kmscon_console_new(struct kmscon_console **out,
					struct kmscon_font_factory *ff);
void kmscon_console_ref(struct kmscon_console *con);
void kmscon_console_unref(struct kmscon_console *con);

unsigned int kmscon_console_get_width(struct kmscon_console *con);
unsigned int kmscon_console_get_height(struct kmscon_console *con);
int kmscon_console_resize(struct kmscon_console *con, unsigned int x,
					unsigned int y, unsigned int height);

void kmscon_console_draw(struct kmscon_console *con);
void kmscon_console_map(struct kmscon_console *con);

void kmscon_console_write(struct kmscon_console *con, kmscon_symbol_t ch);
void kmscon_console_newline(struct kmscon_console *con);
