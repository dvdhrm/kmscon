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

#ifdef USE_GLES2
	#include <GLES2/gl2.h>
	#include <GLES2/gl2ext.h>
#else
	#include <GL/gl.h>
	#include <GL/glext.h>
#endif

#include "log.h"
#include "output.h"

/* OpenGL extension definitions */
typedef void (*PFNGLGENRENDERBUFFERSPROC)
					(GLsizei n, GLuint *renderbuffers);
typedef void (*PFNGLBINDRENDERBUFFERPROC)
					(GLenum target, GLuint renderbuffer);
typedef void (*PFNGLDELETERENDERBUFFERSPROC)
				(GLsizei n, const GLuint *renderbuffers);

typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC) (GLenum target,
	GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUSPROC) (GLenum target);
typedef void (*PFNGLGENFRAMEBUFFERSPROC)
					(GLsizei n, GLuint *framebuffers);
typedef void (*PFNGLBINDFRAMEBUFFERPROC)
					(GLenum target, GLuint framebuffer);
typedef void (*PFNGLDELETEFRAMEBUFFERSPROC)
				(GLsizei n, const GLuint *framebuffers);

typedef GLuint (*PFNGLCREATESHADERPROC) (GLenum type);
typedef void (*PFNGLDELETESHADERPROC) (GLuint shader);
typedef void (*PFNGLSHADERSOURCEPROC) (GLuint shader,
		GLsizei count, const GLchar* *string, const GLint *length);
typedef void (*PFNGLCOMPILESHADERPROC) (GLuint shader);
typedef void (*PFNGLGETSHADERIVPROC)
				(GLuint shader, GLenum pname, GLint *params);
typedef void (*PFNGLGETSHADERINFOLOGPROC)
	(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);

typedef GLuint (*PFNGLCREATEPROGRAMPROC) (void);
typedef void (*PFNGLDELETEPROGRAMPROC) (GLuint program);
typedef void (*PFNGLUSEPROGRAMPROC) (GLuint program);
typedef void (*PFNGLATTACHSHADERPROC) (GLuint program, GLuint shader);
typedef void (*PFNGLBINDATTRIBLOCATIONPROC)
			(GLuint program, GLuint index, const GLchar *name);
typedef void (*PFNGLLINKPROGRAMPROC) (GLuint program);
typedef void (*PFNGLGETPROGRAMIVPROC)
				(GLuint program, GLenum pname, GLint *params);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)
	(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLint (*PFNGLGETUNIFORMLOCATIONPROC)
					(GLuint program, const GLchar *name);
typedef void (*PFNGLUNIFORMMATRIX4FVPROC) (GLint location,
		GLsizei count, GLboolean transpose, const GLfloat *value);
typedef void (*PFNGLUNIFORM1IPROC) (GLint location, GLint v0);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)
		(GLuint index, GLint size, GLenum type, GLboolean normalized,
					GLsizei stride, const GLvoid *pointer);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC) (GLuint index);
typedef void (*PFNGLDRAWARRAYSEXTPROC)
				(GLenum mode, GLint first, GLsizei count);

struct kmscon_context {
	EGLDisplay display;
	EGLContext context;

	GLuint def_program;
	GLuint def_vshader;
	GLuint def_fshader;
	GLuint def_uni_projection;

	GLuint tex_program;
	GLuint tex_vshader;
	GLuint tex_fshader;
	GLuint tex_uni_projection;
	GLuint tex_uni_texture;

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

	PFNGLCREATESHADERPROC proc_create_shader;
	PFNGLDELETESHADERPROC proc_delete_shader;
	PFNGLSHADERSOURCEPROC proc_shader_source;
	PFNGLCOMPILESHADERPROC proc_compile_shader;
	PFNGLGETSHADERIVPROC proc_get_shader_iv;
	PFNGLGETSHADERINFOLOGPROC proc_get_shader_info_log;

	PFNGLCREATEPROGRAMPROC proc_create_program;
	PFNGLDELETEPROGRAMPROC proc_delete_program;
	PFNGLUSEPROGRAMPROC proc_use_program;
	PFNGLATTACHSHADERPROC proc_attach_shader;
	PFNGLBINDATTRIBLOCATIONPROC proc_bind_attrib_location;
	PFNGLLINKPROGRAMPROC proc_link_program;
	PFNGLGETPROGRAMIVPROC proc_get_program_iv;
	PFNGLGETPROGRAMINFOLOGPROC proc_get_program_info_log;
	PFNGLGETUNIFORMLOCATIONPROC proc_get_uniform_location;
	PFNGLUNIFORMMATRIX4FVPROC proc_uniform_matrix_4fv;
	PFNGLUNIFORM1IPROC proc_uniform_1i;
	PFNGLVERTEXATTRIBPOINTERPROC proc_vertex_attrib_pointer;
	PFNGLENABLEVERTEXATTRIBARRAYPROC proc_enable_vertex_attrib_array;
	PFNGLDRAWARRAYSEXTPROC proc_draw_arrays;
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
	GLenum err;

	err = glGetError();
	if (err != GL_NO_ERROR) {
		log_err("context: GL error %d\n", err);
		return true;
	}

	return false;
}

/* external shader sources; generated during build */
extern const char *kmscon_vert_def;
extern const char *kmscon_frag_def;
extern const char *kmscon_vert_tex;
extern const char *kmscon_frag_tex;

static int compile_shader(struct kmscon_context *ctx, GLenum type,
							const char *source)
{
	char msg[512];
	GLint status = 1;
	GLuint s;

	s = ctx->proc_create_shader(type);
	if (s == GL_NONE) {
		log_warning("context: cannot allocate GL shader\n");
		return GL_NONE;
	}

	ctx->proc_shader_source(s, 1, &source, NULL);
	ctx->proc_compile_shader(s);

	ctx->proc_get_shader_iv(s, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		ctx->proc_get_shader_info_log(s, sizeof(msg), NULL, msg);
		log_warning("context: cannot compile shader: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

static int init_def_shader(struct kmscon_context *ctx)
{
	char msg[512];
	GLint status = 1;
	int ret;

	if (!ctx)
		return -EINVAL;

	ctx->def_vshader = compile_shader(ctx, GL_VERTEX_SHADER,
							kmscon_vert_def);
	if (ctx->def_vshader == GL_NONE)
		return -EFAULT;

	ctx->def_fshader = compile_shader(ctx, GL_FRAGMENT_SHADER,
							kmscon_frag_def);
	if (ctx->def_fshader == GL_NONE) {
		ret = -EFAULT;
		goto err_vshader;
	}

	ctx->def_program = ctx->proc_create_program();
	ctx->proc_attach_shader(ctx->def_program, ctx->def_vshader);
	ctx->proc_attach_shader(ctx->def_program, ctx->def_fshader);
	ctx->proc_bind_attrib_location(ctx->def_program, 0, "position");
	ctx->proc_bind_attrib_location(ctx->def_program, 1, "color");

	ctx->proc_link_program(ctx->def_program);
	ctx->proc_get_program_iv(ctx->def_program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		ctx->proc_get_program_info_log(ctx->def_program, sizeof(msg),
								NULL, msg);
		log_warning("context: cannot link shader: %s\n", msg);
		ret = -EFAULT;
		goto err_link;
	}

	ctx->def_uni_projection =
		ctx->proc_get_uniform_location(ctx->def_program, "projection");

	return 0;

err_link:
	ctx->proc_delete_program(ctx->def_program);
	ctx->proc_delete_shader(ctx->def_fshader);
err_vshader:
	ctx->proc_delete_shader(ctx->def_vshader);
	return ret;
}

static int init_tex_shader(struct kmscon_context *ctx)
{
	char msg[512];
	GLint status = 1;
	int ret;

	if (!ctx)
		return -EINVAL;

	ctx->tex_vshader = compile_shader(ctx, GL_VERTEX_SHADER,
							kmscon_vert_tex);
	if (ctx->tex_vshader == GL_NONE)
		return -EFAULT;

	ctx->tex_fshader = compile_shader(ctx, GL_FRAGMENT_SHADER,
							kmscon_frag_tex);
	if (ctx->tex_fshader == GL_NONE) {
		ret = -EFAULT;
		goto err_vshader;
	}

	ctx->tex_program = ctx->proc_create_program();
	ctx->proc_attach_shader(ctx->tex_program, ctx->tex_vshader);
	ctx->proc_attach_shader(ctx->tex_program, ctx->tex_fshader);
	ctx->proc_bind_attrib_location(ctx->tex_program, 0, "position");
	ctx->proc_bind_attrib_location(ctx->tex_program, 1, "texture_position");

	ctx->proc_link_program(ctx->tex_program);
	ctx->proc_get_program_iv(ctx->tex_program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		ctx->proc_get_program_info_log(ctx->tex_program, sizeof(msg),
								NULL, msg);
		log_warning("context: cannot link shader: %s\n", msg);
		ret = -EFAULT;
		goto err_link;
	}

	ctx->tex_uni_projection =
		ctx->proc_get_uniform_location(ctx->tex_program, "projection");
	ctx->tex_uni_texture =
		ctx->proc_get_uniform_location(ctx->tex_program, "texture");

	return 0;

err_link:
	ctx->proc_delete_program(ctx->tex_program);
	ctx->proc_delete_shader(ctx->tex_fshader);
err_vshader:
	ctx->proc_delete_shader(ctx->tex_vshader);
	return ret;
}

static int init_shader(struct kmscon_context *ctx)
{
	int ret;

	ret = init_def_shader(ctx);
	if (ret)
		return ret;

	ret = init_tex_shader(ctx);
	if (ret) {
		ctx->proc_delete_program(ctx->def_program);
		ctx->proc_delete_shader(ctx->def_fshader);
		ctx->proc_delete_shader(ctx->def_vshader);
		return ret;
	}

	return 0;
}

static void destroy_shader(struct kmscon_context *ctx)
{
	if (!ctx)
		return;

	ctx->proc_delete_program(ctx->tex_program);
	ctx->proc_delete_shader(ctx->tex_fshader);
	ctx->proc_delete_shader(ctx->tex_vshader);
	ctx->proc_delete_program(ctx->def_program);
	ctx->proc_delete_shader(ctx->def_fshader);
	ctx->proc_delete_shader(ctx->def_vshader);
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
	EGLenum api;

#ifdef USE_GLES2
	static const EGLint ctx_attribs[] =
			{ EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
#else
	static const EGLint *ctx_attribs = NULL;
#endif

	if (!out || !gbm)
		return -EINVAL;

	log_debug("context: new GL context\n");

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	memset(ctx, 0, sizeof(*ctx));

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

#ifdef USE_GLES2
	api = EGL_OPENGL_ES_API;
#else
	api = EGL_OPENGL_API;
#endif

	if (!eglBindAPI(api)) {
		log_warning("context: cannot bind EGL OpenGL API\n");
		ret = -EFAULT;
		goto err_display;
	}

	ctx->context = eglCreateContext(ctx->display, NULL, EGL_NO_CONTEXT,
								ctx_attribs);
	if (!ctx->context) {
		log_warning("context: cannot create EGL context\n");
		ret = -EFAULT;
		goto err_display;
	}

	if (!eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
							ctx->context)) {
		log_warning("context: cannot use EGL context\n");
		ret = -EFAULT;
		goto err_ctx;
	}

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

	ctx->proc_create_shader =
		(void*) eglGetProcAddress("glCreateShader");
	ctx->proc_delete_shader =
		(void*) eglGetProcAddress("glDeleteShader");
	ctx->proc_shader_source =
		(void*) eglGetProcAddress("glShaderSource");
	ctx->proc_compile_shader =
		(void*) eglGetProcAddress("glCompileShader");
	ctx->proc_get_shader_iv =
		(void*) eglGetProcAddress("glGetShaderiv");
	ctx->proc_get_shader_info_log =
		(void*) eglGetProcAddress("glGetShaderInfoLog");

	ctx->proc_create_program =
		(void*) eglGetProcAddress("glCreateProgram");
	ctx->proc_delete_program =
		(void*) eglGetProcAddress("glDeleteProgram");
	ctx->proc_use_program =
		(void*) eglGetProcAddress("glUseProgram");
	ctx->proc_attach_shader =
		(void*) eglGetProcAddress("glAttachShader");
	ctx->proc_bind_attrib_location =
		(void*) eglGetProcAddress("glBindAttribLocation");
	ctx->proc_link_program =
		(void*) eglGetProcAddress("glLinkProgram");
	ctx->proc_get_program_iv =
		(void*) eglGetProcAddress("glGetProgramiv");
	ctx->proc_get_program_info_log =
		(void*) eglGetProcAddress("glGetProgramInfoLog");
	ctx->proc_get_uniform_location =
		(void*) eglGetProcAddress("glGetUniformLocation");
	ctx->proc_uniform_matrix_4fv =
		(void*) eglGetProcAddress("glUniformMatrix4fv");
	ctx->proc_uniform_1i =
		(void*) eglGetProcAddress("glUniform1i");
	ctx->proc_vertex_attrib_pointer =
		(void*) eglGetProcAddress("glVertexAttribPointer");
	ctx->proc_enable_vertex_attrib_array =
		(void*) eglGetProcAddress("glEnableVertexAttribArray");
	ctx->proc_draw_arrays =
		(void*) eglGetProcAddress("glDrawArraysEXT");

	if (!ctx->proc_rbuf_storage || !ctx->proc_create_image ||
						!ctx->proc_destroy_image) {
		log_warning("context: KHR images not supported\n");
		ret = -ENOTSUP;
		goto err_ctx;
	} else if (!ctx->proc_gen_renderbuffers ||
			!ctx->proc_bind_renderbuffer ||
			!ctx->proc_delete_renderbuffers ||
			!ctx->proc_framebuffer_renderbuffer ||
			!ctx->proc_check_framebuffer_status) {
		log_warning("context: renderbuffers not supported\n");
		ret = -ENOTSUP;
		goto err_ctx;
	} else if (!ctx->proc_create_shader ||
			!ctx->proc_delete_shader ||
			!ctx->proc_shader_source ||
			!ctx->proc_compile_shader ||
			!ctx->proc_get_shader_iv ||
			!ctx->proc_get_shader_info_log) {
		log_warning("context: shaders not supported\n");
		ret = -ENOTSUP;
		goto err_ctx;
	} else if (!ctx->proc_create_program ||
			!ctx->proc_delete_program ||
			!ctx->proc_use_program ||
			!ctx->proc_attach_shader ||
			!ctx->proc_bind_attrib_location ||
			!ctx->proc_link_program ||
			!ctx->proc_get_program_iv ||
			!ctx->proc_get_program_info_log ||
			!ctx->proc_get_uniform_location ||
			!ctx->proc_uniform_matrix_4fv ||
			!ctx->proc_uniform_1i ||
			!ctx->proc_vertex_attrib_pointer ||
			!ctx->proc_enable_vertex_attrib_array ||
			!ctx->proc_draw_arrays) {
		log_warning("context: shaders not supported\n");
		ret = -ENOTSUP;
		goto err_ctx;
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	ret = init_shader(ctx);
	if (ret)
		goto err_ctx;

	*out = ctx;
	return 0;

err_ctx:
	eglDestroyContext(ctx->display, ctx->context);
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

	destroy_shader(ctx);
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

void kmscon_context_draw_def(struct kmscon_context *ctx, float *vertices,
						float *colors, size_t num)
{
	float m[16] = { 1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1 };

	if (!ctx)
		return;

	ctx->proc_use_program(ctx->def_program);
	ctx->proc_uniform_matrix_4fv(ctx->def_uni_projection, 1, GL_FALSE, m);

	ctx->proc_vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	ctx->proc_vertex_attrib_pointer(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
	ctx->proc_enable_vertex_attrib_array(0);
	ctx->proc_enable_vertex_attrib_array(1);
	ctx->proc_draw_arrays(GL_TRIANGLES, 0, num);
}

void kmscon_context_draw_tex(struct kmscon_context *ctx, const float *vertices,
	const float *texcoords, size_t num, unsigned int tex, const float *m)
{
	float mat[16];

	if (!ctx)
		return;

	kmscon_m4_transp_dest(mat, m);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);

	ctx->proc_use_program(ctx->tex_program);
	ctx->proc_uniform_matrix_4fv(ctx->tex_uni_projection, 1, GL_FALSE, mat);
	ctx->proc_uniform_1i(ctx->tex_uni_texture, 0);

	ctx->proc_vertex_attrib_pointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	ctx->proc_vertex_attrib_pointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	ctx->proc_enable_vertex_attrib_array(0);
	ctx->proc_enable_vertex_attrib_array(1);
	ctx->proc_draw_arrays(GL_TRIANGLES, 0, num);
}

unsigned int kmscon_context_new_tex(struct kmscon_context *ctx)
{
	GLuint tex = 0;

	if (!ctx)
		return tex;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	return tex;
}

void kmscon_context_free_tex(struct kmscon_context *ctx, unsigned int tex)
{
	if (!ctx)
		return;

	glDeleteTextures(1, &tex);
}

void kmscon_context_set_tex(struct kmscon_context *ctx, unsigned int tex,
			unsigned int width, unsigned int height, void *buf)
{
	if (!ctx || !buf || !width || !height)
		return;

	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0, GL_BGRA_EXT,
						GL_UNSIGNED_BYTE, buf);
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
