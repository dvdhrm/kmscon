/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * DRM Video backend
 */

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "eloop.h"
#include "shl_log.h"
#include "static_gl.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_drm3d_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "uterm_drm3d_render"

extern const char *gl_static_fill_vert;
extern const char *gl_static_fill_frag;
extern const char *gl_static_blend_vert;
extern const char *gl_static_blend_frag;
extern const char *gl_static_blit_vert;
extern const char *gl_static_blit_frag;

static int init_shaders(struct uterm_video *video)
{
	struct uterm_drm3d_video *v3d = uterm_drm_video_get_data(video);
	int ret;
	char *fill_attr[] = { "position", "color" };
	char *blend_attr[] = { "position", "texture_position" };
	char *blit_attr[] = { "position", "texture_position" };

	if (v3d->sinit == 1)
		return -EFAULT;
	else if (v3d->sinit == 2)
		return 0;

	v3d->sinit = 1;

	ret = gl_shader_new(&v3d->fill_shader, gl_static_fill_vert,
			    gl_static_fill_frag, fill_attr, 2, log_llog, NULL);
	if (ret)
		return ret;

	v3d->uni_fill_proj = gl_shader_get_uniform(v3d->fill_shader,
						   "projection");

	ret = gl_shader_new(&v3d->blend_shader, gl_static_blend_vert,
			    gl_static_blend_frag, blend_attr, 2, log_llog,
			    NULL);
	if (ret)
		return ret;

	v3d->uni_blend_proj = gl_shader_get_uniform(v3d->blend_shader,
						    "projection");
	v3d->uni_blend_tex = gl_shader_get_uniform(v3d->blend_shader,
						   "texture");
	v3d->uni_blend_fgcol = gl_shader_get_uniform(v3d->blend_shader,
						     "fgcolor");
	v3d->uni_blend_bgcol = gl_shader_get_uniform(v3d->blend_shader,
						     "bgcolor");

	ret = gl_shader_new(&v3d->blit_shader, gl_static_blit_vert,
			    gl_static_blit_frag, blit_attr, 2, log_llog, NULL);
	if (ret)
		return ret;

	v3d->uni_blit_proj = gl_shader_get_uniform(v3d->blit_shader,
						   "projection");
	v3d->uni_blit_tex = gl_shader_get_uniform(v3d->blit_shader,
						  "texture");

	gl_tex_new(&v3d->tex, 1);
	v3d->sinit = 2;

	return 0;
}

void uterm_drm3d_deinit_shaders(struct uterm_video *video)
{
	struct uterm_drm3d_video *v3d = uterm_drm_video_get_data(video);

	if (v3d->sinit == 0)
		return;

	v3d->sinit = 0;
	gl_tex_free(&v3d->tex, 1);
	gl_shader_unref(v3d->blit_shader);
	gl_shader_unref(v3d->blend_shader);
	gl_shader_unref(v3d->fill_shader);
}

int uterm_drm3d_display_blit(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y)
{
	struct uterm_drm3d_video *v3d;
	unsigned int sw, sh, tmp, width, height, i;
	float mat[16];
	float vertices[6 * 2], texpos[6 * 2];
	int ret;
	uint8_t *packed, *src, *dst;

	if (!buf || buf->format != UTERM_FORMAT_XRGB32)
		return -EINVAL;

	v3d = uterm_drm_video_get_data(disp->video);
	ret = uterm_drm3d_display_use(disp, NULL);
	if (ret)
		return ret;
	ret = init_shaders(disp->video);
	if (ret)
		return ret;

	sw = uterm_drm_mode_get_width(disp->current_mode);
	sh = uterm_drm_mode_get_height(disp->current_mode);

	vertices[0] = -1.0;
	vertices[1] = -1.0;
	vertices[2] = -1.0;
	vertices[3] = +1.0;
	vertices[4] = +1.0;
	vertices[5] = +1.0;

	vertices[6] = -1.0;
	vertices[7] = -1.0;
	vertices[8] = +1.0;
	vertices[9] = +1.0;
	vertices[10] = +1.0;
	vertices[11] = -1.0;

	texpos[0] = 0.0;
	texpos[1] = 1.0;
	texpos[2] = 0.0;
	texpos[3] = 0.0;
	texpos[4] = 1.0;
	texpos[5] = 0.0;

	texpos[6] = 0.0;
	texpos[7] = 1.0;
	texpos[8] = 1.0;
	texpos[9] = 0.0;
	texpos[10] = 1.0;
	texpos[11] = 1.0;

	tmp = x + buf->width;
	if (tmp < x || x >= sw)
		return -EINVAL;
	if (tmp > sw)
		width = sw - x;
	else
		width = buf->width;

	tmp = y + buf->height;
	if (tmp < y || y >= sh)
		return -EINVAL;
	if (tmp > sh)
		height = sh - y;
	else
		height = buf->height;

	glViewport(x, sh - y - height, width, height);
	glDisable(GL_BLEND);

	gl_shader_use(v3d->blit_shader);

	gl_m4_identity(mat);
	glUniformMatrix4fv(v3d->uni_blit_proj, 1, GL_FALSE, mat);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, v3d->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	if (v3d->supports_rowlen) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, buf->stride / 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf->data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	} else if (buf->stride == width) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf->data);
	} else {
		packed = malloc(width * height);
		if (!packed)
			return -ENOMEM;

		src = buf->data;
		dst = packed;
		for (i = 0; i < height; ++i) {
			memcpy(dst, src, width * 4);
			dst += width * 4;
			src += buf->stride;
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, packed);

		free(packed);
	}

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glUniform1i(v3d->uni_blit_tex, 0);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texpos);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	if (gl_has_error(v3d->blit_shader)) {
		log_warning("GL error");
		return -EFAULT;
	}

	return 0;
}

static int display_blend(struct uterm_display *disp,
			 const struct uterm_video_buffer *buf,
			 unsigned int x, unsigned int y,
			 uint8_t fr, uint8_t fg, uint8_t fb,
			 uint8_t br, uint8_t bg, uint8_t bb)
{
	struct uterm_drm3d_video *v3d;
	unsigned int sw, sh, tmp, width, height, i;
	float mat[16];
	float vertices[6 * 2], texpos[6 * 2], fgcol[3], bgcol[3];
	int ret;
	uint8_t *packed, *src, *dst;

	if (!buf || buf->format != UTERM_FORMAT_GREY)
		return -EINVAL;

	v3d = uterm_drm_video_get_data(disp->video);
	ret = uterm_drm3d_display_use(disp, NULL);
	if (ret)
		return ret;
	ret = init_shaders(disp->video);
	if (ret)
		return ret;

	sw = uterm_drm_mode_get_width(disp->current_mode);
	sh = uterm_drm_mode_get_height(disp->current_mode);

	vertices[0] = -1.0;
	vertices[1] = -1.0;
	vertices[2] = -1.0;
	vertices[3] = +1.0;
	vertices[4] = +1.0;
	vertices[5] = +1.0;

	vertices[6] = -1.0;
	vertices[7] = -1.0;
	vertices[8] = +1.0;
	vertices[9] = +1.0;
	vertices[10] = +1.0;
	vertices[11] = -1.0;

	texpos[0] = 0.0;
	texpos[1] = 1.0;
	texpos[2] = 0.0;
	texpos[3] = 0.0;
	texpos[4] = 1.0;
	texpos[5] = 0.0;

	texpos[6] = 0.0;
	texpos[7] = 1.0;
	texpos[8] = 1.0;
	texpos[9] = 0.0;
	texpos[10] = 1.0;
	texpos[11] = 1.0;

	fgcol[0] = fr / 255.0;
	fgcol[1] = fg / 255.0;
	fgcol[2] = fb / 255.0;
	bgcol[0] = br / 255.0;
	bgcol[1] = bg / 255.0;
	bgcol[2] = bb / 255.0;

	tmp = x + buf->width;
	if (tmp < x || x >= sw)
		return -EINVAL;
	if (tmp > sw)
		width = sw - x;
	else
		width = buf->width;

	tmp = y + buf->height;
	if (tmp < y || y >= sh)
		return -EINVAL;
	if (tmp > sh)
		height = sh - y;
	else
		height = buf->height;

	glViewport(x, sh - y - height, width, height);
	glDisable(GL_BLEND);

	gl_shader_use(v3d->blend_shader);

	gl_m4_identity(mat);
	glUniformMatrix4fv(v3d->uni_blend_proj, 1, GL_FALSE, mat);

	glUniform3fv(v3d->uni_blend_fgcol, 1, fgcol);
	glUniform3fv(v3d->uni_blend_bgcol, 1, bgcol);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, v3d->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	if (v3d->supports_rowlen) {
		glPixelStorei(GL_UNPACK_ROW_LENGTH, buf->stride);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0,
			     GL_ALPHA, GL_UNSIGNED_BYTE, buf->data);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	} else if (buf->stride == width) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0,
			     GL_ALPHA, GL_UNSIGNED_BYTE, buf->data);
	} else {
		packed = malloc(width * height);
		if (!packed)
			return -ENOMEM;

		src = buf->data;
		dst = packed;
		for (i = 0; i < height; ++i) {
			memcpy(dst, src, width);
			dst += width;
			src += buf->stride;
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, width, height, 0,
			     GL_ALPHA, GL_UNSIGNED_BYTE, packed);

		free(packed);
	}

	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glUniform1i(v3d->uni_blend_tex, 0);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texpos);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	if (gl_has_error(v3d->blend_shader)) {
		log_warning("GL error");
		return -EFAULT;
	}

	return 0;
}

int uterm_drm3d_display_fake_blendv(struct uterm_display *disp,
				    const struct uterm_video_blend_req *req,
				    size_t num)
{
	int ret;
	unsigned int i;

	if (!disp || !req)
		return -EINVAL;

	for (i = 0; i < num; ++i, ++req) {
		if (!req->buf)
			continue;

		ret = display_blend(disp, req->buf, req->x, req->y,
				    req->fr, req->fg, req->fb,
				    req->br, req->bg, req->bb);
		if (ret)
			return ret;
	}

	return 0;
}

int uterm_drm3d_display_fill(struct uterm_display *disp,
			     uint8_t r, uint8_t g, uint8_t b,
			     unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height)
{
	struct uterm_drm3d_video *v3d;
	unsigned int sw, sh, tmp, i;
	float mat[16];
	float vertices[6 * 2], colors[6 * 4];
	int ret;

	v3d = uterm_drm_video_get_data(disp->video);
	ret = uterm_drm3d_display_use(disp, NULL);
	if (ret)
		return ret;
	ret = init_shaders(disp->video);
	if (ret)
		return ret;

	sw = uterm_drm_mode_get_width(disp->current_mode);
	sh = uterm_drm_mode_get_height(disp->current_mode);

	for (i = 0; i < 6; ++i) {
		colors[i * 4 + 0] = r / 255.0;
		colors[i * 4 + 1] = g / 255.0;
		colors[i * 4 + 2] = b / 255.0;
		colors[i * 4 + 3] = 1.0;
	}

	vertices[0] = -1.0;
	vertices[1] = -1.0;
	vertices[2] = -1.0;
	vertices[3] = +1.0;
	vertices[4] = +1.0;
	vertices[5] = +1.0;

	vertices[6] = -1.0;
	vertices[7] = -1.0;
	vertices[8] = +1.0;
	vertices[9] = +1.0;
	vertices[10] = +1.0;
	vertices[11] = -1.0;

	tmp = x + width;
	if (tmp < x || x >= sw)
		return -EINVAL;
	if (tmp > sw)
		width = sw - x;
	tmp = y + height;
	if (tmp < y || y >= sh)
		return -EINVAL;
	if (tmp > sh)
		height = sh - y;

	glViewport(x, y, width, height);
	glDisable(GL_BLEND);

	gl_shader_use(v3d->fill_shader);
	gl_m4_identity(mat);
	glUniformMatrix4fv(v3d->uni_fill_proj, 1, GL_FALSE, mat);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	if (gl_has_error(v3d->fill_shader)) {
		log_warning("GL error");
		return -EFAULT;
	}

	return 0;
}
