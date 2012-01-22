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
#include "log.h"
#include "output.h"
#include "unicode.h"

struct kmscon_console {
	size_t ref;
	struct kmscon_font_factory *ff;
	struct kmscon_compositor *comp;
	struct kmscon_context *ctx;

	/* font */
	unsigned int res_x;
	unsigned int res_y;
	struct kmscon_font *font;

	/* console cells */
	struct kmscon_buffer *cells;
	unsigned int cells_x;
	unsigned int cells_y;

	/* cursor */
	unsigned int cursor_x;
	unsigned int cursor_y;
};

int kmscon_console_new(struct kmscon_console **out,
		struct kmscon_font_factory *ff, struct kmscon_compositor *comp)
{
	struct kmscon_console *con;
	int ret;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	con->ref = 1;
	con->ff = ff;
	con->comp = comp;
	con->ctx = kmscon_compositor_get_context(comp);
	log_debug("console: new console\n");

	ret = kmscon_buffer_new(&con->cells, 0, 0);
	if (ret)
		goto err_free;

	con->cells_x = kmscon_buffer_get_width(con->cells);
	con->cells_y = kmscon_buffer_get_height(con->cells);

	kmscon_font_factory_ref(con->ff);
	kmscon_compositor_ref(con->comp);
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

	kmscon_font_unref(con->font);
	kmscon_buffer_unref(con->cells);
	kmscon_compositor_unref(con->comp);
	kmscon_font_factory_unref(con->ff);
	free(con);
	log_debug("console: destroying console\n");
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

	log_debug("console: resizing to %ux%u:%u\n", x, y, height);

	ret = kmscon_buffer_resize(con->cells, x, y);
	if (ret)
		return ret;

	con->cells_x = kmscon_buffer_get_width(con->cells);
	con->cells_y = kmscon_buffer_get_height(con->cells);

	if (con->cursor_x > con->cells_x)
		con->cursor_x = con->cells_x;
	if (con->cursor_y > con->cells_y)
		con->cursor_y = con->cells_y;

	ret = kmscon_font_factory_load(con->ff, &font, 0,
							height / con->cells_y);
	if (ret) {
		log_err("console: cannot create new font: %d\n", ret);
		return ret;
	}

	kmscon_font_unref(con->font);
	con->font = font;
	con->res_x = con->cells_x * kmscon_font_get_width(con->font);
	con->res_y = height;
	log_debug("console: new resolution %ux%u\n", con->res_x, con->res_y);

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
void kmscon_console_map(struct kmscon_console *con)
{
	if (!con)
		return;

	kmscon_buffer_draw(con->cells, con->font);
}

void kmscon_console_write(struct kmscon_console *con, kmscon_symbol_t ch)
{
	if (!con)
		return;

	if (con->cursor_x >= con->cells_x) {
		con->cursor_x = 0;
		con->cursor_y++;
		if (con->cursor_y >= con->cells_y) {
			con->cursor_y--;
			kmscon_buffer_rotate(con->cells);
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
	if (con->cursor_y >= con->cells_y) {
		con->cursor_y--;
		kmscon_buffer_rotate(con->cells);
	}
}
