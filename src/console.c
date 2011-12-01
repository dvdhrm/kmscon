/*
 * kmscon - Console Management
 * Written 2011 by David Herrmann <dh.herrmann@googlemail.com>
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

struct kmscon_cell {
	struct kmscon_char *ch;
};

struct kmscon_console {
	size_t ref;

	/* GL texture */
	GLuint tex;
	uint32_t res_x;
	uint32_t res_y;

	/* cairo surface */
	cairo_t *cr;
	cairo_surface_t *surf;
	unsigned char *surf_buf;

	/* console cells */
	uint32_t lines_x;
	uint32_t lines_y;
	struct kmscon_cell *cells;
	bool cells_dirty;

	/* cursor position */
	uint32_t cursor_x;
	uint32_t cursor_y;

	/* active font */
	struct kmscon_font *font;
};

static void kmscon_console_free_res(struct kmscon_console *con)
{
	if (con && con->cr) {
		cairo_destroy(con->cr);
		cairo_surface_destroy(con->surf);
		free(con->surf_buf);
		con->cr = NULL;
		con->surf = NULL;
		con->surf_buf = NULL;
	}
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
	con->cells_dirty = true;

	ret = kmscon_console_set_res(con, 800, 600);
	if (ret)
		goto err_free;

	ret = kmscon_console_resize(con, 80, 24);
	if (ret)
		goto err_res;

	glGenTextures(1, &con->tex);

	*out = con;
	return 0;

err_res:
	kmscon_console_free_res(con);
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

static void console_free_cells(struct kmscon_console *con)
{
	uint32_t i, size;

	if (con->cells) {
		size = con->lines_x * con->lines_y;

		for (i = 0; i < size; ++i)
			kmscon_char_free(con->cells[i].ch);

		free(con->cells);
	}
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
	console_free_cells(con);
	glDeleteTextures(1, &con->tex);
	free(con);
}

/*
 * This resets the resolution used for drawing operations. It is recommended to
 * set this to the size of your framebuffer, however, you can set this to
 * anything except 0.
 * This image-resolution is used internally to render the console fonts. The
 * kmscon_console_map() function can map this image to any framebuffer size you
 * want. Therefore, this screen resolution is just a performance and quality
 * hint.
 * By default this is set to 800x600.
 * Returns 0 on success -EINVAL if con, x or y is 0/NULL and -ENOMEM on
 * out-of-mem errors.
 */
int kmscon_console_set_res(struct kmscon_console *con, uint32_t x, uint32_t y)
{
	unsigned char *buf;
	cairo_t *cr;
	cairo_surface_t *surface;
	int stride, ret;
	cairo_format_t format = CAIRO_FORMAT_ARGB32;

	if (!con || !x || !y)
		return -EINVAL;

	stride = cairo_format_stride_for_width(format, x);

	buf = malloc(stride * y);
	if (!buf)
		return -ENOMEM;

	surface = cairo_image_surface_create_for_data(buf, format, x, y,
								stride);
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
		cairo_destroy(con->cr);
		cairo_surface_destroy(con->surf);
		free(con->surf_buf);
	}

	con->res_x = x;
	con->res_y = y;
	con->surf_buf = buf;
	con->surf = surface;
	con->cr = cr;

	glBindTexture(GL_TEXTURE_RECTANGLE, con->tex);
	glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, con->res_x, con->res_y,
				0, GL_BGRA, GL_UNSIGNED_BYTE, con->surf_buf);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	return 0;

err_cairo:
	cairo_destroy(cr);
err_free:
	cairo_surface_destroy(surface);
	free(buf);
	return ret;
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
	size_t i, j, pos;
	double xs, ys, x, y;

	if (!con || !con->cr)
		return;

	cairo_save(con->cr);

	cairo_set_operator(con->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(con->cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(con->cr);

	cairo_set_operator(con->cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgba(con->cr, 1.0, 1.0, 1.0, 1.0);

	xs = con->res_x / (double)con->lines_x;
	ys = con->res_y / (double)con->lines_y;

	y = 0;
	for (i = 0; i < con->lines_y; ++i) {
		x = 0;
		for (j = 0; j < con->lines_x; ++j) {
			pos = i * con->lines_x + j;
			kmscon_font_draw(con->font, con->cells[pos].ch, con->cr,
									x, y);
			x += xs;
		}
		y += ys;
	}

	cairo_restore(con->cr);

	/* refresh GL texture contents */
	glBindTexture(GL_TEXTURE_RECTANGLE, con->tex);
	glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA, con->res_x, con->res_y,
				0, GL_BGRA, GL_UNSIGNED_BYTE, con->surf_buf);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	/* reset dirty flags */
	con->cells_dirty = false;
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

/*
 * Resize console. x/y must not be 0.
 * This resizes the whole console buffer and recreates all cells. It tries to
 * preserve as many content from the previous buffer as possible.
 */
int kmscon_console_resize(struct kmscon_console *con, uint32_t x, uint32_t y)
{
	struct kmscon_cell *cells;
	struct kmscon_font *font;
	uint32_t size, i, j;
	int ret;

	size = x * y;
	if (!con || !size || size < x || size < y)
		return -EINVAL;

	ret = kmscon_font_new(&font, con->res_y / y);
	if (ret)
		return ret;

	cells = malloc(sizeof(*cells) * size);
	if (!cells) {
		ret = -ENOMEM;
		goto err_font;
	}

	memset(cells, 0, sizeof(*cells) * size);

	for (i = 0; i < size; ++i) {
		ret = kmscon_char_new(&cells[i].ch);
		if (ret) {
			for (j = 0; j < i; ++j)
				kmscon_char_free(cells[j].ch);
			goto err_free;
		}
		kmscon_char_set_u8(cells[i].ch, "?", 1);
	}

	kmscon_font_unref(con->font);
	con->font = font;

	console_free_cells(con);
	con->lines_x = x;
	con->lines_y = y;
	con->cells = cells;

	return 0;

err_free:
	free(cells);
err_font:
	kmscon_font_unref(font);
	return ret;
}

void kmscon_console_cursor_get(struct kmscon_console *con, uint32_t *x,
								uint32_t *y)
{
	if (!con) {
		if (x)
			*x = 0;
		if (y)
			*y = 0;
		return;
	}

	if (x)
		*x = con->cursor_x;

	if (y)
		*y = con->cursor_y;
}

void kmscon_console_cursor_move(struct kmscon_console *con, int32_t x,
								int32_t y)
{
	int32_t tx, ty;

	if (!con)
		return;

	tx = con->cursor_x;
	ty = con->cursor_y;

	tx += x;
	ty += y;

	if (tx < 0)
		tx = 0;
	if (ty < 0)
		ty = 0;

	while (tx >= con->lines_x) {
		tx -= con->lines_x;
		ty++;
	}

	if (ty >= con->lines_y)
		ty = con->lines_y - 1;

	con->cursor_x += tx;
	con->cursor_y += ty;
	con->cells_dirty = true;
}

void kmscon_console_cursor_goto(struct kmscon_console *con, uint32_t x,
								uint32_t y)
{
	if (!con)
		return;

	con->cursor_x = x;
	con->cursor_y = y;

	while (con->cursor_x >= con->lines_x) {
		con->cursor_x -= con->lines_x;
		con->cursor_y++;
	}

	if (con->cursor_y >= con->lines_y)
		con->cursor_y = con->lines_y - 1;

	con->cells_dirty = true;
}

int kmscon_console_write(struct kmscon_console *con,
						const struct kmscon_char *ch)
{
	int ret;
	uint32_t pos;

	if (!con || !ch)
		return -EINVAL;

	pos = con->cursor_y * con->lines_x + con->cursor_x;
	ret = kmscon_char_set(con->cells[pos].ch, ch);
	if (ret)
		return ret;

	kmscon_console_cursor_move(con, 1, 0);
	con->cells_dirty = true;

	return 0;
}
