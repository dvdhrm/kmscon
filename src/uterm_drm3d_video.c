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
#include "shl_misc.h"
#include "static_gl.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_drm3d_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "uterm_drm3d_video"

static int display_init(struct uterm_display *disp)
{
	struct uterm_drm3d_display *d3d;
	int ret;

	d3d = malloc(sizeof(*d3d));
	if (!d3d)
		return -ENOMEM;
	memset(d3d, 0, sizeof(*d3d));

	ret = uterm_drm_display_init(disp, d3d);
	if (ret) {
		free(d3d);
		return ret;
	}

	return 0;
}

static void display_destroy(struct uterm_display *disp)
{
	free(uterm_drm_display_get_data(disp));
	uterm_drm_display_destroy(disp);
}

static void bo_destroy_event(struct gbm_bo *bo, void *data)
{
	struct uterm_drm3d_rb *rb = data;
	struct uterm_drm_video *vdrm;

	if (!rb)
		return;

	vdrm = rb->disp->video->data;
	drmModeRmFB(vdrm->fd, rb->fb);
	free(rb);
}

static struct uterm_drm3d_rb *bo_to_rb(struct uterm_display *disp,
				       struct gbm_bo *bo)
{
	struct uterm_drm3d_rb *rb = gbm_bo_get_user_data(bo);
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	int ret;
	unsigned int stride, handle, width, height;;

	if (rb)
		return rb;

	rb = malloc(sizeof(*rb));
	if (!rb) {
		log_error("cannot allocate memory for render buffer (%d): %m",
			  errno);
		return NULL;
	}
	rb->disp = disp;
	rb->bo = bo;

#ifdef BUILD_HAVE_GBM_BO_GET_PITCH
	stride = gbm_bo_get_pitch(rb->bo);
#else
	stride = gbm_bo_get_stride(rb->bo);
#endif
	handle = gbm_bo_get_handle(rb->bo).u32;
	width = gbm_bo_get_width(rb->bo);
	height = gbm_bo_get_height(rb->bo);

	ret = drmModeAddFB(vdrm->fd, width, height, 24, 32, stride,
			   handle, &rb->fb);
	if (ret) {
		log_err("cannot add drm-fb (%d): %m", errno);
		free(rb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, rb, bo_destroy_event);
	return rb;
}

static int display_activate(struct uterm_display *disp,
			    struct uterm_mode *mode)
{
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm;
	struct uterm_drm3d_video *v3d;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm3d_display *d3d = uterm_drm_display_get_data(disp);
	int ret;
	struct gbm_bo *bo;
	drmModeModeInfo *minfo;

	if (!mode)
		return -EINVAL;

	vdrm = video->data;
	v3d = uterm_drm_video_get_data(video);
	minfo = uterm_drm_mode_get_info(mode);
	log_info("activating display %p to %ux%u", disp,
		 minfo->hdisplay, minfo->vdisplay);

	ret = uterm_drm_display_activate(disp, vdrm->fd);
	if (ret)
		return ret;

	d3d->current = NULL;
	d3d->next = NULL;
	disp->current_mode = mode;

	d3d->gbm = gbm_surface_create(v3d->gbm, minfo->hdisplay,
				      minfo->vdisplay, GBM_FORMAT_XRGB8888,
				      GBM_BO_USE_SCANOUT |
				      GBM_BO_USE_RENDERING);
	if (!d3d->gbm) {
		log_error("cannot create gbm surface (%d): %m", errno);
		ret = -EFAULT;
		goto err_saved;
	}

	d3d->surface = eglCreateWindowSurface(v3d->disp, v3d->conf,
					      (EGLNativeWindowType)d3d->gbm,
					      NULL);
	if (d3d->surface == EGL_NO_SURFACE) {
		log_error("cannot create EGL window surface");
		ret = -EFAULT;
		goto err_gbm;
	}

	if (!eglMakeCurrent(v3d->disp, d3d->surface, d3d->surface,
			    v3d->ctx)) {
		log_error("cannot activate EGL context");
		ret = -EFAULT;
		goto err_surface;
	}

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!eglSwapBuffers(v3d->disp, d3d->surface)) {
		log_error("cannot swap buffers");
		ret = -EFAULT;
		goto err_noctx;
	}

	bo = gbm_surface_lock_front_buffer(d3d->gbm);
	if (!bo) {
		log_error("cannot lock front buffer during creation");
		ret = -EFAULT;
		goto err_noctx;
	}

	d3d->current = bo_to_rb(disp, bo);
	if (!d3d->current) {
		log_error("cannot lock front buffer");
		ret = -EFAULT;
		goto err_bo;
	}

	ret = drmModeSetCrtc(vdrm->fd, ddrm->crtc_id, d3d->current->fb,
			     0, 0, &ddrm->conn_id, 1, minfo);
	if (ret) {
		log_err("cannot set drm-crtc");
		ret = -EFAULT;
		goto err_bo;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_bo:
	gbm_surface_release_buffer(d3d->gbm, bo);
err_noctx:
	eglMakeCurrent(v3d->disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       v3d->ctx);
err_surface:
	eglDestroySurface(v3d->disp, d3d->surface);
err_gbm:
	gbm_surface_destroy(d3d->gbm);
err_saved:
	disp->current_mode = NULL;
	uterm_drm_display_deactivate(disp, vdrm->fd);
	return ret;
}

static void display_deactivate(struct uterm_display *disp)
{
	struct uterm_drm3d_display *d3d = uterm_drm_display_get_data(disp);
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm;
	struct uterm_drm3d_video *v3d;

	log_info("deactivating display %p", disp);

	vdrm = video->data;
	v3d = uterm_drm_video_get_data(video);
	uterm_drm_display_deactivate(disp, vdrm->fd);

	eglMakeCurrent(v3d->disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       v3d->ctx);
	eglDestroySurface(v3d->disp, d3d->surface);

	if (d3d->current) {
		gbm_surface_release_buffer(d3d->gbm,
					   d3d->current->bo);
		d3d->current = NULL;
	}
	if (d3d->next) {
		gbm_surface_release_buffer(d3d->gbm,
					   d3d->next->bo);
		d3d->next = NULL;
	}

	gbm_surface_destroy(d3d->gbm);
	disp->current_mode = NULL;
}

int uterm_drm3d_display_use(struct uterm_display *disp, bool *opengl)
{
	struct uterm_drm3d_display *d3d = uterm_drm_display_get_data(disp);
	struct uterm_drm3d_video *v3d;

	v3d = uterm_drm_video_get_data(disp->video);
	if (!eglMakeCurrent(v3d->disp, d3d->surface,
			    d3d->surface, v3d->ctx)) {
		log_error("cannot activate EGL context");
		return -EFAULT;
	}

	if (opengl)
		*opengl = true;

	/* TODO: lets find a way how to retrieve the current front buffer */
	return 0;
}

static int display_swap(struct uterm_display *disp, bool immediate)
{
	int ret;
	struct gbm_bo *bo;
	struct uterm_drm3d_rb *rb;
	struct uterm_drm3d_display *d3d = uterm_drm_display_get_data(disp);
	struct uterm_video *video = disp->video;
	struct uterm_drm3d_video *v3d = uterm_drm_video_get_data(video);

	if (!gbm_surface_has_free_buffers(d3d->gbm))
		return -EBUSY;

	if (!eglSwapBuffers(v3d->disp, d3d->surface)) {
		log_error("cannot swap EGL buffers (%d): %m", errno);
		return -EFAULT;
	}

	bo = gbm_surface_lock_front_buffer(d3d->gbm);
	if (!bo) {
		log_error("cannot lock front buffer");
		return -EFAULT;
	}

	rb = bo_to_rb(disp, bo);
	if (!rb) {
		log_error("cannot lock front gbm buffer (%d): %m", errno);
		gbm_surface_release_buffer(d3d->gbm, bo);
		return -EFAULT;
	}

	ret = uterm_drm_display_swap(disp, rb->fb, immediate);
	if (ret) {
		gbm_surface_release_buffer(d3d->gbm, bo);
		return ret;
	}

	if (d3d->next) {
		gbm_surface_release_buffer(d3d->gbm, d3d->next->bo);
		d3d->next = NULL;
	}

	if (immediate) {
		if (d3d->current)
			gbm_surface_release_buffer(d3d->gbm, d3d->current->bo);
		d3d->current = rb;
	} else {
		d3d->next = rb;
	}

	return 0;
}

static const struct display_ops drm_display_ops = {
	.init = display_init,
	.destroy = display_destroy,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = uterm_drm_display_set_dpms,
	.use = uterm_drm3d_display_use,
	.get_buffers = NULL,
	.swap = display_swap,
	.blit = uterm_drm3d_display_blit,
	.fake_blendv = uterm_drm3d_display_fake_blendv,
	.fill = uterm_drm3d_display_fill,
};

static void show_displays(struct uterm_video *video)
{
	int ret;
	struct uterm_display *iter;
	struct shl_dlist *i;

	if (!video_is_awake(video))
		return;

	shl_dlist_for_each(i, &video->displays) {
		iter = shl_dlist_entry(i, struct uterm_display, list);

		if (!display_is_online(iter))
			continue;
		if (iter->dpms != UTERM_DPMS_ON)
			continue;

		ret = uterm_drm3d_display_use(iter, NULL);
		if (ret)
			continue;

		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		display_swap(iter, true);
	}
}

static void page_flip_handler(struct uterm_display *disp)
{
	struct uterm_drm3d_display *d3d = uterm_drm_display_get_data(disp);

	if (d3d->next) {
		if (d3d->current)
			gbm_surface_release_buffer(d3d->gbm,
						   d3d->current->bo);
		d3d->current = d3d->next;
		d3d->next = NULL;
	}
}

static int video_init(struct uterm_video *video, const char *node)
{
	static const EGLint conf_att[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 0,
		EGL_NONE,
	};
	static const EGLint ctx_att[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	const char *ext;
	int ret;
	EGLint major, minor, n;
	EGLenum api;
	EGLBoolean b;
	struct uterm_drm_video *vdrm;
	struct uterm_drm3d_video *v3d;

	v3d = malloc(sizeof(*v3d));
	if (!v3d)
		return -ENOMEM;
	memset(v3d, 0, sizeof(*v3d));

	ret = uterm_drm_video_init(video, node, &drm_display_ops,
				   page_flip_handler, v3d);
	if (ret)
		goto err_free;
	vdrm = video->data;

	log_debug("initialize 3D layer on %p", video);

	v3d->gbm = gbm_create_device(vdrm->fd);
	if (!v3d->gbm) {
		log_err("cannot create gbm device for %s (permission denied)",
			node);
		ret = -EFAULT;
		goto err_video;
	}

	v3d->disp = eglGetDisplay((EGLNativeDisplayType) v3d->gbm);
	if (v3d->disp == EGL_NO_DISPLAY) {
		log_err("cannot retrieve egl display for %s", node);
		ret = -EFAULT;
		goto err_gbm;
	}

	b = eglInitialize(v3d->disp, &major, &minor);
	if (!b) {
		log_err("cannot init egl display for %s", node);
		ret = -EFAULT;
		goto err_gbm;
	}

	log_debug("EGL Init %d.%d", major, minor);
	log_debug("EGL Version %s", eglQueryString(v3d->disp, EGL_VERSION));
	log_debug("EGL Vendor %s", eglQueryString(v3d->disp, EGL_VENDOR));
	ext = eglQueryString(v3d->disp, EGL_EXTENSIONS);
	log_debug("EGL Extensions %s", ext);

	if (!ext || !strstr(ext, "EGL_KHR_surfaceless_context")) {
		log_err("surfaceless opengl not supported");
		ret = -EFAULT;
		goto err_disp;
	}

	api = EGL_OPENGL_ES_API;
	if (!eglBindAPI(api)) {
		log_err("cannot bind opengl-es api");
		ret = -EFAULT;
		goto err_disp;
	}

	b = eglChooseConfig(v3d->disp, conf_att, &v3d->conf, 1, &n);
	if (!b || n != 1) {
		log_error("cannot find a proper EGL framebuffer configuration");
		ret = -EFAULT;
		goto err_disp;
	}

	v3d->ctx = eglCreateContext(v3d->disp, v3d->conf, EGL_NO_CONTEXT,
				    ctx_att);
	if (v3d->ctx == EGL_NO_CONTEXT) {
		log_error("cannot create egl context");
		ret = -EFAULT;
		goto err_disp;
	}

	if (!eglMakeCurrent(v3d->disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    v3d->ctx)) {
		log_error("cannot activate surfaceless EGL context");
		ret = -EFAULT;
		goto err_ctx;
	}

	ext = (const char*)glGetString(GL_EXTENSIONS);
	if (ext && strstr((const char*)ext, "GL_EXT_unpack_subimage"))
		v3d->supports_rowlen = true;
	else
		log_warning("your GL implementation does not support GL_EXT_unpack_subimage, rendering may be slower than usual");

	return 0;

err_ctx:
	eglDestroyContext(v3d->disp, v3d->ctx);
err_disp:
	eglTerminate(v3d->disp);
err_gbm:
	gbm_device_destroy(v3d->gbm);
err_video:
	uterm_drm_video_destroy(video);
err_free:
	free(v3d);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct uterm_drm3d_video *v3d = uterm_drm_video_get_data(video);

	log_info("free drm video device %p", video);

	if (!eglMakeCurrent(v3d->disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    v3d->ctx))
		log_error("cannot activate GL context during destruction");
	uterm_drm3d_deinit_shaders(video);

	eglMakeCurrent(v3d->disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);
	eglDestroyContext(v3d->disp, v3d->ctx);
	eglTerminate(v3d->disp);
	gbm_device_destroy(v3d->gbm);
	free(v3d);
	uterm_drm_video_destroy(video);
}

static int video_poll(struct uterm_video *video)
{
	return uterm_drm_video_poll(video);
}

static void video_sleep(struct uterm_video *video)
{
	show_displays(video);
	uterm_drm_video_sleep(video);
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	ret = uterm_drm_video_wake_up(video);
	if (ret) {
		uterm_drm_video_arm_vt_timer(video);
		return ret;
	}

	show_displays(video);
	return 0;
}

static const struct video_ops drm_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO: reset all saved CRTCs on segfault */
	.poll = video_poll,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};

static const struct uterm_video_module drm3d_module = {
	.ops = &drm_video_ops,
};

SHL_EXPORT
const struct uterm_video_module *UTERM_VIDEO_DRM3D = &drm3d_module;
