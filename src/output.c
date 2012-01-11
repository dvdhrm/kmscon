/*
 * kmscon - KMS/DRM output handling
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
 * KMS/DRM Output Handling
 * This provides the compositor, output and mode objects and creates OpenGL
 * contexts available for drawing directly to the graphics framebuffer.
 */

/*
 * TODO: Avoid using this hack and instead retrieve EGL and GL extension
 * pointers dynamically on initialization.
 */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "log.h"
#include "output.h"

struct kmscon_mode {
	size_t ref;
	struct kmscon_mode *next;
	struct kmscon_output *output;

	drmModeModeInfo info;
};

struct render_buffer {
	GLuint rb;
	struct gbm_bo *bo;
	EGLImageKHR image;
	uint32_t fb;
};

struct kmscon_output {
	size_t ref;
	struct kmscon_output *next;
	struct kmscon_compositor *comp;

	/* temporary flag used in compositor_refresh */
	unsigned int available : 1;
	/* flag which indicates whether the output is connected */
	unsigned int connected : 1;
	/* flag which indicates whether the output is active */
	unsigned int active : 1;

	size_t count_modes;
	struct kmscon_mode *modes;
	struct kmscon_mode *current;
	struct kmscon_mode *def_mode;

	uint32_t conn_id;
	uint32_t crtc_id;

	unsigned int cur_rb;
	struct render_buffer rb[2];
	GLuint fb;

	drmModeCrtcPtr saved_crtc;
};

enum compositor_state {
	COMPOSITOR_ASLEEP,
	COMPOSITOR_AWAKE,
};

struct kmscon_compositor {
	size_t ref;
	int state;

	size_t count_outputs;
	struct kmscon_output *outputs;

	int drm_fd;
	struct gbm_device *gbm;
	EGLDisplay display;
	EGLContext context;
};

/*
 * Creates a new output mode. This mode is not bound to any output and all
 * values are initialized to zero.
 * Returns 0 on success and copies a pointer to the object into \out.
 * Otherwise returns negative error code.
 */
int kmscon_mode_new(struct kmscon_mode **out)
{
	struct kmscon_mode *mode;

	if (!out)
		return -EINVAL;

	mode = malloc(sizeof(*mode));
	if (!mode)
		return -ENOMEM;

	memset(mode, 0, sizeof(*mode));
	mode->ref = 1;

	*out = mode;
	return 0;
}

void kmscon_mode_ref(struct kmscon_mode *mode)
{
	if (!mode)
		return;

	++mode->ref;
}

void kmscon_mode_unref(struct kmscon_mode *mode)
{
	if (!mode || !mode->ref)
		return;

	if (--mode->ref)
		return;

	/*
	 * The mode is always unbound at this time, because mode_bind takes a
	 * reference and mode_unbind releases it.
	 */

	free(mode);
}

/*
 * Binds the mode to an output. Even though this is called "mode"_bind, its the
 * output object that owns the mode, not vice versa!
 * The output object must go sure that it unbinds all modes before destroying
 * itself.
 * Binding a mode does not mean using it. This only links it into the list of
 * available modes. The output must set the values of the mode directly. By
 * default they are set to 0/NULL.
 * Returns 0 on success or negative error code on failure.
 */
static int kmscon_mode_bind(struct kmscon_mode *mode,
						struct kmscon_output *output)
{
	if (!mode || !output)
		return -EINVAL;

	if (mode->output || mode->next)
		return -EALREADY;

	mode->next = output->modes;
	output->modes = mode;
	++output->count_modes;

	mode->output = output;
	kmscon_mode_ref(mode);

	if (!output->def_mode)
		output->def_mode = mode;

	return 0;
}

/*
 * This unbinds the mode from its output. If the mode is currently active, then
 * this function will return -EBUSY. Otherwise it returns 0.
 */
static int kmscon_mode_unbind(struct kmscon_mode *mode)
{
	struct kmscon_mode *iter;
	struct kmscon_output *output;

	if (!mode || !mode->output)
		return 0;

	output = mode->output;

	if (output->current == mode)
		return -EBUSY;

	if (output->modes == mode) {
		output->modes = output->modes->next;
	} else if (output->modes) {
		for (iter = output->modes; iter->next; iter = iter->next) {
			if (iter->next == mode) {
				iter->next = mode->next;
				break;
			}
		}
	}

	mode->next = NULL;
	mode->output = NULL;
	--output->count_modes;
	kmscon_mode_unref(mode);

	if (output->def_mode == mode)
		output->def_mode = output->modes;

	return 0;
}

struct kmscon_mode *kmscon_mode_next(struct kmscon_mode *mode)
{
	if (!mode)
		return NULL;

	return mode->next;
}

const char *kmscon_mode_get_name(const struct kmscon_mode *mode)
{
	if (!mode)
		return NULL;

	return mode->info.name;
}

uint32_t kmscon_mode_get_width(const struct kmscon_mode *mode)
{
	if (!mode)
		return 0;

	return mode->info.hdisplay;
}

uint32_t kmscon_mode_get_height(const struct kmscon_mode *mode)
{
	if (!mode)
		return 0;

	return mode->info.vdisplay;
}

/*
 * Creates a new output object. The returned raw output object is useless
 * unless you bind it to a compositor, connect it to the DRM and activate it.
 * Returns 0 on success, otherwise a negative error code.
 */
int kmscon_output_new(struct kmscon_output **out)
{
	struct kmscon_output *output;

	if (!out)
		return -EINVAL;

	log_debug("output: creating output object\n");

	output = malloc(sizeof(*output));
	if (!output)
		return -ENOMEM;

	memset(output, 0, sizeof(*output));
	output->ref = 1;

	*out = output;
	return 0;
}

void kmscon_output_ref(struct kmscon_output *output)
{
	if (!output)
		return;

	++output->ref;
}

/*
 * Drops a reference. All connected modes should already be removed when the
 * output is unbound so no cleanup needs to be done here.
 */
void kmscon_output_unref(struct kmscon_output *output)
{
	if (!output || !output->ref)
		return;

	if (--output->ref)
		return;

	/*
	 * Output is already deactivated because output_bind takes
	 * a reference and output_unbind drops it.
	 * output->current is also NULL then.
	 */

	free(output);
	log_debug("output: destroying output object\n");
}

/*
 * This binds the output to the given compositor. If the output is already
 * bound, this will fail with -EALREADY.
 * This only links the output into the list of available outputs, it does not
 * activate the output or connect an crtc, nor does it create a framebuffer.
 */
static int kmscon_output_bind(struct kmscon_output *output,
						struct kmscon_compositor *comp)
{
	if (!output || !comp)
		return -EINVAL;

	if (output->comp || output->next)
		return -EALREADY;

	output->next = comp->outputs;
	comp->outputs = output;
	++comp->count_outputs;

	output->comp = comp;
	kmscon_output_ref(output);

	return 0;
}

/*
 * This unbinds the output from its compositor. If the output is currently
 * active, then it is deactivated first. The DRM connection is also removed so
 * the object is quite useless now unless you reconnect it.
 */
static void kmscon_output_unbind(struct kmscon_output *output)
{
	struct kmscon_output *iter;
	struct kmscon_compositor *comp;

	if (!output || !output->comp)
		return;

	/* deactivate and disconnect the output */
	kmscon_output_deactivate(output);
	output->connected = 0;
	while (output->modes)
		kmscon_mode_unbind(output->modes);

	comp = output->comp;

	if (comp->outputs == output) {
		comp->outputs = comp->outputs->next;
	} else if (comp->outputs) {
		for (iter = comp->outputs; iter->next; iter = iter->next) {
			if (iter->next == output) {
				iter->next = output->next;
				break;
			}
		}
	}

	output->next = NULL;
	output->comp = NULL;
	--comp->count_outputs;
	kmscon_output_unref(output);
}

/*
 * Finds an available unused crtc for the given encoder. Returns -1 if none is
 * found. Otherwise returns the non-negative crtc id.
 */
static int32_t find_crtc(struct kmscon_compositor *comp, drmModeRes *res,
							drmModeEncoder *enc)
{
	int i;
	struct kmscon_output *iter;
	uint32_t crtc = 0;

	for (i = 0; i < res->count_crtcs; ++i) {
		if (enc->possible_crtcs & (1 << i)) {
			crtc = res->crtcs[i];

			/* check that the crtc is unused */
			for (iter = comp->outputs; iter; iter = iter->next) {
				if (iter->connected && iter->crtc_id == crtc)
					break;
			}

			if (!iter)
				break;
		}
	}

	if (i == res->count_crtcs)
		return -1;

	return crtc;
}

/*
 * This connects the given output with the drm connector/crtc/encoder. This can
 * only be called once on a bound output. It will fail if it is called again
 * unless you unbind and rebind the object.
 * If the given drm connector is invalid or cannot be initialized, then this
 * function returns an appropriate negative error code. Returns 0 on success.
 *
 * This does not create any framebuffer or renderbuffers. It only reads the
 * available data so the application can retrieve information about the output.
 * The application can now activate and deactivate the output as often as it
 * wants.
 *
 * This does not work if the bound compositor is asleep!
 */
static int kmscon_output_connect(struct kmscon_output *output, drmModeRes *res,
							drmModeConnector *conn)
{
	struct kmscon_compositor *comp;
	struct kmscon_mode *mode;
	drmModeEncoder *enc;
	int ret, i;
	int32_t crtc = -1;

	if (!output || !output->comp || !conn->count_modes)
		return -EINVAL;

	if (kmscon_compositor_is_asleep(output->comp))
		return -EINVAL;

	if (output->connected)
		return -EALREADY;

	comp = output->comp;

	/* find unused crtc */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(comp->drm_fd, conn->encoders[i]);
		if (!enc)
			continue;

		crtc = find_crtc(comp, res, enc);
		drmModeFreeEncoder(enc);
		if (crtc >= 0)
			break;
	}

	if (crtc < 0) {
		log_warning("output: no free CRTC left to connect output\n");
		return -EINVAL;
	}

	/* copy all modes into the output modes-list */
	for (i = 0; i < conn->count_modes; ++i) {
		ret = kmscon_mode_new(&mode);
		if (ret)
			continue;

		ret = kmscon_mode_bind(mode, output);
		if (ret) {
			kmscon_mode_unref(mode);
			continue;
		}

		mode->info = conn->modes[i];
		kmscon_mode_unref(mode);
	}

	if (!output->count_modes) {
		log_warning("output: no suitable mode available for output\n");
		return -EINVAL;
	}

	output->conn_id = conn->connector_id;
	output->crtc_id = crtc;
	output->connected = 1;

	return 0;
}

/*
 * Returns true if the output is active and the related compositor is awake.
 */
bool kmscon_output_is_awake(struct kmscon_output *output)
{
	if (!output)
		return NULL;

	return output->active && !kmscon_compositor_is_asleep(output->comp);
}

/*
 * Returns the next output in the list. If there is no next output or the
 * output is not bound to any compositor, then it returns NULL.
 * This does not take a reference of the next output nor drop a reference of
 * the current output.
 */
struct kmscon_output *kmscon_output_next(struct kmscon_output *output)
{
	if (!output)
		return NULL;

	return output->next;
}

/*
 * Returns the first entry in the list of available modes at this output. This
 * does not take a reference of the returned mode so you shouldn't call unref
 * on it unless you called *_ref earlier.
 * Returns NULL if the list is empty.
 */
struct kmscon_mode *kmscon_output_get_modes(struct kmscon_output *output)
{
	if (!output)
		return NULL;

	return output->modes;
}

/*
 * Returns a pointer to the currently used mode. Returns NULL if no mode is
 * currently active.
 */
struct kmscon_mode *kmscon_output_get_current(struct kmscon_output *output)
{
	if (!output)
		return NULL;

	return output->current;
}

/*
 * Returns a pointer to the default mode which will be used if no other mode is
 * set explicitely. Returns NULL if no default mode is available.
 */
struct kmscon_mode *kmscon_output_get_default(struct kmscon_output *output)
{
	if (!output)
		return NULL;

	return output->def_mode;
}

static int init_rb(struct render_buffer *rb, struct kmscon_compositor *comp,
							drmModeModeInfo *mode)
{
	int ret;
	unsigned int stride, handle;

	rb->bo = gbm_bo_create(comp->gbm, mode->hdisplay, mode->vdisplay,
				GBM_BO_FORMAT_XRGB8888,
				GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!rb->bo) {
		log_warning("output: cannot create gbm buffer object\n");
		return -EFAULT;
	}

	rb->image = eglCreateImageKHR(comp->display, NULL,
					EGL_NATIVE_PIXMAP_KHR, rb->bo, NULL);
	if (!rb->image) {
		log_warning("output: cannot create EGL image\n");
		ret = -EFAULT;
		goto err_bo;
	}

	glGenRenderbuffers(1, &rb->rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb->rb);
	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, rb->image);

	stride = gbm_bo_get_pitch(rb->bo);
	handle = gbm_bo_get_handle(rb->bo).u32;

	/*
	 * TODO: Is there a way to choose 24/32 dynamically without hard-coding
	 * these values here?
	 */
	ret = drmModeAddFB(comp->drm_fd, mode->hdisplay, mode->vdisplay,
					24, 32, stride, handle, &rb->fb);
	if (ret) {
		log_warning("output: cannot add DRM framebuffer object\n");
		ret = -EFAULT;
		goto err_rb;
	}

	return 0;

err_rb:
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glDeleteRenderbuffers(1, &rb->rb);
	eglDestroyImageKHR(comp->display, rb->image);
err_bo:
	gbm_bo_destroy(rb->bo);
	return ret;
}

static void destroy_rb(struct render_buffer *rb,
					struct kmscon_compositor *comp)
{
	drmModeRmFB(comp->drm_fd, rb->fb);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glDeleteRenderbuffers(1, &rb->rb);
	eglDestroyImageKHR(comp->display, rb->image);
	gbm_bo_destroy(rb->bo);
}

/*
 * This activates the output in the given mode. This returns -EALREADY if the
 * output is already activated. To switch modes, deactivate and then reactivate
 * the output.
 * When the output is activated, its previous screen contents and mode are
 * saved, to be restored when the output is deactivated.
 * Returns 0 on success.
 * This does not work if the compositor is asleep.
 */
int kmscon_output_activate(struct kmscon_output *output,
						struct kmscon_mode *mode)
{
	struct kmscon_compositor *comp;
	int ret;

	if (!output || !output->comp || !output->connected || !output->modes)
		return -EINVAL;

	if (kmscon_compositor_is_asleep(output->comp))
		return -EINVAL;

	if (output->active)
		return -EALREADY;

	if (!mode)
		mode = output->def_mode;

	log_debug("output: activating output with res %ux%u\n",
				mode->info.hdisplay, mode->info.vdisplay);

	comp = output->comp;
	output->saved_crtc = drmModeGetCrtc(comp->drm_fd, output->crtc_id);

	ret = init_rb(&output->rb[0], comp, &mode->info);
	if (ret)
		goto err_saved;

	ret = init_rb(&output->rb[1], comp, &mode->info);
	if (ret) {
		destroy_rb(&output->rb[0], comp);
		goto err_saved;
	}

	output->current = mode;
	output->active = 1;
	output->cur_rb = 0;
	glGenFramebuffers(1, &output->fb);
	glBindFramebuffer(GL_FRAMEBUFFER, output->fb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					GL_RENDERBUFFER, output->rb[0].rb);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
						GL_FRAMEBUFFER_COMPLETE) {
		log_warning("output: invalid GL framebuffer state\n");
		ret = -EFAULT;
		goto err_fb;
	}

	glViewport(0, 0, mode->info.hdisplay, mode->info.vdisplay);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	ret = kmscon_output_swap(output);
	if (ret)
		goto err_fb;

	return 0;

err_fb:
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &output->fb);
	destroy_rb(&output->rb[0], output->comp);
	destroy_rb(&output->rb[1], output->comp);
	output->active = 0;
	output->current = NULL;
err_saved:
	if (output->saved_crtc) {
		drmModeFreeCrtc(output->saved_crtc);
		output->saved_crtc = NULL;
	}

	return ret;
}

/*
 * Deactivate the output. This does not disconnect the output so you can
 * reactivate this output again.
 * When the output is deactivated, the screen contents and mode it had before
 * it was activated are restored.
 */
void kmscon_output_deactivate(struct kmscon_output *output)
{
	if (!output || !output->active)
		return;

	if (output->saved_crtc) {
		drmModeSetCrtc(output->comp->drm_fd,
						output->saved_crtc->crtc_id,
						output->saved_crtc->buffer_id,
						output->saved_crtc->x,
						output->saved_crtc->y,
						&output->conn_id,
						1,
						&output->saved_crtc->mode);
		drmModeFreeCrtc(output->saved_crtc);
		output->saved_crtc = NULL;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &output->fb);
	destroy_rb(&output->rb[0], output->comp);
	destroy_rb(&output->rb[1], output->comp);
	output->current = NULL;
	output->active = 0;
	log_debug("output: deactivated output\n");
}

/*
 * Returns true if the output is currently active. Otherwise returns false.
 */
bool kmscon_output_is_active(struct kmscon_output *output)
{
	if (!output)
		return false;

	return output->active;
}

/*
 * Binds the framebuffer of this output and sets a valid viewport so you can
 * start drawing to this output.
 * This does not work if the compositor is asleep. Returns 0 on success.
 */
int kmscon_output_use(struct kmscon_output *output)
{
	if (!output || !output->active)
		return -EINVAL;

	if (kmscon_compositor_is_asleep(output->comp))
		return -EINVAL;

	glBindFramebuffer(GL_FRAMEBUFFER, output->fb);
	glViewport(0, 0, output->current->info.hdisplay,
					output->current->info.vdisplay);

	return 0;
}

/*
 * This swaps the two renderbuffers and displays the new front buffer on the
 * screen. This does not work if the compositor is asleep.
 * This automatically binds the framebuffer of the output so you do not need to
 * call kmscon_output_use after calling this even if another framebuffer was
 * bound before.
 * Returns 0 on success.
 */
int kmscon_output_swap(struct kmscon_output *output)
{
	int ret;

	if (!output || !output->active)
		return -EINVAL;

	if (kmscon_compositor_is_asleep(output->comp))
		return -EINVAL;

	glBindFramebuffer(GL_FRAMEBUFFER, output->fb);
	glFinish();

	ret = drmModeSetCrtc(output->comp->drm_fd, output->crtc_id,
		output->rb[output->cur_rb].fb, 0, 0, &output->conn_id, 1,
						&output->current->info);
	if (ret) {
		log_warning("output: cannot set CRTC\n");
		ret = -EFAULT;
	}

	output->cur_rb ^= 1;
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_RENDERBUFFER, output->rb[output->cur_rb].rb);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
						GL_FRAMEBUFFER_COMPLETE) {
		log_warning("output: invalid GL framebuffer state\n");
		ret = -EFAULT;
	}

	return ret;
}

/*
 * Initializes the compositor object. This opens the DRI device, initializes
 * EGL and creates a GL context. It does not activate the GL context. You need
 * to call kmscon_compositor_use() to activate the context.
 * Returns 0 on success.
 */
static int compositor_init(struct kmscon_compositor *comp)
{
	EGLint major, minor;
	int ret;
	const char *ext;

	comp->state = COMPOSITOR_ASLEEP;

	/* TODO: Retrieve this path dynamically */
	comp->drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (comp->drm_fd < 0) {
		log_warning("output: cannot open dri/card0: %d\n", errno);
		return -errno;
	}

	comp->gbm = gbm_create_device(comp->drm_fd);
	if (!comp->gbm) {
		log_warning("output: cannot allocate gbm device\n");
		ret = -EFAULT;
		goto err_drm;
	}

	comp->display = eglGetDisplay((EGLNativeDisplayType)comp->gbm);
	if (!comp->display) {
		log_warning("output: cannot get EGL display\n");
		ret = -EFAULT;
		goto err_gbm;
	}

	ret = eglInitialize(comp->display, &major, &minor);
	if (!ret) {
		log_warning("output: cannot initialize EGL display\n");
		ret = -EFAULT;
		goto err_gbm;
	}

	ext = eglQueryString(comp->display, EGL_EXTENSIONS);
	if (!ext || !strstr(ext, "EGL_KHR_surfaceless_opengl")) {
		log_warning("output: surfaceless EGL not supported\n");
		ret = -ENOTSUP;
		goto err_display;
	}

	if (!eglBindAPI(EGL_OPENGL_API)) {
		log_warning("output: cannot bind EGL OpenGL API\n");
		ret = -EFAULT;
		goto err_display;
	}

	comp->context = eglCreateContext(comp->display, NULL,
							EGL_NO_CONTEXT, NULL);
	if (!comp->context) {
		log_warning("output: cannot create EGL context\n");
		ret = -EFAULT;
		goto err_display;
	}

	return 0;

err_display:
	eglTerminate(comp->display);
err_gbm:
	gbm_device_destroy(comp->gbm);
err_drm:
	close(comp->drm_fd);
	return ret;
}

/*
 * Counterpart of compositor_init(). Must not be called if compositor_init()
 * failed.
 */
static void compositor_deinit(struct kmscon_compositor *comp)
{
	while (comp->outputs)
		kmscon_output_unbind(comp->outputs);

	eglDestroyContext(comp->display, comp->context);
	eglTerminate(comp->display);
	gbm_device_destroy(comp->gbm);
	close(comp->drm_fd);
}

/*
 * Create a new compositor object. A GL context is created but the
 * compositor is asleep by default so no outputs are connected.
 */
int kmscon_compositor_new(struct kmscon_compositor **out)
{
	struct kmscon_compositor *comp;
	int ret;

	if (!out)
		return -EINVAL;

	log_debug("output: creating compositor\n");

	comp = malloc(sizeof(*comp));
	if (!comp)
		return -ENOMEM;

	memset(comp, 0, sizeof(*comp));
	comp->ref = 1;

	ret = compositor_init(comp);
	if (ret) {
		free(comp);
		return ret;
	}

	*out = comp;
	return 0;
}

void kmscon_compositor_ref(struct kmscon_compositor *comp)
{
	if (!comp)
		return;

	++comp->ref;
}

/*
 * Drops a compositor reference. This automatically disconnects all outputs if
 * the last reference is dropped.
 */
void kmscon_compositor_unref(struct kmscon_compositor *comp)
{
	if (!comp || !comp->ref)
		return;

	if (--comp->ref)
		return;

	compositor_deinit(comp);
	free(comp);
	log_debug("output: destroying compositor\n");
}

/*
 * This puts the compositor asleep. While the compositor is asleep, no access
 * to the DRI are made so other applications may use the DRM.
 * You shouldn't access the compositor and its outputs while it is asleep as
 * almost all functions will return -EINVAL while asleep.
 */
void kmscon_compositor_sleep(struct kmscon_compositor *comp)
{
	if (!comp)
		return;

	log_debug("output: putting compositor asleep\n");
	comp->state = COMPOSITOR_ASLEEP;
	drmDropMaster(comp->drm_fd);
}

/*
 * This wakes up the compositor. It automatically calls
 * kmscon_compositor_refresh(). If this function fails, the compositor is kept
 * asleep.
 * Returns the number of detected outputs on success or a negative error code
 * on failure.
 */
int kmscon_compositor_wake_up(struct kmscon_compositor *comp)
{
	int ret;

	if (!comp)
		return -EINVAL;

	if (comp->state == COMPOSITOR_AWAKE)
		return comp->count_outputs;

	log_debug("output: waking up compositor\n");

	ret = drmSetMaster(comp->drm_fd);
	if (ret) {
		log_warning("output: cannot acquire DRM master privs\n");
		return -EACCES;
	}

	comp->state = COMPOSITOR_AWAKE;
	ret = kmscon_compositor_refresh(comp);
	if (ret >= 0)
		return ret;

	comp->state = COMPOSITOR_ASLEEP;
	drmDropMaster(comp->drm_fd);

	return ret;
}

/*
 * Returns true if the compositor is asleep. Returns false if the compositor is
 * awake.
 */
bool kmscon_compositor_is_asleep(struct kmscon_compositor *comp)
{
	if (!comp)
		return false;

	return comp->state == COMPOSITOR_ASLEEP;
}

/*
 * This activates the EGL/GL context of this compositor. This works even if the
 * compositor is asleep. Moreover, most other subsystems that need an GL context
 * require this function to be called before they are used.
 *
 * You must call this before trying to enable outputs. A new compositor is not
 * enabled by default.
 * Returns 0 on success.
 *
 * If you have multiple compositors or GL contexts, you must take into account
 * that only one context can be active at a time. It is not recommended to have
 * different contexts in different threads.
 */
int kmscon_compositor_use(struct kmscon_compositor *comp)
{
	if (!eglMakeCurrent(comp->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
							comp->context)) {
		log_warning("output: cannot use EGL context\n");
		return -EFAULT;
	}

	return 0;
}

/*
 * Returns a pointer to the first output that is bound to the compositor. You
 * can use kmscon_output_next() to iterate through the single linked list of
 * outputs.
 * Returns NULL if the list is empty or on failure.
 * You do *NOT* own a reference to the returned output. If you want to keep the
 * pointer you *MUST* call kmscon_output_ref() on it. Otherwise, you must not
 * call kmscon_output_unref() on the returned object.
 * This is because you are considered to own the compositor and guarantee that
 * the compositor is not destroyed while you iterate the list. The compositor
 * itself owns a reference of all its outputs so there is no need to increase
 * this every time you iterate the list.
 *
 * This works even if the compositor is asleep.
 */
struct kmscon_output *kmscon_compositor_get_outputs(
					struct kmscon_compositor *comp)
{
	if (!comp)
		return NULL;

	return comp->outputs;
}

static int add_output(struct kmscon_compositor *comp, drmModeRes *res,
							drmModeConnector *conn)
{
	struct kmscon_output *output;
	int ret;

	ret = kmscon_output_new(&output);
	if (ret)
		return ret;

	ret = kmscon_output_bind(output, comp);
	if (ret)
		goto err_unref;

	ret = kmscon_output_connect(output, res, conn);
	if (ret)
		goto err_unbind;

	output->available = 1;
	kmscon_output_unref(output);
	return 0;

err_unbind:
	kmscon_output_unbind(output);
err_unref:
	kmscon_output_unref(output);
	return ret;
}

/*
 * Refreshs the list of available outputs. This returns -EINVAL if the
 * compositor is asleep.
 * All currently connected outputs that are still available are left untouched.
 * If an output is no longer available, it is disconnected and unbound from the
 * compositor. You should no longer use it and drop all your references.
 *
 * New monitors are automatically added into the list of outputs and all
 * available modes are added. The outputs are left deactivated, though. You
 * should reiterate the output list and activate new outputs if you want
 * hotplug support.
 *
 * Returns the number of available outputs on success and negative error code
 * on failure.
 */
int kmscon_compositor_refresh(struct kmscon_compositor *comp)
{
	drmModeConnector *conn;
	drmModeRes *res;
	int i;
	uint32_t cid;
	struct kmscon_output *output, *tmp;

	if (!comp || comp->state != COMPOSITOR_AWAKE)
		return -EINVAL;

	res = drmModeGetResources(comp->drm_fd);
	if (!res) {
		log_warning("output: cannot retrieve DRM resources\n");
		return -EACCES;
	}

	for (output = comp->outputs; output; output = output->next)
		output->available = 0;

	for (i = 0; i < res->count_connectors; ++i) {
		cid = res->connectors[i];
		conn = drmModeGetConnector(comp->drm_fd, cid);
		if (!conn)
			continue;

		if (conn->connection == DRM_MODE_CONNECTED) {
			for (output = comp->outputs; output;
						output = output->next) {
				if (output->conn_id == cid) {
					output->available = 1;
					break;
				}
			}

			if (!output)
				add_output(comp, res, conn);
		}

		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	for (output = comp->outputs; output; ) {
		tmp = output;
		output = output->next;

		if (tmp->available)
			continue;

		kmscon_output_unbind(tmp);
	}

	return comp->count_outputs;
}
