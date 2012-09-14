/*
 * kmscon - Font handling of Text Renderer
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
 * SECTION:text_font
 * @short_description: Font handling of text renderer
 * @include: text.h
 *
 * The text renderer needs a backend that draws glyphs which then can be shown
 * on the screen. This font handling subsystem provides a very simple API to
 * load arbitrary font-renderer backends. That is, you can choose from
 * in-memory bitmap fonts up to full Unicode compatible font libraries like
 * pango during runtime.
 *
 * This system does not provide any renderer by itself. You need to register one
 * of the available font-renderers first which then is used as backend for this
 * system. kmscon_font_register() and kmscon_font_unregister() can be used to
 * register font-renderers manually.
 *
 * @kmscon_font_attr is used to specify font-attributes for the fonts you want.
 * Please see kmscon_font_find() for more information on font-attributes. This
 * function returns a matching font which then can be used for drawing.
 * kmscon_font_ref()/kmscon_font_unref() are used for reference counting.
 * kmscon_font_render() renders a single unicode glyph and returns the glyph
 * buffer. kmscon_font_drop() frees this buffer again. A kmscon_glyph object
 * contains a memory-buffer with the renderered glyph plus some metrics like
 * height/width but also ascent/descent.
 *
 * Font-backends must take into account that this API must be thread-safe as it
 * is shared between different threads to reduce memory-footprint.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "static_misc.h"
#include "text.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "text_font"

struct font_backend {
	struct kmscon_dlist list;
	const struct kmscon_font_ops *ops;
};

static pthread_mutex_t font_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct kmscon_dlist font__list = KMSCON_DLIST_INIT(font__list);

static void font_lock()
{
	pthread_mutex_lock(&font_mutex);
}

static void font_unlock()
{
	pthread_mutex_unlock(&font_mutex);
}

/**
 * kmscon_font_attr_normalize:
 * @attr: Attribute to normalize
 *
 * This normalizes @attr and fills out missing entries. The following is done:
 * - If attr->name is empty, then it is set to KMSCON_FONT_DEFAULT_NAME
 * - If attr->ppi is 0, it is set to KMSCON_FONT_DEFAULT_PPI
 * - If attr->height is not set but attr->points is given, then attr->heights is
 *   calculated from attr->points.
 * - If attr->height is set, then attr->points is recalculated and overwritten
 *
 * The other fields are not changed. If attr->points is set but attr->height is
 * not set, then the height is calculated and after that the points are
 * recalculated so we will never have division-errors.
 */
void kmscon_font_attr_normalize(struct kmscon_font_attr *attr)
{
	if (!attr)
		return;

	if (!*attr->name)
		memcpy(attr->name, KMSCON_FONT_DEFAULT_NAME,
		       sizeof(KMSCON_FONT_DEFAULT_NAME));

	if (!attr->ppi)
		attr->ppi = KMSCON_FONT_DEFAULT_PPI;

	if (!attr->height && attr->points)
		attr->height = attr->points * attr->ppi / 72;
	if (attr->height)
		attr->points = attr->height * 72 / attr->ppi;
}

/**
 * kmscon_font_attr_match:
 * @a1: First attribute to match
 * @a2: Second attribute to match
 *
 * Compares @a1 and @a2 and returns true if they match. Both must be normalized
 * before comparing them, otherwise the comparison may return inexact results.
 * If width, height or *name is 0, then the fields are _not_ compared so you can
 * have wildmask matches.
 * points and dpi are never compared as the normalization already computes the
 * height correctly. So there is no need to use these.
 *
 * Returns: true if they match, otherwise false
 */
bool kmscon_font_attr_match(const struct kmscon_font_attr *a1,
			    const struct kmscon_font_attr *a2)
{
	if (!a1 || !a2)
		return false;

	if (a1->width && a2->width && a1->width != a2->width)
		return false;
	if (a1->height && a2->height && a1->height != a2->height)
		return false;
	if (a1->bold != a2->bold)
		return false;
	if (a1->italic != a2->italic)
		return false;
	if (*a1->name && *a2->name && strcmp(a1->name, a2->name))
		return false;

	return true;
}

/**
 * kmscon_font_register:
 * @ops: Font operations and name for new font backend
 *
 * This register a new font backend with operations set to @ops. The name
 * @ops->name must be valid.
 *
 * The first font that is registered automatically becomes the default font and
 * the fallback font. So make sure you register a safe fallback as first font.
 * If this font is unregistered, the next font in the list becomes the default
 * and fallback font.
 *
 * Returns: 0 on success, negative error code on failure
 */
int kmscon_font_register(const struct kmscon_font_ops *ops)
{
	struct kmscon_dlist *iter;
	struct font_backend *be;
	int ret;

	if (!ops || !ops->name)
		return -EINVAL;

	log_debug("register font backend %s", ops->name);

	font_lock();

	kmscon_dlist_for_each(iter, &font__list) {
		be = kmscon_dlist_entry(iter, struct font_backend, list);
		if (!strcmp(be->ops->name, ops->name)) {
			log_error("registering already available font backend %s",
				  ops->name);
			ret = -EALREADY;
			goto out_unlock;
		}
	}

	be = malloc(sizeof(*be));
	if (!be) {
		log_error("cannot allocate memory for font backend");
		ret = -ENOMEM;
		goto out_unlock;
	}

	memset(be, 0, sizeof(*be));
	be->ops = ops;
	kmscon_dlist_link(&font__list, &be->list);

	ret = 0;

out_unlock:
	font_unlock();
	return ret;
}

/**
 * kmscon_font_unregister:
 * @name: Name of font backend
 *
 * This unregisters the font-backend that is registered with name @name. If
 * @name is not found, a warning is printed but nothing else is done.
 */
void kmscon_font_unregister(const char *name)
{
	struct kmscon_dlist *iter;
	struct font_backend *be;

	if (!name)
		return;

	log_debug("unregister font backend %s", name);

	font_lock();

	kmscon_dlist_for_each(iter, &font__list) {
		be = kmscon_dlist_entry(iter, struct font_backend, list);
		if (strcmp(name, be->ops->name))
			continue;

		kmscon_dlist_unlink(&be->list);
		break;
	}

	if (iter == &font__list)
		be = NULL;

	font_unlock();

	if (!be) {
		log_error("cannot unregister font backend %s: not found", name);
	} else {
		free(be);
	}
}

/**
 * kmscon_font_find:
 * @out: A pointer to the new font is stored here
 * @attr: Attribute describing the font
 * @backend: Backend to use or NULL for default backend
 *
 * Lookup a font by the given attributes. It uses the font backend @backend. If
 * it is NULL, the default backend is used. If the given backend cannot find the
 * a suitable font, the fallback backend is tried. This backend should always
 * find a suitable font.
 *
 * Stores a pointer to the new font in @out and returns 0. Otherwise, @out is
 * not touched and an error is returned.
 *
 * The attributes in @attr are not always matched. There are even font backends
 * which have only one fixed font and always return this one so you cannot rely
 * on this behavior. That is, this function cannot be used to get an exact
 * match, it rather returns the best matching font.
 * There is currently no need to get an exact match so no API is available to
 * get this. Instead, you should always use the best match and the user must be
 * happy. We do print warnings if no close match can be found, though. The user
 * should read them if they want more information what font fallback was used.
 *
 * If this functions fails, you must not assume that there is another font that
 * might work. Moreover, you must not implement a fallback font yourself as this
 * is already implemented inside of this function! This function fails only due
 * to internal errors like failed memory allocations. If it fails, the chances
 * that you can allocate your own fallback font are pretty small so don't do it.
 *
 * About DPI and Point Sizes:
 * Many computer graphics systems use "Points" as measurement for font sizes.
 * However, most of them also use 72 or 96 as fixed DPI size for monitors. This
 * means, the Point sizes can be directly converted into pixels. But lets
 * look at the facts:
 *   1 Point is defined as 1/72 of an inch. That is, a 10 Point font will be
 *   exactly 10 / 72 inches, which is ~0.13889 inches, which is
 *   0.13889 * 2.54 cm, which is approximately 0.3528 cm. This applies to
 *   printed paper. If we want the same on a monitor, we must need more
 *   information. First, the monitor renders in pixels, that is, we must know
 *   how many Pixels per Inch (PPI) are displayed. Often the same information is
 *   given as Dots per Inch (DPI) but these two are identical in this context.
 *   If the DPI is 96, we know that our 10 Point font is 10 / 72 inches. Which
 *   then means it is 10 / 72 * 96 pixels, which is ~13.333 pixels. So we
 *   internally render the font with 13 pixels and display it as 13 pixels. This
 *   guarantees, that the font will be 10 Point big which means 0.3528 cm on the
 *   display. This of course requires that we know the exact PPI/DPI of the
 *   display.
 * But if we take into account that Windows uses fixed 96 PPI and Mac OS X 72
 * PPI (independent of the monitor), they drop all this information and instead
 * render the font in pixel sizes. Because if you use fixed 72 PPI, a 10 Point
 * font will always be 10 / 72 * 72 = 10 pixels high. This means, it would be
 * rather convenient to directly specify pixel-sizes on the monitor. If you want
 * to work with documents that shall be printed, you want to specify Points so
 * the printed result will look nice. But the disadvantage is, that your monitor
 * can print this font in the weirdest size if it uses PPI much bigger or lower
 * than the common 96 or 72. Therefore, if you work with a monitor you probably
 * want to also specify the pixel-height of the font as you probably don't know
 * the PPI of your monitor and don't want to do all that math in your head.
 * Therefore, for applications that will probably never print their output (like
 * the virtual (!) console this is for), it is often requested that we can
 * specify the pixel size instead of the Point size of a font so you can
 * predict the output better.
 * Hence, we provide both. If pixel information is given, that is, attr->height
 * is not 0, then we try to return a font with this pixel height.
 * If it is 0, attr->points is used together with attr->ppi to calculate the
 * pixel size. If attr->ppi is 0, then 72 is used.
 * After the font was chosen, all fields "points", "ppi", "height" and "width"
 * will contain the exact values for this font. If "ppi" was zero and pixel
 * sizes where specified, then the resulting "points" size is calculated with
 * "ppi" = 72 again. So if you use the "points" field please always specify
 * "ppi", either.
 *
 * Returns: 0 on success, error code on failure
 */
int kmscon_font_find(struct kmscon_font **out,
		     const struct kmscon_font_attr *attr,
		     const char *backend)
{
	struct kmscon_font *font;
	struct kmscon_dlist *iter;
	struct font_backend *be, *def;
	int ret;

	if (!out || !attr)
		return -EINVAL;

	font_lock();

	log_debug("searching for: be: %s nm: %s ppi: %u pt: %u b: %d i: %d he: %u wt: %u",
		  backend, attr->name, attr->ppi, attr->points,
		  attr->bold, attr->italic, attr->height,
		  attr->width);

	if (kmscon_dlist_empty(&font__list)) {
		log_error("no font backend available");
		ret = -EFAULT;
	} else {
		ret = 0;
		def = kmscon_dlist_entry(font__list.prev,
					 struct font_backend,
					 list);
		if (!backend) {
			be = def;
		} else {
			kmscon_dlist_for_each(iter, &font__list) {
				be = kmscon_dlist_entry(iter,
							struct font_backend,
							list);
				if (!strcmp(backend, be->ops->name))
					break;
			}
			if (iter == &font__list) {
				log_warning("requested backend %s not found",
					    backend);
				be = def;
			}
		}
	}

	if (ret)
		goto out_unlock;

	font = malloc(sizeof(*font));
	if (!font) {
		log_error("cannot allocate memory for new font");
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(font, 0, sizeof(*font));
	font->ref = 1;
	font->ops = be->ops;

	ret = font->ops->init(font, attr);
	if (ret) {
		if (be == def) {
			log_error("default backend %s cannot find font",
				  font->ops->name);
			goto err_free;
		}

		log_warning("backend %s cannot find font; trying default backend %s",
			    be->ops->name, def->ops->name);

		memset(font, 0, sizeof(*font));
		font->ref = 1;
		font->ops = def->ops;

		ret = font->ops->init(font, attr);
		if (ret) {
			log_error("default backend %s cannot find font",
				  font->ops->name);
			goto err_free;
		}
	}

	log_debug("using: be: %s nm: %s ppi: %u pt: %u b: %d i: %d he: %u wt: %u",
		  font->ops->name, font->attr.name, font->attr.ppi,
		  font->attr.points, font->attr.bold, font->attr.italic,
		  font->attr.height, font->attr.width);
	*out = font;
	ret = 0;
	goto out_unlock;

err_free:
	free(font);
out_unlock:
	font_unlock();
	return ret;
}

/**
 * kmscon_font_ref:
 * @font: Valid font object
 *
 * This increases the reference count of @font by one.
 */
void kmscon_font_ref(struct kmscon_font *font)
{
	if (!font || !font->ref)
		return;

	++font->ref;
}

/**
 * kmscon_font_unref:
 * @font: Valid font object
 *
 * This decreases the reference count of @font by one. If it drops to zero, the
 * object is freed.
 */
void kmscon_font_unref(struct kmscon_font *font)
{
	if (!font || !font->ref || --font->ref)
		return;

	font_lock();
	log_debug("freeing font");
	font->ops->destroy(font);
	free(font);
	font_unlock();
}

/**
 * kmscon_font_render:
 * @font: Valid font object
 * @sym: Symbol to find a glyph for
 * @out: Output buffer for glyph
 *
 * Renders the glyph for symbol @sym and places a pointer to the glyph in @out.
 * If the glyph cannot be found or is invalid, an error is returned. The glyph
 * is cached internally and removed when the last reference to this font is
 * dropped.
 * If the glyph is no available in this font-set, then -ERANGE is returned.
 *
 * Returns: 0 on success, negative error code on failure
 */
int kmscon_font_render(struct kmscon_font *font, tsm_symbol_t sym,
		       const struct kmscon_glyph **out)
{
	if (!font || !out)
		return -EINVAL;

	return font->ops->render(font, sym, out);
}

/**
 * kmscon_font_render_empty:
 * @font: Valid font object
 * @out: Output buffer for glyph
 *
 * Same as kmscon_font_render() but this renders a glyph that has no content and
 * can be used to blit solid backgrounds. That is, the resulting buffer will be
 * all 0 but the dimensions are the same as for all other glyphs.
 *
 * Returns: 0 on success, negative error code on failure
 */
int kmscon_font_render_empty(struct kmscon_font *font,
			     const struct kmscon_glyph **out)
{
	if (!font || !out)
		return -EINVAL;

	return font->ops->render_empty(font, out);
}

/**
 * kmscon_font_render_inval:
 * @font: Valid font object
 * @out: Output buffer for glyph
 *
 * Same sa kmscon_font_render_empty() but renders a glyph that can be used as
 * replacement for any other non-drawable glyph. That is, if
 * kmscon_font_render() returns -ERANGE, then this glyph can be used as
 * replacement.
 *
 * Returns: 0 on success ,engative error code on failure
 */
int kmscon_font_render_inval(struct kmscon_font *font,
			     const struct kmscon_glyph **out)
{
	if (!font || !out)
		return -EINVAL;

	return font->ops->render_inval(font, out);
}
