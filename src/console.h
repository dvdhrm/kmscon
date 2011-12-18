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

#include <cairo.h>
#include <inttypes.h>
#include <stdlib.h>

struct kmscon_char;
struct kmscon_font;
struct kmscon_buffer;
struct kmscon_console;

/* single printable characters */

int kmscon_char_new(struct kmscon_char **out);
int kmscon_char_new_u8(struct kmscon_char **out, const char *str, size_t len);
int kmscon_char_dup(struct kmscon_char **out, const struct kmscon_char *orig);
void kmscon_char_free(struct kmscon_char *ch);
void kmscon_char_reset(struct kmscon_char *ch);

int kmscon_char_set(struct kmscon_char *ch, const struct kmscon_char *orig);
int kmscon_char_set_u8(struct kmscon_char *ch, const char *str, size_t len);
const char *kmscon_char_get_u8(const struct kmscon_char *ch);
size_t kmscon_char_get_len(const struct kmscon_char *ch);
int kmscon_char_append_u8(struct kmscon_char *ch, const char *str, size_t len);

/* font objects with cached glyphs */

int kmscon_font_new(struct kmscon_font **out, uint32_t height);
void kmscon_font_ref(struct kmscon_font *font);
void kmscon_font_unref(struct kmscon_font *font);

int kmscon_font_draw(struct kmscon_font *font, const struct kmscon_char *ch,
					cairo_t *cr, uint32_t x, uint32_t y);

/* console buffer with cell objects */

int kmscon_buffer_new(struct kmscon_buffer **out, uint32_t x, uint32_t y);
void kmscon_buffer_ref(struct kmscon_buffer *buf);
void kmscon_buffer_unref(struct kmscon_buffer *buf);

int kmscon_buffer_resize(struct kmscon_buffer *buf, uint32_t x, uint32_t y);
void kmscon_buffer_draw(struct kmscon_buffer *buf, struct kmscon_font *font,
			void *dcr, unsigned int width, unsigned int height);

/* console objects */

int kmscon_console_new(struct kmscon_console **out);
void kmscon_console_ref(struct kmscon_console *con);
void kmscon_console_unref(struct kmscon_console *con);

int kmscon_console_set_res(struct kmscon_console *con, uint32_t x, uint32_t y);
void kmscon_console_draw(struct kmscon_console *con);
void kmscon_console_map(struct kmscon_console *con);
