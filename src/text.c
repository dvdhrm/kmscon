/*
 * kmscon - Text Renderer
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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

/**
 * SECTION:text
 * @short_description: Text Renderer
 * @include: text.h
 *
 * TODO
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "static_misc.h"
#include "text.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "text"

struct text_backend {
	struct kmscon_dlist list;
	const struct kmscon_text_ops *ops;
};

static pthread_mutex_t text_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct kmscon_dlist text__list = KMSCON_DLIST_INIT(text__list);

static void text_lock()
{
	pthread_mutex_lock(&text_mutex);
}

static void text_unlock()
{
	pthread_mutex_unlock(&text_mutex);
}

/**
 * kmscon_text_register:
 * @ops: Text operations and name for new backend
 *
 * This register a new text backend with operations set to @ops. The name
 * @ops->name must be valid.
 *
 * The first font that is registered automatically becomes the default and
 * fallback. So make sure you register a safe fallback as first backend.
 * If this is unregistered, the next in the list becomes the default
 * and fallback.
 *
 * Returns: 0 on success, negative error code on failure
 */
int kmscon_text_register(const struct kmscon_text_ops *ops)
{
	struct kmscon_dlist *iter;
	struct text_backend *be;
	int ret;

	if (!ops || !ops->name)
		return -EINVAL;

	log_debug("register text backend %s", ops->name);

	text_lock();

	kmscon_dlist_for_each(iter, &text__list) {
		be = kmscon_dlist_entry(iter, struct text_backend, list);
		if (!strcmp(be->ops->name, ops->name)) {
			log_error("registering already available backend %s",
				  ops->name);
			ret = -EALREADY;
			goto out_unlock;
		}
	}

	be = malloc(sizeof(*be));
	if (!be) {
		log_error("cannot allocate memory for backend");
		ret = -ENOMEM;
		goto out_unlock;
	}

	memset(be, 0, sizeof(*be));
	be->ops = ops;
	kmscon_dlist_link(&text__list, &be->list);

	ret = 0;

out_unlock:
	text_unlock();
	return ret;
}

/**
 * kmscon_text_unregister:
 * @name: Name of backend
 *
 * This unregisters the text-backend that is registered with name @name. If
 * @name is not found, a warning is printed but nothing else is done.
 */
void kmscon_text_unregister(const char *name)
{
	struct kmscon_dlist *iter;
	struct text_backend *be;

	if (!name)
		return;

	log_debug("unregister backend %s", name);

	text_lock();

	kmscon_dlist_for_each(iter, &text__list) {
		be = kmscon_dlist_entry(iter, struct text_backend, list);
		if (strcmp(name, be->ops->name))
			continue;

		kmscon_dlist_unlink(&be->list);
		break;
	}

	if (iter == &text__list)
		be = NULL;

	text_unlock();

	if (!be) {
		log_error("cannot unregister backend %s: not found", name);
	} else {
		free(be);
	}
}

/**
 * kmscon_text_new:
 * @out: A pointer to the new text-renderer is stored here
 * @backend: Backend to use or NULL for default backend
 *
 * Returns: 0 on success, error code on failure
 */
int kmscon_text_new(struct kmscon_text **out,
		    const char *backend)
{
	struct kmscon_text *text;
	struct kmscon_dlist *iter;
	struct text_backend *be, *def;
	int ret;

	if (!out)
		return -EINVAL;

	text_lock();

	if (kmscon_dlist_empty(&text__list)) {
		log_error("no text backend available");
		ret = -EFAULT;
	} else {
		ret = 0;
		def = kmscon_dlist_entry(text__list.next,
					 struct text_backend,
					 list);
		if (!backend) {
			be = def;
		} else {
			kmscon_dlist_for_each(iter, &text__list) {
				be = kmscon_dlist_entry(iter,
							struct text_backend,
							list);
				if (!strcmp(backend, be->ops->name))
					break;
			}
			if (iter == &text__list) {
				log_warning("requested backend %s not found",
					    backend);
				be = def;
			}
		}
	}

	if (ret)
		goto out_unlock;

	text = malloc(sizeof(*text));
	if (!text) {
		log_error("cannot allocate memory for new text-renderer");
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(text, 0, sizeof(*text));
	text->ref = 1;
	text->ops = be->ops;

	ret = text->ops->init(text);
	if (ret) {
		if (be == def) {
			log_error("default backend %s cannot create renderer",
				  text->ops->name);
			goto err_free;
		}

		log_warning("backend %s cannot create renderer; trying default backend %s",
			    be->ops->name, def->ops->name);

		memset(text, 0, sizeof(*text));
		text->ref = 1;
		text->ops = def->ops;

		ret = text->ops->init(text);
		if (ret) {
			log_error("default backend %s cannot create renderer",
				  text->ops->name);
			goto err_free;
		}
	}

	log_debug("using: be: %s", text->ops->name);
	*out = text;
	ret = 0;
	goto out_unlock;

err_free:
	free(text);
out_unlock:
	text_unlock();
	return ret;
}

/**
 * kmscon_text_ref:
 * @text: Valid text-renderer object
 *
 * This increases the reference count of @text by one.
 */
void kmscon_text_ref(struct kmscon_text *text)
{
	if (!text || !text->ref)
		return;

	++text->ref;
}

/**
 * kmscon_text_unref:
 * @text: Valid text-renderer object
 *
 * This decreases the reference count of @text by one. If it drops to zero, the
 * object is freed.
 */
void kmscon_text_unref(struct kmscon_text *text)
{
	if (!text || !text->ref || --text->ref)
		return;

	text_lock();
	log_debug("freeing text renderer");
	text->ops->destroy(text);
	kmscon_font_unref(text->font);
	uterm_screen_unref(text->screen);
	free(text);
	text_unlock();
}

/**
 * kmscon_text_set_font:
 * @txt: Valid text-renderer object
 * @font: Valid font object
 *
 * This makes the text-renderer @txt use the font @font. You can drop your
 * reference to @font after calling this.
 */
void kmscon_text_set_font(struct kmscon_text *txt,
			  struct kmscon_font *font)
{
	if (!txt || !font)
		return;

	kmscon_font_unref(txt->font);
	txt->font = font;
	kmscon_font_ref(txt->font);

	txt->ops->new_font(txt);
}

/**
 * kmscon_text_set_bgcolor:
 * @txt: Valid text-renderer object
 * @r: red value
 * @g: green value
 * @b: blue value
 *
 * This sets the background color to r/g/b. The background color is a solid
 * color which is used for the whole background. You should give the same as the
 * default background color of your characters as this will speed up the drawing
 * operations.
 */
void kmscon_text_set_bgcolor(struct kmscon_text *txt,
			     uint8_t r, uint8_t g, uint8_t b)
{
	if (!txt)
		return;

	txt->bg_r = r;
	txt->bg_g = g;
	txt->bg_b = b;

	txt->ops->new_bgcolor(txt);
}

void kmscon_text_set_screen(struct kmscon_text *txt,
			    struct uterm_screen *screen)
{
	if (!txt || !screen)
		return;

	uterm_screen_unref(txt->screen);
	txt->screen = screen;
	uterm_screen_ref(txt->screen);

	txt->ops->new_screen(txt);
}

unsigned int kmscon_text_get_cols(struct kmscon_text *txt)
{
	if (!txt)
		return 0;

	return txt->cols;
}

unsigned int kmscon_text_get_rows(struct kmscon_text *txt)
{
	if (!txt)
		return 0;

	return txt->rows;
}

void kmscon_text_prepare(struct kmscon_text *txt)
{
	if (!txt || !txt->font || !txt->screen)
		return;

	txt->rendering = true;
	txt->ops->prepare(txt);
}

void kmscon_text_draw(struct kmscon_text *txt, kmscon_symbol_t ch,
		      unsigned int posx, unsigned int posy,
		      const struct font_char_attr *attr)
{
	if (!txt || !txt->rendering)
		return;
	if (posx >= txt->cols || posy >= txt->rows || !attr)
		return;

	txt->ops->draw(txt, ch, posx, posy, attr);
}

void kmscon_text_render(struct kmscon_text *txt)
{
	if (!txt || !txt->rendering)
		return;

	txt->ops->render(txt);
	txt->rendering = false;
}
