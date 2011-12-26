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

/*
 * TODO: Avoid using this hack and instead retrieve GL extension
 * pointers dynamically on initialization.
 */
#define GL_GLEXT_PROTOTYPES

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "console.h"
#include "log.h"

struct kmscon_console {
	size_t ref;

	/* GL texture and font */
	GLuint tex;
	unsigned int res_x;
	unsigned int res_y;
	struct kmscon_font *font;

	/* cairo surface */
	cairo_t *cr;
	cairo_surface_t *surf;
	unsigned char *surf_buf;

	/* console cells */
	struct kmscon_buffer *cells;
	unsigned int cells_x;
	unsigned int cells_y;

	/* cursor */
	unsigned int cursor_x;
	unsigned int cursor_y;
};

static void kmscon_console_free_res(struct kmscon_console *con)
{
	if (con && con->cr) {
		glDeleteTextures(1, &con->tex);
		cairo_destroy(con->cr);
		cairo_surface_destroy(con->surf);
		free(con->surf_buf);
		con->tex = 0;
		con->cr = NULL;
		con->surf = NULL;
		con->surf_buf = NULL;
	}
}

static int kmscon_console_new_res(struct kmscon_console *con)
{
	unsigned char *buf;
	cairo_t *cr;
	cairo_surface_t *surface;
	int stride, ret;
	cairo_format_t format = CAIRO_FORMAT_ARGB32;

	if (!con)
		return -EINVAL;

	stride = cairo_format_stride_for_width(format, con->res_x);

	buf = malloc(stride * con->res_y);
	if (!buf)
		return -ENOMEM;

	surface = cairo_image_surface_create_for_data(buf, format, con->res_x,
							con->res_y, stride);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		ret = -ENOMEM;
		goto err_free;
	}

	cr = cairo_create(surface);
	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
		ret = -EFAULT;
		goto err_cairo;
	}

	if (con->cr) {
		glDeleteTextures(1, &con->tex);
		con->tex = 0;
		cairo_destroy(con->cr);
		cairo_surface_destroy(con->surf);
		free(con->surf_buf);
	}

	con->surf_buf = buf;
	con->surf = surface;
	con->cr = cr;

	glGenTextures(1, &con->tex);
	glBindTexture(GL_TEXTURE_RECTANGLE, con->tex);
	glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, con->res_x, con->res_y,
				0, GL_BGRA, GL_UNSIGNED_BYTE, con->surf_buf);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	log_debug("console: new resolution %ux%u\n", con->res_x, con->res_y);
	return 0;

err_cairo:
	cairo_destroy(cr);
err_free:
	cairo_surface_destroy(surface);
	free(buf);
	return ret;
}

int kmscon_console_new(struct kmscon_console **out)
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
	log_debug("console: new console\n");

	ret = kmscon_buffer_new(&con->cells, 0, 0);
	if (ret)
		goto err_free;

	con->cells_x = kmscon_buffer_get_width(con->cells);
	con->cells_y = kmscon_buffer_get_height(con->cells);

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

	kmscon_console_free_res(con);
	kmscon_font_unref(con->font);
	kmscon_buffer_unref(con->cells);
	free(con);
	log_debug("console: destroing console\n");
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

	ret = kmscon_font_new(&font, height / con->cells_y);
	if (ret) {
		log_err("console: cannot create new font: %d\n", ret);
		return ret;
	}

	kmscon_font_unref(con->font);
	con->font = font;
	con->res_x = con->cells_x * kmscon_font_get_width(con->font);
	con->res_y = height;

	ret = kmscon_console_new_res(con);
	if (ret) {
		log_err("console: cannot create drawing buffers: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * This redraws the console. It does not clip/copy the image onto any
 * framebuffer. You must use kmscon_console_map() to do this.
 * This allows to draw the console once and then map it onto multiple
 * framebuffers so it is displayed on multiple monitors with different screen
 * resolutions.
 * You must have called kmscon_console_set_res() before.
 */
void kmscon_console_draw(struct kmscon_console *con)
{
	if (!con || !con->cr)
		return;

	cairo_save(con->cr);

	cairo_set_operator(con->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(con->cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(con->cr);

	cairo_set_operator(con->cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(con->cr, 1.0, 1.0, 1.0, 1.0);

	kmscon_buffer_draw(con->cells, con->font, con->cr, con->res_x,
								con->res_y);

	cairo_restore(con->cr);

	/* refresh GL texture contents */
	glBindTexture(GL_TEXTURE_RECTANGLE, con->tex);
	glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, con->res_x, con->res_y,
				0, GL_BGRA, GL_UNSIGNED_BYTE, con->surf_buf);
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
	if (!con || !con->cr)
		return;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_RECTANGLE);
	glBindTexture(GL_TEXTURE_RECTANGLE, con->tex);

	glBegin(GL_QUADS);
		glColor4f(0.0f, 0.0f, 0.0f, 0.0f);

		glTexCoord2f(0.0f, 0.0f);
		glVertex2f(-1.0f, -1.0f);

		glTexCoord2f(con->res_x, 0.0f);
		glVertex2f(1.0f, -1.0f);

		glTexCoord2f(con->res_x, con->res_y);
		glVertex2f(1.0f, 1.0f);

		glTexCoord2f(0.0f, con->res_y);
		glVertex2f(-1.0f, 1.0f);
	glEnd();
}

void kmscon_console_write(struct kmscon_console *con,
						const struct kmscon_char *ch)
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
