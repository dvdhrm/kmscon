/*
 * uterm - Linux User-Space Terminal drm3d module
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

/* Internal definitions */

#ifndef UTERM_DRM3D_INTERNAL_H
#define UTERM_DRM3D_INTERNAL_H

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
#include "uterm_video.h"

/* thanks khronos for breaking backwards compatibility.. */
#if !defined(GL_UNPACK_ROW_LENGTH) && defined(GL_UNPACK_ROW_LENGTH_EXT)
#  define GL_UNPACK_ROW_LENGTH GL_UNPACK_ROW_LENGTH_EXT
#endif

struct uterm_drm3d_rb {
	struct uterm_display *disp;
	struct gbm_bo *bo;
	uint32_t fb;
};

struct uterm_drm3d_display {
	struct gbm_surface *gbm;
	EGLSurface surface;
	struct uterm_drm3d_rb *current;
	struct uterm_drm3d_rb *next;
};

struct uterm_drm3d_video {
	struct gbm_device *gbm;
	EGLDisplay disp;
	EGLConfig conf;
	EGLContext ctx;

	unsigned int sinit;
	bool supports_rowlen;
	GLuint tex;

	struct gl_shader *fill_shader;
	GLuint uni_fill_proj;

	struct gl_shader *blend_shader;
	GLuint uni_blend_proj;
	GLuint uni_blend_tex;
	GLuint uni_blend_fgcol;
	GLuint uni_blend_bgcol;

	struct gl_shader *blit_shader;
	GLuint uni_blit_proj;
	GLuint uni_blit_tex;
};

int uterm_drm3d_display_use(struct uterm_display *disp, bool *opengl);
void uterm_drm3d_deinit_shaders(struct uterm_video *video);
int uterm_drm3d_display_blit(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y);
int uterm_drm3d_display_fake_blendv(struct uterm_display *disp,
				    const struct uterm_video_blend_req *req,
				    size_t num);
int uterm_drm3d_display_fill(struct uterm_display *disp,
			     uint8_t r, uint8_t g, uint8_t b,
			     unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height);

#endif /* UTERM_DRM3D_INTERNAL_H */
