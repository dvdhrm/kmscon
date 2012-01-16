/*
 * kmscon - Drawing Contexts
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
 * Drawing Contexts
 * This provides a drwaing context for compositor objects and associated
 * framebuffers for output objects. It is implemented with OpenGL as backend.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "log.h"
#include "output.h"

struct kmscon_context {
	EGLDisplay display;
	EGLContext context;
	PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC proc_rbuf_storage;
	PFNEGLCREATEIMAGEKHRPROC proc_create_image;
	PFNEGLDESTROYIMAGEKHRPROC proc_destroy_image;
	PFNGLGENRENDERBUFFERSPROC proc_gen_renderbuffers;
	PFNGLBINDRENDERBUFFERPROC proc_bind_renderbuffer;
	PFNGLDELETERENDERBUFFERSPROC proc_delete_renderbuffers;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC proc_framebuffer_renderbuffer;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC proc_check_framebuffer_status;
	PFNGLGENFRAMEBUFFERSPROC proc_gen_framebuffers;
	PFNGLBINDFRAMEBUFFERPROC proc_bind_framebuffer;
	PFNGLDELETEFRAMEBUFFERSPROC proc_delete_framebuffers;
};

struct renderbuffer {
	struct kmscon_context *ctx;
	EGLImageKHR image;
	GLuint rb;
};

struct kmscon_framebuffer {
	struct kmscon_context *ctx;
	GLuint fb;
	struct renderbuffer *rbs[2];
	unsigned int current_rb;
};

/*
 * Clear the GL error stack. The standard says that the error value is just a
 * single value and no list/stack. However, multiple error fields may be defined
 * and glGetError() returns only one of them until all are cleared. Hence, we
 * loop until no more error is retrieved.
 */
static void clear_gl_error()
{
	GLenum err;

	do {
		err = glGetError();
	} while (err != GL_NO_ERROR);
}

/* return true if there is a pending GL error */
static bool has_gl_error()
{
	return glGetError() != GL_NO_ERROR;
}

/*
 * Create the GL context
 * This uses the EGL library for context creation and needs a valid gbm device
 * as argument. The caller must provide a valid gbm device as \gbm. We do not
 * touch \gbm at all but pass it to EGL. The \gbm object must live as long as we
 * do.
 */
int kmscon_context_new(struct kmscon_context **out, void *gbm)
{
	struct kmscon_context *ctx;
	EGLint major, minor;
	int ret;
	const char *ext;

	if (!out || !gbm)
		return -EINVAL;

	log_debug("context: new GL context\n");

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	memset(ctx, 0, sizeof(*ctx));

	ctx->proc_rbuf_storage = (void*)
		eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	ctx->proc_create_image =
		(void*) eglGetProcAddress("eglCreateImageKHR");
	ctx->proc_destroy_image =
		(void*) eglGetProcAddress("eglDestroyImageKHR");
	ctx->proc_gen_renderbuffers =
		(void*) eglGetProcAddress("glGenRenderbuffers");
	ctx->proc_bind_renderbuffer =
		(void*) eglGetProcAddress("glBindRenderbuffer");
	ctx->proc_delete_renderbuffers =
		(void*) eglGetProcAddress("glDeleteRenderbuffers");
	ctx->proc_framebuffer_renderbuffer =
		(void*) eglGetProcAddress("glFramebufferRenderbuffer");
	ctx->proc_check_framebuffer_status =
		(void*) eglGetProcAddress("glCheckFramebufferStatus");
	ctx->proc_gen_framebuffers =
		(void*) eglGetProcAddress("glGenFramebuffers");
	ctx->proc_bind_framebuffer =
		(void*) eglGetProcAddress("glBindFramebuffer");
	ctx->proc_delete_framebuffers =
		(void*) eglGetProcAddress("glDeleteFramebuffers");

	if (!ctx->proc_rbuf_storage || !ctx->proc_create_image ||
						!ctx->proc_destroy_image) {
		log_warning("context: KHR images not supported\n");
		ret = -ENOTSUP;
		goto err_free;
	} else if (!ctx->proc_gen_renderbuffers ||
			!ctx->proc_bind_renderbuffer ||
			!ctx->proc_delete_renderbuffers ||
			!ctx->proc_framebuffer_renderbuffer ||
			!ctx->proc_check_framebuffer_status) {
		log_warning("context: renderbuffers not supported\n");
		ret = -ENOTSUP;
		goto err_free;
	}

	ctx->display = eglGetDisplay((EGLNativeDisplayType) gbm);
	if (!ctx->display) {
		log_warning("context: cannot get EGL display\n");
		ret = -EFAULT;
		goto err_free;
	}

	ret = eglInitialize(ctx->display, &major, &minor);
	if (!ret) {
		log_warning("context: cannot initialize EGL display\n");
		ret = -EFAULT;
		goto err_free;
	}

	ext = eglQueryString(ctx->display, EGL_EXTENSIONS);
	if (!ext || !strstr(ext, "EGL_KHR_surfaceless_opengl")) {
		log_warning("context: surfaceless EGL not supported\n");
		ret = -ENOTSUP;
		goto err_display;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		log_warning("context: cannot bind EGL OpenGL API\n");
		ret = -EFAULT;
		goto err_display;
	}

	ctx->context = eglCreateContext(ctx->display, NULL, EGL_NO_CONTEXT,
									NULL);
	if (!ctx->context) {
		log_warning("context: cannot create EGL context\n");
		ret = -EFAULT;
		goto err_display;
	}

	*out = ctx;
	return 0;

err_display:
	eglTerminate(ctx->display);
err_free:
	free(ctx);
	return ret;
}

void kmscon_context_destroy(struct kmscon_context *ctx)
{
	if (!ctx)
		return;

	eglDestroyContext(ctx->display, ctx->context);
	eglTerminate(ctx->display);
	free(ctx);
	log_debug("context: destroying GL context\n");
}

int kmscon_context_use(struct kmscon_context *ctx)
{
	if (!ctx)
		return -EINVAL;

	if (!eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
							ctx->context)) {
		log_warning("context: cannot use EGL context\n");
		return -EFAULT;
	}

	return 0;
}

bool kmscon_context_is_active(struct kmscon_context *ctx)
{
	if (!ctx)
		return false;

	return ctx->context == eglGetCurrentContext();
}

void kmscon_context_flush(struct kmscon_context *ctx)
{
	if (!ctx)
		return;

	glFinish();
}

void kmscon_context_viewport(struct kmscon_context *ctx,
				unsigned int width, unsigned int height)
{
	if (!ctx)
		return;

	glViewport(0, 0, width, height);
}

void kmscon_context_clear(struct kmscon_context *ctx)
{
	if (!ctx)
		return;

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

int renderbuffer_new(struct renderbuffer **out, struct kmscon_context *ctx,
								void *bo)
{
	struct renderbuffer *rb;
	int ret;

	if (!out || !ctx || !bo)
		return -EINVAL;

	rb = malloc(sizeof(*rb));
	if (!rb)
		return -ENOMEM;

	memset(rb, 0, sizeof(*rb));
	rb->ctx = ctx;

	clear_gl_error();

	rb->image = ctx->proc_create_image(ctx->display, NULL,
					EGL_NATIVE_PIXMAP_KHR, bo, NULL);
	if (!rb->image) {
		log_warning("context: cannot create EGL image\n");
		ret = -EFAULT;
		goto err_free;
	}

	ctx->proc_gen_renderbuffers(1, &rb->rb);
	ctx->proc_bind_renderbuffer(GL_RENDERBUFFER, rb->rb);
	ctx->proc_rbuf_storage(GL_RENDERBUFFER, rb->image);

	if (has_gl_error()) {
		log_warning("context: cannot create renderbuffers\n");
		ret = -EFAULT;
		goto err_gl;
	}

	*out = rb;
	return 0;

err_gl:
	ctx->proc_bind_renderbuffer(GL_RENDERBUFFER, 0);
	ctx->proc_delete_renderbuffers(1, &rb->rb);
	ctx->proc_destroy_image(ctx->display, rb->image);
err_free:
	free(rb);
	return ret;
}

void renderbuffer_destroy(struct renderbuffer *rb)
{
	if (!rb)
		return;

	rb->ctx->proc_bind_renderbuffer(GL_RENDERBUFFER, 0);
	rb->ctx->proc_delete_renderbuffers(1, &rb->rb);
	rb->ctx->proc_destroy_image(rb->ctx->display, rb->image);
	free(rb);
}

int kmscon_framebuffer_new(struct kmscon_framebuffer **out,
			struct kmscon_context *ctx, void *bo1, void *bo2)
{
	struct kmscon_framebuffer *fb;
	int ret;

	if (!out || !ctx || !bo1 || !bo2)
		return -EINVAL;

	fb = malloc(sizeof(*fb));
	if (!fb)
		return -ENOMEM;

	memset(fb, 0, sizeof(*fb));
	fb->ctx = ctx;

	ret = renderbuffer_new(&fb->rbs[0], ctx, bo1);
	if (ret)
		goto err_free;

	ret = renderbuffer_new(&fb->rbs[1], ctx, bo2);
	if (ret)
		goto err_rb;

	ctx->proc_gen_framebuffers(1, &fb->fb);
	ctx->proc_bind_framebuffer(GL_FRAMEBUFFER, fb->fb);
	ctx->proc_framebuffer_renderbuffer(GL_FRAMEBUFFER,
			GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb->rbs[0]->rb);

	if (ctx->proc_check_framebuffer_status(GL_FRAMEBUFFER) !=
						GL_FRAMEBUFFER_COMPLETE) {
		log_warning("context: invalid GL framebuffer state\n");
		ret = -EFAULT;
		goto err_fb;
	}

	*out = fb;
	return 0;

err_fb:
	ctx->proc_bind_framebuffer(GL_FRAMEBUFFER, 0);
	ctx->proc_delete_framebuffers(1, &fb->fb);
	renderbuffer_destroy(fb->rbs[1]);
err_rb:
	renderbuffer_destroy(fb->rbs[0]);
err_free:
	free(fb);
	return ret;
}

void kmscon_framebuffer_destroy(struct kmscon_framebuffer *fb)
{
	if (!fb)
		return;

	fb->ctx->proc_bind_framebuffer(GL_FRAMEBUFFER, 0);
	fb->ctx->proc_delete_framebuffers(1, &fb->fb);
	renderbuffer_destroy(fb->rbs[1]);
	renderbuffer_destroy(fb->rbs[0]);
	free(fb);
}

void kmscon_framebuffer_use(struct kmscon_framebuffer *fb)
{
	if (!fb)
		return;

	fb->ctx->proc_bind_framebuffer(GL_FRAMEBUFFER, fb->fb);
}

int kmscon_framebuffer_swap(struct kmscon_framebuffer *fb)
{
	if (!fb)
		return -EINVAL;

	fb->current_rb ^= 1;
	fb->ctx->proc_bind_framebuffer(GL_FRAMEBUFFER, fb->fb);
	fb->ctx->proc_framebuffer_renderbuffer(GL_FRAMEBUFFER,
					GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
					fb->rbs[fb->current_rb]->rb);

	if (fb->ctx->proc_check_framebuffer_status(GL_FRAMEBUFFER) !=
						GL_FRAMEBUFFER_COMPLETE)
		log_warning("context: invalid GL framebuffer state\n");

	return fb->current_rb;
}
