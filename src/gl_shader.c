/*
 * GL - Graphics Layer
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
#include "gl.h"
#include "log.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "gl"

/* Clear the GL error stack. The standard says that the error value is just a
 * single value and no list/stack. However, multiple error fields may be defined
 * and glGetError() returns only one of them until all are cleared. Hence, we
 * loop until no more error is retrieved.
 */
void gl_clear_error()
{
	GLenum err;

	do {
		err = glGetError();
	} while (err != GL_NO_ERROR);
}

/* return true if there is a pending GL error */
bool gl_has_error()
{
	GLenum err;

	err = glGetError();
	if (err != GL_NO_ERROR) {
		log_err("GL error %d", err);
		return true;
	}

	return false;
}

void gl_viewport(struct uterm_screen *screen)
{
	glViewport(0, 0,
			uterm_screen_width(screen),
			uterm_screen_height(screen));
}

unsigned int gl_tex_new()
{
	GLuint tex = 0;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	return tex;
}

void gl_tex_free(unsigned int tex)
{
	glDeleteTextures(1, &tex);
}

void gl_tex_load(unsigned int tex, unsigned int width, unsigned int stride,
			unsigned int height, void *buf)
{
	if (!buf || !width || !height)
		return;

	/* With OpenGL instead of OpenGLES2 we must use this on linux:
	 * glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA,
	 *              GL_UNSIGNED_BYTE, buf);
	 *
	 * TODO: Check what kind of stride we need to support here.
	 * GL_UNPACK_ROW_LENGTH only supports specifying a single row but
	 * doesn't allow pixel strides. cairo currently works fine without
	 * touching it but we should probably fix this properly.
	 */

	glBindTexture(GL_TEXTURE_2D, tex);
	/* glPixelStorei(GL_UNPACK_ROW_LENGTH, stride); */
	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, width, height, 0,
			GL_BGRA_EXT, GL_UNSIGNED_BYTE, buf);
	/* glPixelStorei(GL_UNPACK_ROW_LENGTH, 0); */
}

struct gl_shader {
	unsigned long ref;

	GLuint def_program;
	GLuint def_vshader;
	GLuint def_fshader;
	GLuint def_uni_projection;

	GLuint tex_program;
	GLuint tex_vshader;
	GLuint tex_fshader;
	GLuint tex_uni_projection;
	GLuint tex_uni_texture;
};

/* external shader sources; generated during build */
extern const char *kmscon_vert_def;
extern const char *kmscon_frag_def;
extern const char *kmscon_vert_tex;
extern const char *kmscon_frag_tex;

static int compile_shader(GLenum type, const char *source)
{
	char msg[512];
	GLint status = 1;
	GLuint s;

	s = glCreateShader(type);
	if (s == GL_NONE) {
		log_warn("cannot allocate GL shader");
		return GL_NONE;
	}

	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);

	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		glGetShaderInfoLog(s, sizeof(msg), NULL, msg);
		log_warn("cannot compile shader: %s", msg);
		return GL_NONE;
	}

	return s;
}

static int init_def_shader(struct gl_shader *shader)
{
	char msg[512];
	GLint status = 1;
	int ret;

	shader->def_vshader = compile_shader(GL_VERTEX_SHADER,
						kmscon_vert_def);
	if (shader->def_vshader == GL_NONE)
		return -EFAULT;

	shader->def_fshader = compile_shader(GL_FRAGMENT_SHADER,
						kmscon_frag_def);
	if (shader->def_fshader == GL_NONE) {
		ret = -EFAULT;
		goto err_vshader;
	}

	shader->def_program = glCreateProgram();
	glAttachShader(shader->def_program, shader->def_vshader);
	glAttachShader(shader->def_program, shader->def_fshader);
	glBindAttribLocation(shader->def_program, 0, "position");
	glBindAttribLocation(shader->def_program, 1, "color");

	glLinkProgram(shader->def_program);
	glGetProgramiv(shader->def_program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		glGetProgramInfoLog(shader->def_program, sizeof(msg),
					NULL, msg);
		log_warn("cannot link shader: %s", msg);
		ret = -EFAULT;
		goto err_link;
	}

	shader->def_uni_projection =
		glGetUniformLocation(shader->def_program, "projection");

	return 0;

err_link:
	glDeleteProgram(shader->def_program);
	glDeleteShader(shader->def_fshader);
err_vshader:
	glDeleteShader(shader->def_vshader);
	return ret;
}

static void free_def_shader(struct gl_shader *shader)
{
	glDeleteProgram(shader->def_program);
	glDeleteShader(shader->def_fshader);
	glDeleteShader(shader->def_vshader);
}

static int init_tex_shader(struct gl_shader *shader)
{
	char msg[512];
	GLint status = 1;
	int ret;

	shader->tex_vshader = compile_shader(GL_VERTEX_SHADER,
						kmscon_vert_tex);
	if (shader->tex_vshader == GL_NONE)
		return -EFAULT;

	shader->tex_fshader = compile_shader(GL_FRAGMENT_SHADER,
						kmscon_frag_tex);
	if (shader->tex_fshader == GL_NONE) {
		ret = -EFAULT;
		goto err_vshader;
	}

	shader->tex_program = glCreateProgram();
	glAttachShader(shader->tex_program, shader->tex_vshader);
	glAttachShader(shader->tex_program, shader->tex_fshader);
	glBindAttribLocation(shader->tex_program, 0, "position");
	glBindAttribLocation(shader->tex_program, 1, "texture_position");

	glLinkProgram(shader->tex_program);
	glGetProgramiv(shader->tex_program, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		msg[0] = 0;
		glGetProgramInfoLog(shader->tex_program, sizeof(msg),
					NULL, msg);
		log_warn("cannot link shader: %s", msg);
		ret = -EFAULT;
		goto err_link;
	}

	shader->tex_uni_projection =
		glGetUniformLocation(shader->tex_program, "projection");
	shader->tex_uni_texture =
		glGetUniformLocation(shader->tex_program, "texture");

	return 0;

err_link:
	glDeleteProgram(shader->tex_program);
	glDeleteShader(shader->tex_fshader);
err_vshader:
	glDeleteShader(shader->tex_vshader);
	return ret;
}

static void free_tex_shader(struct gl_shader *shader)
{
	glDeleteProgram(shader->tex_program);
	glDeleteShader(shader->tex_fshader);
	glDeleteShader(shader->tex_vshader);
}

int gl_shader_new(struct gl_shader **out)
{
	struct gl_shader *shader;
	int ret;

	if (!out)
		return -EINVAL;

	shader = malloc(sizeof(*shader));
	if (!shader)
		return -ENOMEM;
	memset(shader, 0, sizeof(*shader));
	shader->ref = 1;

	ret = init_def_shader(shader);
	if (ret)
		goto err_free;

	ret = init_tex_shader(shader);
	if (ret)
		goto err_def;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	log_debug("new shader object %p", shader);
	*out = shader;
	return 0;

err_def:
	free_def_shader(shader);
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

	log_debug("free shader object %p", shader);
	free_tex_shader(shader);
	free_def_shader(shader);
	free(shader);
}

void gl_shader_draw_def(struct gl_shader *shader, float *vertices,
			float *colors, size_t num)
{
	float m[16] = { 1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1 };

	if (!shader || !vertices || !colors || !num)
		return;

	glUseProgram(shader->def_program);
	glUniformMatrix4fv(shader->def_uni_projection, 1, GL_FALSE, m);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, colors);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, num);
}

void gl_shader_draw_tex(struct gl_shader *shader, const float *vertices,
			const float *texcoords, size_t num,
			unsigned int tex, const float *m)
{
	float mat[16];

	if (!shader || !vertices || !texcoords || !num || !m)
		return;

	gl_m4_transpose_dest(mat, m);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);

	glUseProgram(shader->tex_program);
	glUniformMatrix4fv(shader->tex_uni_projection, 1, GL_FALSE, mat);
	glUniform1i(shader->tex_uni_texture, 0);

	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, num);
}
