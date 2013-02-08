/*
 * GL - Graphics Layer
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Shader API
 * This provides basic shader objects that are used to draw sprites and
 * textures.
 */

#define GL_GLEXT_PROTOTYPES

#include <errno.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_llog.h"
#include "static_gl.h"

#define LLOG_SUBSYSTEM "gl_shader"

struct gl_shader {
	unsigned long ref;
	llog_submit_t llog;
	void *llog_data;

	GLuint program;
	GLuint vshader;
	GLuint fshader;
};

/* Clear the GL error stack. The standard says that the error value is just a
 * single value and no list/stack. However, multiple error fields may be defined
 * and glGetError() returns only one of them until all are cleared. Hence, we
 * loop until no more errors are retrieved. */
void gl_clear_error()
{
	GLenum err;

	do {
		err = glGetError();
	} while (err != GL_NO_ERROR);
}

const char *gl_err_to_str(GLenum err)
{
	switch (err) {
	case GL_NO_ERROR:
		return "<NO_ERROR>";
	case GL_INVALID_ENUM:
		return "<INVALID_ENUM>";
	case GL_INVALID_VALUE:
		return "<INVALID_VALUE>";
	case GL_INVALID_OPERATION:
		return "<INVALID_OPERATION>";
#ifdef GL_STACK_OVERFLOW
	case GL_STACK_OVERFLOW:
		return "<STACK_OVERFLOW>";
#endif
#ifdef GL_STACK_UNDERFLOW
	case GL_STACK_UNDERFLOW:
		return "<STACK_UNDERFLOW>";
#endif
	case GL_OUT_OF_MEMORY:
		return "<OUT_OF_MEMORY>";
	default:
		return "<unknown>";
	}
}

/* return true if there is a pending GL error */
bool gl_has_error(struct gl_shader *shader)
{
	GLenum err;

	err = glGetError();
	if (err != GL_NO_ERROR) {
		llog_error(shader, "GL error %d: %s", err, gl_err_to_str(err));
		return true;
	}

	return false;
}

static int compile_shader(struct gl_shader *shader, GLenum type,
			  const char *source)
{
	char msg[512];
	GLint status = 1;
	GLuint s;

	s = glCreateShader(type);
	if (s == GL_NONE) {
		llog_warning(shader, "cannot allocate GL shader");
		return GL_NONE;
	}

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);

	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		glGetShaderInfoLog(s, sizeof(msg), NULL, msg);
		llog_warning(shader, "cannot compile shader: %s", msg);
		return GL_NONE;
	}

	return s;
}

int gl_shader_new(struct gl_shader **out, const char *vert, const char *frag,
		  char **attr, size_t attr_count, llog_submit_t llog,
		  void *llog_data)
{
	struct gl_shader *shader;
	int ret, i;
	char msg[512];
	GLint status = 1;

	if (!out || !vert || !frag)
		return -EINVAL;

	shader = malloc(sizeof(*shader));
	if (!shader)
		return -ENOMEM;
	memset(shader, 0, sizeof(*shader));
	shader->ref = 1;
	shader->llog = llog;
	shader->llog_data = llog_data;

	llog_debug(shader, "new shader");

	shader->vshader = compile_shader(shader, GL_VERTEX_SHADER, vert);
	if (shader->vshader == GL_NONE) {
		ret = -EFAULT;
		goto err_free;
	}

	shader->fshader = compile_shader(shader, GL_FRAGMENT_SHADER, frag);
	if (shader->fshader == GL_NONE) {
		ret = -EFAULT;
		goto err_vshader;
	}

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vshader);
	glAttachShader(shader->program, shader->fshader);

	for (i = 0; i < attr_count; ++i)
		glBindAttribLocation(shader->program, i, attr[i]);

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		glGetProgramInfoLog(shader->program, sizeof(msg), NULL, msg);
		llog_warning(shader, "cannot link shader: %s", msg);
		ret = -EFAULT;
		goto err_link;
	}

	if (gl_has_error(shader)) {
		llog_warning(shader, "shader creation failed");
		ret = -EFAULT;
		goto err_link;
	}

	*out = shader;
	return 0;

err_link:
	glDeleteProgram(shader->program);
	glDeleteShader(shader->fshader);
err_vshader:
	glDeleteShader(shader->vshader);
err_free:
	free(shader);
	return ret;
}

void gl_shader_ref(struct gl_shader *shader)
{
	if (!shader || !shader->ref)
		return;

	++shader->ref;
}

void gl_shader_unref(struct gl_shader *shader)
{
	if (!shader || !shader->ref || --shader->ref)
		return;

	llog_debug(shader, "free shader");

	glDeleteProgram(shader->program);
	glDeleteShader(shader->fshader);
	glDeleteShader(shader->vshader);
	free(shader);
}

GLuint gl_shader_get_uniform(struct gl_shader *shader, const char *name)
{
	if (!shader)
		return 0;

	return glGetUniformLocation(shader->program, name);
}

void gl_shader_use(struct gl_shader *shader)
{
	if (!shader)
		return;

	glUseProgram(shader->program);
}

void gl_tex_new(GLuint *tex, size_t num)
{
	size_t i;

	glGenTextures(num, tex);

	for (i = 0; i < num; ++i) {
		glBindTexture(GL_TEXTURE_2D, tex[i]);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
				GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
				GL_CLAMP_TO_EDGE);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
				GL_CLAMP_TO_EDGE);
	}
}

void gl_tex_free(GLuint *tex, size_t num)
{
	glDeleteTextures(num, tex);
}

void gl_tex_load(GLuint tex, unsigned int width, unsigned int stride,
		 unsigned int height, uint8_t *buf)
{
	if (!buf || !width || !height || !stride)
		return;

	/* With OpenGL instead of OpenGLES2 we must use this on linux:
	 * glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA,
	 *              GL_UNSIGNED_BYTE, buf);
	 *
	 * TODO: Check what kind of stride we need to support here.
	 * GL_UNPACK_ROW_LENGTH only supports specifying a single row but
	 * doesn't allow pixel strides. cairo currently works fine without
	 * touching it but we should probably fix this properly. */

	glBindTexture(GL_TEXTURE_2D, tex);
	/* glPixelStorei(GL_UNPACK_ROW_LENGTH, stride); */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0,
			GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf);
	/* glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); */
}
