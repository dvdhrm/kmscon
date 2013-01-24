/*
 * kmscon - Text Renderer
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Text Renderer
 * The Text-Renderer subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 */

#ifndef KMSCON_TEXT_H
#define KMSCON_TEXT_H

#include <errno.h>
#include <stdlib.h>
#include "font.h"
#include "kmscon_module.h"
#include "tsm_screen.h"
#include "uterm_video.h"

/* text renderer */

struct kmscon_text;
struct kmscon_text_ops;

struct kmscon_text {
	unsigned long ref;
	struct shl_register_record *record;
	const struct kmscon_text_ops *ops;
	void *data;

	struct kmscon_font *font;
	struct kmscon_font *bold_font;
	struct uterm_display *disp;
	unsigned int cols;
	unsigned int rows;
	bool rendering;
};

struct kmscon_text_ops {
	const char *name;
	struct kmscon_module *owner;
	int (*init) (struct kmscon_text *txt);
	void (*destroy) (struct kmscon_text *txt);
	int (*set) (struct kmscon_text *txt);
	void (*unset) (struct kmscon_text *txt);
	int (*prepare) (struct kmscon_text *txt);
	int (*draw) (struct kmscon_text *txt,
		     uint32_t id, const uint32_t *ch, size_t len,
		     unsigned int width,
		     unsigned int posx, unsigned int posy,
		     const struct tsm_screen_attr *attr);
	int (*render) (struct kmscon_text *txt);
	void (*abort) (struct kmscon_text *txt);
};

int kmscon_text_register(const struct kmscon_text_ops *ops);
void kmscon_text_unregister(const char *name);

int kmscon_text_new(struct kmscon_text **out, const char *backend);
void kmscon_text_ref(struct kmscon_text *txt);
void kmscon_text_unref(struct kmscon_text *txt);

int kmscon_text_set(struct kmscon_text *txt,
		    struct kmscon_font *font,
		    struct kmscon_font *bold_font,
		    struct uterm_display *disp);
void kmscon_text_unset(struct kmscon_text *txt);
unsigned int kmscon_text_get_cols(struct kmscon_text *txt);
unsigned int kmscon_text_get_rows(struct kmscon_text *txt);

int kmscon_text_prepare(struct kmscon_text *txt);
int kmscon_text_draw(struct kmscon_text *txt,
		     uint32_t id, const uint32_t *ch, size_t len,
		     unsigned int width,
		     unsigned int posx, unsigned int posy,
		     const struct tsm_screen_attr *attr);
int kmscon_text_render(struct kmscon_text *txt);
void kmscon_text_abort(struct kmscon_text *txt);

int kmscon_text_prepare_cb(struct tsm_screen *con, void *data);
int kmscon_text_draw_cb(struct tsm_screen *con,
			uint32_t id, const uint32_t *ch, size_t len,
			unsigned int width,
			unsigned int posx, unsigned int posy,
			const struct tsm_screen_attr *attr, void *data);
int kmscon_text_render_cb(struct tsm_screen *con, void *data);

/* modularized backends */

extern struct kmscon_text_ops kmscon_text_bblit_ops;
extern struct kmscon_text_ops kmscon_text_bbulk_ops;
extern struct kmscon_text_ops kmscon_text_gltex_ops;
extern struct kmscon_text_ops kmscon_text_cairo_ops;
extern struct kmscon_text_ops kmscon_text_pixman_ops;

#endif /* KMSCON_TEXT_H */
