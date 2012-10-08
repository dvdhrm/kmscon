/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
#include "log.h"
#include "uterm.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "video_drm"

static const char *mode_get_name(const struct uterm_mode *mode)
{
	return mode->drm.info.name;
}

static unsigned int mode_get_width(const struct uterm_mode *mode)
{
	return mode->drm.info.hdisplay;
}

static unsigned int mode_get_height(const struct uterm_mode *mode)
{
	return mode->drm.info.vdisplay;
}

static int init_rb(struct uterm_display *disp, struct drm_rb *rb)
{
	unsigned int stride, handle;
	int ret;
	struct uterm_video *video = disp->video;

	rb->bo = gbm_bo_create(video->drm.gbm,
				disp->current_mode->drm.info.hdisplay,
				disp->current_mode->drm.info.vdisplay,
				GBM_BO_FORMAT_XRGB8888,
				GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!rb->bo) {
		log_err("cannot create gbm-bo");
		return -EFAULT;
	}

#ifdef BUILD_HAVE_GBM_BO_GET_PITCH
	stride = gbm_bo_get_pitch(rb->bo);
#else
	stride = gbm_bo_get_stride(rb->bo);
#endif
	handle = gbm_bo_get_handle(rb->bo).u32;

	/* TODO: how can we choose 24/32 dynamically? */
	ret = drmModeAddFB(video->drm.fd,
					disp->current_mode->drm.info.hdisplay,
					disp->current_mode->drm.info.vdisplay,
					24, 32, stride, handle, &rb->fb);
	if (ret) {
		log_err("cannot add drm-fb");
		ret = -EFAULT;
		goto err_gbm;
	}

	rb->image = eglCreateImageKHR(video->drm.disp, NULL,
					EGL_NATIVE_PIXMAP_KHR, rb->bo, NULL);
	if (!rb->image) {
		log_err("cannot create egl image");
		ret = -EFAULT;
		goto err_fb;
	}

	glGenRenderbuffers(1, &rb->rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb->rb);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, rb->image);

	return 0;

err_fb:
	drmModeRmFB(video->drm.fd, rb->fb);
err_gbm:
	gbm_bo_destroy(rb->bo);
	return ret;
}

static void destroy_rb(struct uterm_display *disp, struct drm_rb *rb)
{
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glDeleteRenderbuffers(1, &rb->rb);
	eglDestroyImageKHR(disp->video->drm.disp, rb->image);
	drmModeRmFB(disp->video->drm.fd, rb->fb);
	gbm_bo_destroy(rb->bo);
}

static int find_crtc(struct uterm_video *video, drmModeRes *res,
							drmModeEncoder *enc)
{
	int i, crtc;
	struct uterm_display *iter;

	for (i = 0; i < res->count_crtcs; ++i) {
		if (enc->possible_crtcs & (1 << i)) {
			crtc = res->crtcs[i];
			for (iter = video->displays; iter; iter = iter->next) {
				if (iter->drm.crtc_id == crtc)
					break;
			}
			if (!iter)
				return crtc;
		}
	}

	return -1;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	struct uterm_video *video = disp->video;
	int ret, crtc, i;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;

	if (!video || !video_is_awake(video) || !mode)
		return -EINVAL;
	if (display_is_online(disp))
		return -EINVAL;

	ret = video_do_use(disp->video);
	if (ret)
		return ret;

	log_info("activating display %p to %ux%u", disp,
			mode->drm.info.hdisplay, mode->drm.info.vdisplay);

	res = drmModeGetResources(video->drm.fd);
	if (!res) {
		log_err("cannot get resources for display %p", disp);
		return -EFAULT;
	}
	conn = drmModeGetConnector(video->drm.fd, disp->drm.conn_id);
	if (!conn) {
		log_err("cannot get connector for display %p", disp);
		drmModeFreeResources(res);
		return -EFAULT;
	}

	crtc = -1;
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(video->drm.fd, conn->encoders[i]);
		if (!enc)
			continue;
		crtc = find_crtc(video, res, enc);
		drmModeFreeEncoder(enc);
		if (crtc >= 0)
			break;
	}

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);

	if (crtc < 0) {
		log_warn("cannot find crtc for new display");
		return -ENODEV;
	}

	disp->drm.crtc_id = crtc;
	disp->drm.current_rb = 0;
	disp->current_mode = mode;
	disp->drm.saved_crtc = drmModeGetCrtc(video->drm.fd,
							disp->drm.crtc_id);

	ret = init_rb(disp, &disp->drm.rb[0]);
	if (ret)
		goto err_saved;

	ret = init_rb(disp, &disp->drm.rb[1]);
	if (ret)
		goto err_rb;

	glGenFramebuffers(1, &disp->drm.fb);
	glBindFramebuffer(GL_FRAMEBUFFER, disp->drm.fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					GL_RENDERBUFFER, disp->drm.rb[1].rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
						GL_FRAMEBUFFER_COMPLETE) {
		log_err("cannot create gl-framebuffer");
		ret = -EFAULT;
		goto err_fb;
	}

	ret = drmModeSetCrtc(video->drm.fd, disp->drm.crtc_id,
			disp->drm.rb[0].fb, 0, 0, &disp->drm.conn_id, 1,
						&disp->current_mode->drm.info);
	if (ret) {
		log_err("cannot set drm-crtc");
		ret = -EFAULT;
		goto err_fb;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_fb:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &disp->drm.fb);
	destroy_rb(disp, &disp->drm.rb[1]);
err_rb:
	destroy_rb(disp, &disp->drm.rb[0]);
err_saved:
	disp->current_mode = NULL;
	if (disp->drm.saved_crtc) {
		drmModeFreeCrtc(disp->drm.saved_crtc);
		disp->drm.saved_crtc = NULL;
	}
	return ret;
}

static void display_deactivate(struct uterm_display *disp)
{
	int ret;

	if (!display_is_online(disp))
		return;

	if (disp->drm.saved_crtc) {
		if (disp->video->flags & VIDEO_AWAKE) {
			drmModeSetCrtc(disp->video->drm.fd,
					disp->drm.saved_crtc->crtc_id,
					disp->drm.saved_crtc->buffer_id,
					disp->drm.saved_crtc->x,
					disp->drm.saved_crtc->y,
					&disp->drm.conn_id,
					1,
					&disp->drm.saved_crtc->mode);
		}
		drmModeFreeCrtc(disp->drm.saved_crtc);
		disp->drm.saved_crtc = NULL;
	}

	ret = video_do_use(disp->video);
	if (ret)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &disp->drm.fb);
	destroy_rb(disp, &disp->drm.rb[1]);
	destroy_rb(disp, &disp->drm.rb[0]);
	disp->current_mode = NULL;
	disp->flags &= ~(DISPLAY_ONLINE | DISPLAY_VSYNC);
	log_info("deactivating display %p", disp);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int i, ret, set;
	drmModeConnector *conn;
	drmModePropertyRes *prop;

	if (!display_is_conn(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	switch (state) {
	case UTERM_DPMS_ON:
		set = DRM_MODE_DPMS_ON;
		break;
	case UTERM_DPMS_STANDBY:
		set = DRM_MODE_DPMS_STANDBY;
		break;
	case UTERM_DPMS_SUSPEND:
		set = DRM_MODE_DPMS_SUSPEND;
		break;
	case UTERM_DPMS_OFF:
		set = DRM_MODE_DPMS_OFF;
		break;
	default:
		return -EINVAL;
	}

	log_info("setting DPMS of display %p to %s", disp,
			uterm_dpms_to_name(state));

	conn = drmModeGetConnector(disp->video->drm.fd, disp->drm.conn_id);
	if (!conn) {
		log_err("cannot get display connector");
		return -EFAULT;
	}

	ret = 0;
	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(disp->video->drm.fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			ret = drmModeConnectorSetProperty(disp->video->drm.fd,
				disp->drm.conn_id, prop->prop_id, set);
			if (ret) {
				log_info("cannot set DPMS");
				ret = -EFAULT;
			}
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props) {
		ret = 0;
		log_warn("display does not support DPMS");
		state = UTERM_DPMS_UNKNOWN;
	}

	drmModeFreeConnector(conn);
	disp->dpms = state;
	return ret;
}

static int display_use(struct uterm_display *disp)
{
	int ret;

	if (!display_is_online(disp))
		return -EINVAL;

	ret = video_do_use(disp->video);
	if (ret)
		return ret;

	/* TODO: we need triple buffering as a VSYNC may still be pending */
	glBindFramebuffer(GL_FRAMEBUFFER, disp->drm.fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		GL_RENDERBUFFER, disp->drm.rb[disp->drm.current_rb ^ 1].rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
						GL_FRAMEBUFFER_COMPLETE) {
		log_warn("cannot set gl-renderbuffer");
		return -EFAULT;
	}

	return 0;
}

static int display_swap(struct uterm_display *disp)
{
	int ret;

	if (!display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;
	if (disp->dpms != UTERM_DPMS_ON)
		return -EINVAL;

	/* TODO: is glFlush sufficient here? */
	glFinish();

	errno = 0;
	disp->drm.current_rb ^= 1;
	ret = drmModePageFlip(disp->video->drm.fd, disp->drm.crtc_id,
				disp->drm.rb[disp->drm.current_rb].fb,
				DRM_MODE_PAGE_FLIP_EVENT, disp);
	if (ret) {
		log_warn("page-flip failed %d %d", ret, errno);
		return -EFAULT;
	}
	uterm_display_ref(disp);
	disp->flags |= DISPLAY_VSYNC;

	return 0;
}

extern const char *gl_static_fill_vert;
extern const char *gl_static_fill_frag;
extern const char *gl_static_blend_vert;
extern const char *gl_static_blend_frag;
extern const char *gl_static_blit_vert;
extern const char *gl_static_blit_frag;

static int init_shaders(struct uterm_video *video)
{
	int ret;
	char *fill_attr[] = { "position", "color" };
	char *blend_attr[] = { "position", "texture_position" };
	char *blit_attr[] = { "position", "texture_position" };

	if (video->drm.sinit == 1)
		return -EFAULT;
	else if (video->drm.sinit == 2)
		return 0;

	video->drm.sinit = 1;

	ret = gl_shader_new(&video->drm.fill_shader, gl_static_fill_vert,
			    gl_static_fill_frag, fill_attr, 2, log_llog);
	if (ret)
		return ret;

	video->drm.uni_fill_proj = gl_shader_get_uniform(
					video->drm.fill_shader,
					"projection");

	ret = gl_shader_new(&video->drm.blend_shader, gl_static_blend_vert,
			    gl_static_blend_frag, blend_attr, 2, log_llog);
	if (ret)
		return ret;

	video->drm.uni_blend_proj = gl_shader_get_uniform(
					video->drm.blend_shader,
					"projection");
	video->drm.uni_blend_tex = gl_shader_get_uniform(
					video->drm.blend_shader,
					"texture");
	video->drm.uni_blend_fgcol = gl_shader_get_uniform(
					video->drm.blend_shader,
					"fgcolor");
	video->drm.uni_blend_bgcol = gl_shader_get_uniform(
					video->drm.blend_shader,
					"bgcolor");

	ret = gl_shader_new(&video->drm.blit_shader, gl_static_blit_vert,
			    gl_static_blit_frag, blit_attr, 2, log_llog);
	if (ret)
		return ret;

	video->drm.uni_blit_proj = gl_shader_get_uniform(
					video->drm.blit_shader,
					"projection");
	video->drm.uni_blit_tex = gl_shader_get_uniform(
					video->drm.blit_shader,
					"texture");

	gl_tex_new(&video->drm.tex, 1);
	video->drm.sinit = 2;

	return 0;
}

static void deinit_shaders(struct uterm_video *video)
{
	if (video->drm.sinit == 0)
		return;

	video->drm.sinit = 0;
	gl_tex_free(&video->drm.tex, 1);
	gl_shader_unref(video->drm.blit_shader);
	gl_shader_unref(video->drm.blend_shader);
	gl_shader_unref(video->drm.fill_shader);
}

static int display_blit(struct uterm_display *disp,
			const struct uterm_video_buffer *buf,
			unsigned int x, unsigned int y)
{
	unsigned int sw, sh, tmp, width, height, i;
	float mat[16];
	float vertices[6 * 2], texpos[6 * 2];
	int ret;
	uint8_t *packed, *src, *dst;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_XRGB32)
		return -EINVAL;

	ret = display_use(disp);
	if (ret)
		return ret;
	ret = init_shaders(disp->video);
	if (ret)
		return ret;

	sw = disp->current_mode->drm.info.hdisplay;
	sh = disp->current_mode->drm.info.vdisplay;

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
	texpos[1] = 0.0;
	texpos[2] = 0.0;
	texpos[3] = 1.0;
	texpos[4] = 1.0;
	texpos[5] = 1.0;

	texpos[6] = 0.0;
	texpos[7] = 0.0;
	texpos[8] = 1.0;
	texpos[9] = 1.0;
	texpos[10] = 1.0;
	texpos[11] = 0.0;

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

	glViewport(x, y, width, height);
	glDisable(GL_BLEND);

	gl_shader_use(disp->video->drm.blit_shader);

	gl_m4_identity(mat);
	glUniformMatrix4fv(disp->video->drm.uni_blit_proj, 1, GL_FALSE, mat);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, disp->video->drm.tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	if (disp->video->drm.supports_rowlen) {
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
	glUniform1i(disp->video->drm.uni_blit_tex, 0);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texpos);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	if (gl_has_error(disp->video->drm.blit_shader)) {
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
	unsigned int sw, sh, tmp, width, height, i;
	float mat[16];
	float vertices[6 * 2], texpos[6 * 2], fgcol[3], bgcol[3];
	int ret;
	uint8_t *packed, *src, *dst;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_GREY)
		return -EINVAL;

	ret = display_use(disp);
	if (ret)
		return ret;
	ret = init_shaders(disp->video);
	if (ret)
		return ret;

	sw = disp->current_mode->drm.info.hdisplay;
	sh = disp->current_mode->drm.info.vdisplay;

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
	texpos[1] = 0.0;
	texpos[2] = 0.0;
	texpos[3] = 1.0;
	texpos[4] = 1.0;
	texpos[5] = 1.0;

	texpos[6] = 0.0;
	texpos[7] = 0.0;
	texpos[8] = 1.0;
	texpos[9] = 1.0;
	texpos[10] = 1.0;
	texpos[11] = 0.0;

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

	glViewport(x, y, width, height);
	glDisable(GL_BLEND);

	gl_shader_use(disp->video->drm.blend_shader);

	gl_m4_identity(mat);
	glUniformMatrix4fv(disp->video->drm.uni_blend_proj, 1, GL_FALSE, mat);

	glUniform3fv(disp->video->drm.uni_blend_fgcol, 1, fgcol);
	glUniform3fv(disp->video->drm.uni_blend_bgcol, 1, bgcol);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, disp->video->drm.tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	if (disp->video->drm.supports_rowlen) {
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
	glUniform1i(disp->video->drm.uni_blend_tex, 0);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texpos);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	if (gl_has_error(disp->video->drm.blend_shader)) {
		log_warning("GL error");
		return -EFAULT;
	}

	return 0;
}

static int display_fake_blendv(struct uterm_display *disp,
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

static int display_fill(struct uterm_display *disp,
			uint8_t r, uint8_t g, uint8_t b,
			unsigned int x, unsigned int y,
			unsigned int width, unsigned int height)
{
	unsigned int sw, sh, tmp, i;
	float mat[16];
	float vertices[6 * 2], colors[6 * 4];
	int ret;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;

	ret = display_use(disp);
	if (ret)
		return ret;
	ret = init_shaders(disp->video);
	if (ret)
		return ret;

	sw = disp->current_mode->drm.info.hdisplay;
	sh = disp->current_mode->drm.info.vdisplay;

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

	gl_shader_use(disp->video->drm.fill_shader);
	gl_m4_identity(mat);
	glUniformMatrix4fv(disp->video->drm.uni_fill_proj, 1, GL_FALSE, mat);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);

	if (gl_has_error(disp->video->drm.fill_shader)) {
		log_warning("GL error");
		return -EFAULT;
	}

	return 0;
}

static void show_displays(struct uterm_video *video)
{
	int ret;
	struct uterm_display *iter;

	if (!video_is_awake(video))
		return;

	for (iter = video->displays; iter; iter = iter->next) {
		if (!display_is_online(iter))
			continue;
		if (iter->dpms != UTERM_DPMS_ON)
			continue;

		ret = drmModeSetCrtc(video->drm.fd, iter->drm.crtc_id,
			iter->drm.rb[iter->drm.current_rb].fb, 0, 0,
			&iter->drm.conn_id, 1, &iter->current_mode->drm.info);
		if (ret) {
			log_err("cannot set drm-crtc on display %p", iter);
			continue;
		}
	}
}

static int get_dpms(struct uterm_display *disp, drmModeConnector *conn)
{
	int i, ret;
	drmModePropertyRes *prop;

	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(disp->video->drm.fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			switch (conn->prop_values[i]) {
			case DRM_MODE_DPMS_ON:
				ret = UTERM_DPMS_ON;
				break;
			case DRM_MODE_DPMS_STANDBY:
				ret = UTERM_DPMS_STANDBY;
				break;
			case DRM_MODE_DPMS_SUSPEND:
				ret = UTERM_DPMS_SUSPEND;
				break;
			case DRM_MODE_DPMS_OFF:
			default:
				ret = UTERM_DPMS_OFF;
			}

			drmModeFreeProperty(prop);
			return ret;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props)
		log_warn("display does not support DPMS");
	return UTERM_DPMS_UNKNOWN;
}

static void bind_display(struct uterm_video *video, drmModeRes *res,
							drmModeConnector *conn)
{
	struct uterm_display *disp;
	struct uterm_mode *mode;
	int ret, i;

	ret = display_new(&disp, &drm_display_ops);
	if (ret)
		return;

	for (i = 0; i < conn->count_modes; ++i) {
		ret = mode_new(&mode, &drm_mode_ops);
		if (ret)
			continue;
		mode->drm.info = conn->modes[i];
		mode->next = disp->modes;
		disp->modes = mode;

		/* TODO: more sophisticated default-mode selection */
		if (!disp->default_mode)
			disp->default_mode = mode;
	}

	if (!disp->modes) {
		log_warn("no valid mode for display found");
		uterm_display_unref(disp);
		return;
	}

	disp->video = video;
	disp->drm.conn_id = conn->connector_id;
	disp->flags |= DISPLAY_AVAILABLE;
	disp->next = video->displays;
	video->displays = disp;
	disp->dpms = get_dpms(disp, conn);
	log_info("display %p DPMS is %s", disp,
			uterm_dpms_to_name(disp->dpms));
	VIDEO_CB(video, disp, UTERM_NEW);
}

static void unbind_display(struct uterm_display *disp)
{
	if (!display_is_conn(disp))
		return;

	VIDEO_CB(disp->video, disp, UTERM_GONE);
	display_deactivate(disp);
	disp->video = NULL;
	disp->flags &= ~DISPLAY_AVAILABLE;
	uterm_display_unref(disp);
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
						unsigned int usec, void *data)
{
	struct uterm_display *disp = data;

	disp->flags &= ~DISPLAY_VSYNC;
	uterm_display_unref(disp);
}

static void event(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_video *video = data;
	drmEventContext ev;

	if (mask & (EV_HUP | EV_ERR)) {
		log_err("error or hangup on DRM fd");
		ev_eloop_rm_fd(video->drm.efd);
		video->drm.efd = NULL;
		return;
	}

	if (mask & EV_READABLE) {
		memset(&ev, 0, sizeof(ev));
		ev.version = DRM_EVENT_CONTEXT_VERSION;
		ev.page_flip_handler = page_flip_handler;
		drmHandleEvent(video->drm.fd, &ev);
	}
}

static int video_init(struct uterm_video *video, const char *node)
{
	const char *ext;
	int ret;
	EGLint major, minor;
	EGLenum api;
	static const EGLint ctx_att[] =
		{ EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	struct drm_video *drm = &video->drm;

	log_info("probing %s", node);

	drm->fd = open(node, O_RDWR | O_CLOEXEC);
	if (drm->fd < 0) {
		log_err("cannot open drm device %s (%d): %m", node, errno);
		return -EFAULT;
	}
	drmDropMaster(drm->fd);

	drm->gbm = gbm_create_device(drm->fd);
	if (!drm->gbm) {
		log_err("cannot create gbm device for %s (permission denied)",
			node);
		ret = -EFAULT;
		goto err_close;
	}

	setenv("EGL_DISPLAY", "drm", 1);
	drm->disp = eglGetDisplay((EGLNativeDisplayType) drm->gbm);
	if (!drm->disp) {
		log_err("cannot retrieve egl display for %s", node);
		ret = -EFAULT;
		goto err_gbm;
	}

	ret = eglInitialize(drm->disp, &major, &minor);
	if (!ret) {
		log_err("cannot init egl display for %s", node);
		ret = -EFAULT;
		goto err_gbm;
	}

	ext = eglQueryString(drm->disp, EGL_EXTENSIONS);
	if (!ext || (!strstr(ext, "EGL_KHR_surfaceless_opengl") && !strstr(ext, "EGL_KHR_surfaceless_context"))) {
		log_err("surfaceless opengl not supported");
		ret = -EFAULT;
		goto err_disp;
	}

	api = EGL_OPENGL_ES_API;
	/* TODO: allow api = EGL_OPENGL_API */
	if (!eglBindAPI(api)) {
		log_err("cannot bind opengl-es api");
		ret = -EFAULT;
		goto err_disp;
	}

	drm->ctx = eglCreateContext(drm->disp, NULL, EGL_NO_CONTEXT, ctx_att);
	if (!drm->ctx) {
		log_err("cannot create egl context");
		ret = -EFAULT;
		goto err_disp;
	}

	if (!eglMakeCurrent(drm->disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    drm->ctx)) {
		log_err("cannot activate egl context");
		ret = -EFAULT;
		goto err_ctx;
	}

	ext = (const char*)glGetString(GL_EXTENSIONS);
	if (ext && strstr((const char*)ext, "GL_EXT_unpack_subimage"))
		drm->supports_rowlen = true;
	else
		log_warning("your GL implementation does not support GL_EXT_unpack_subimage, rendering may be slower than usual");

	ret = ev_eloop_new_fd(video->eloop, &drm->efd, drm->fd,
				EV_READABLE, event, video);
	if (ret)
		goto err_noctx;

	video->flags |= VIDEO_HOTPLUG;
	log_info("new drm device via %s", node);

	return 0;

err_noctx:
	eglMakeCurrent(drm->disp,
		       EGL_NO_SURFACE,
		       EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);
err_ctx:
	eglDestroyContext(drm->disp, drm->ctx);
err_disp:
	eglTerminate(drm->disp);
err_gbm:
	gbm_device_destroy(drm->gbm);
err_close:
	close(drm->fd);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct drm_video *drm = &video->drm;
	struct uterm_display *disp;

	while ((disp = video->displays)) {
		video->displays = disp->next;
		disp->next = NULL;
		unbind_display(disp);
	}

	log_info("free drm device");
	ev_eloop_rm_fd(drm->efd);

	video_do_use(video);
	deinit_shaders(video);

	eglMakeCurrent(drm->disp,
			EGL_NO_SURFACE,
			EGL_NO_SURFACE,
			EGL_NO_CONTEXT);
	eglDestroyContext(drm->disp, drm->ctx);
	eglTerminate(drm->disp);
	gbm_device_destroy(drm->gbm);
	drmDropMaster(drm->fd);
	close(drm->fd);
}

static int video_use(struct uterm_video *video)
{
	if (eglGetCurrentContext() == video->drm.ctx)
		return 0;

	if (!eglMakeCurrent(video->drm.disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
				video->drm.ctx)) {
		log_err("cannot activate egl context");
		return -EFAULT;
	}

	return 0;
}

static int hotplug(struct uterm_video *video)
{
	drmModeRes *res;
	drmModeConnector *conn;
	struct uterm_display *disp, *tmp;
	int i;

	if (!video_is_awake(video) || !video_need_hotplug(video))
		return 0;

	res = drmModeGetResources(video->drm.fd);
	if (!res) {
		log_err("cannot retrieve drm resources");
		return -EACCES;
	}

	for (disp = video->displays; disp; disp = disp->next)
		disp->flags &= ~DISPLAY_AVAILABLE;

	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(video->drm.fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED) {
			for (disp = video->displays; disp; disp = disp->next) {
				if (disp->drm.conn_id == res->connectors[i]) {
					disp->flags |= DISPLAY_AVAILABLE;
					break;
				}
			}
			if (!disp)
				bind_display(video, res, conn);
		}
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	while (video->displays) {
		tmp = video->displays;
		if (tmp->flags & DISPLAY_AVAILABLE)
			break;

		video->displays = tmp->next;
		tmp->next = NULL;
		unbind_display(tmp);
	}
	for (disp = video->displays; disp && disp->next; ) {
		tmp = disp->next;
		if (tmp->flags & DISPLAY_AVAILABLE) {
			disp = tmp;
		} else {
			disp->next = tmp->next;
			tmp->next = NULL;
			unbind_display(tmp);
		}
	}

	video->flags &= ~VIDEO_HOTPLUG;
	return 0;
}

static int video_poll(struct uterm_video *video)
{
	video->flags |= VIDEO_HOTPLUG;
	return hotplug(video);
}

static void video_sleep(struct uterm_video *video)
{
	if (!video_is_awake(video))
		return;

	drmDropMaster(video->drm.fd);
	video->flags &= ~VIDEO_AWAKE;
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	if (video_is_awake(video))
		return 0;

	ret = drmSetMaster(video->drm.fd);
	if (ret) {
		log_err("cannot set DRM-master");
		return -EACCES;
	}

	video->flags |= VIDEO_AWAKE;
	ret = hotplug(video);
	if (ret) {
		video->flags &= ~VIDEO_AWAKE;
		drmDropMaster(video->drm.fd);
		return ret;
	}

	show_displays(video);
	return 0;
}

const struct mode_ops drm_mode_ops = {
	.init = NULL,
	.destroy = NULL,
	.get_name = mode_get_name,
	.get_width = mode_get_width,
	.get_height = mode_get_height,
};

const struct display_ops drm_display_ops = {
	.init = NULL,
	.destroy = NULL,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = display_use,
	.swap = display_swap,
	.blit = display_blit,
	.blend = display_blend,
	.blendv = display_fake_blendv,
	.fake_blendv = display_fake_blendv,
	.fill = display_fill,
};

const struct video_ops drm_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO: reset all saved CRTCs on segfault */
	.use = video_use,
	.poll = video_poll,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};
