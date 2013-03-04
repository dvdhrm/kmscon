/*
 * kmscon - Freetype2 font backend
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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

/**
 * SECTION:font_freetype2.c
 * @short_description: Freetype2 font backend
 * @include: font.h
 *
 * The freetype2 backend uses freetype2 to render glyphs into memory
 * buffers. It uses a hashmap to cache all rendered glyphs of a single
 * font-face. Therefore, rendering should be very fast. Also, when loading a
 * glyph it pre-renders all common (mostly ASCII) characters, so it can measure
 * the font and return a valid font width.
 */

#include <errno.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_dlist.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "tsm_unicode.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_freetype2"

struct glyph {
	bool shrinked;
	unsigned int width;
};

struct face {
	unsigned long ref;
	struct shl_dlist list;

	bool shrink;
	struct kmscon_font_attr attr;
	struct kmscon_font_attr real_attr;
	unsigned int baseline;
	FT_Face face;
	pthread_mutex_t glyph_lock;
	struct shl_hashtable *glyphs;

	struct kmscon_glyph empty;
	struct kmscon_glyph inval;
};

static pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long manager__refcnt;
static FT_Library manager__lib;
static struct shl_dlist manager__list = SHL_DLIST_INIT(manager__list);

static void manager_lock()
{
	pthread_mutex_lock(&manager_mutex);
}

static void manager_unlock()
{
	pthread_mutex_unlock(&manager_mutex);
}

/* TODO: We currently load the default font-config configuration on start-up but
 * should probably provide a way to refresh it on SIGHUP or similar. Font-config
 * provides the FcInitBringUptoDate() or FcInitReinitialize() functions. */
static int manager__ref()
{
	FT_Error err;

	if (!manager__refcnt++) {
		err = FT_Init_FreeType(&manager__lib);
		if (err) {
			log_warn("cannot initialize freetype2");
			--manager__refcnt;
			return -EFAULT;
		}

		if (!FcInit()) {
			log_warn("cannot initialize fontconfig library");
			err = FT_Done_FreeType(manager__lib);
			if (err)
				log_warn("cannot deinitialize freetype2");
			--manager__refcnt;
			return -EFAULT;
		}
	}

	return 0;
}

static void manager__unref()
{
	FT_Error err;

	if (!--manager__refcnt) {
		/* FcFini() uses assert() to check whether all resources were
		 * correctly freed before FcFini() is called. As an emergency
		 * console, we cannot risk being killed because we have a small
		 * memory leak. Therefore, we rather skip deinitializing
		 * fontconfig and blame their authors here.
		 * Never ever use assert()/abort()/etc. in critical code paths!
		 * Bullshit...
		 * TODO: Fix upstream fontconfig to drop all those ugly
		 * assertions. */
		// FcFini();
		err = FT_Done_FreeType(manager__lib);
		if (err)
			log_warn("cannot deinitialize freetype2");
	}
}

static int get_glyph(struct face *face, struct kmscon_glyph **out,
		     uint32_t id, const uint32_t *ch, size_t len)
{
	struct kmscon_glyph *glyph;
	struct glyph *data;
	FT_Error err;
	FT_UInt idx;
	FT_Bitmap *bmap;
	FT_GlyphSlot slot;
	bool res;
	unsigned int i, j, wmax, hmax, idx1, idx2, cwidth;
	int ret, hoff1, hoff2, woff1, woff2;

	if (!len)
		return -ERANGE;
	cwidth = tsm_ucs4_get_width(*ch);
	if (!cwidth)
		return -ERANGE;

	pthread_mutex_lock(&face->glyph_lock);
	res = shl_hashtable_find(face->glyphs, (void**)&glyph,
				 (void*)(long)id);
	pthread_mutex_unlock(&face->glyph_lock);
	if (res) {
		*out = glyph;
		return 0;
	}

	manager_lock();

	glyph = malloc(sizeof(*glyph) + sizeof(struct glyph));
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(glyph, 0, sizeof(*glyph) + sizeof(struct glyph));
	glyph->data = (void*)(((uint8_t*)glyph) + sizeof(*glyph));
	data = glyph->data;
	glyph->width = cwidth;

	/* We currently ignore composed-symbols. That is, we only use the first
	 * UCS-4 code and draw this character. This works great for most simple
	 * ASCII characters but composed CJK characters often consist of
	 * multiple UCS-4 codes.
	 * TODO: Fix this by drawing all related characters into a single glyph
	 * and saving it or simply refer to the pango backend which already does
	 * that. */
	if (!*ch) {
		ret = -ERANGE;
		goto out_glyph;
	}

	idx = FT_Get_Char_Index(face->face, *ch);
	err = FT_Load_Glyph(face->face, idx, FT_LOAD_DEFAULT);
	if (err) {
		ret = -ERANGE;
		goto out_glyph;
	}

	err = FT_Render_Glyph(face->face->glyph, FT_RENDER_MODE_NORMAL);
	if (err) {
		ret = -ERANGE;
		goto out_glyph;
	}

	slot = face->face->glyph;
	bmap = &slot->bitmap;
	if (slot->format != FT_GLYPH_FORMAT_BITMAP ||
	    bmap->pixel_mode != FT_PIXEL_MODE_GRAY ||
	    bmap->num_grays != 256 ||
	    !bmap->rows ||
	    !bmap->width) {
		ret = -ERANGE;
		goto out_glyph;
	}

	data->width = bmap->width;
	glyph->buf.width = face->real_attr.width * cwidth;
	glyph->buf.height = face->real_attr.height;
	glyph->buf.stride = glyph->buf.width;
	glyph->buf.format = UTERM_FORMAT_GREY;
	glyph->buf.data = malloc(glyph->buf.stride * glyph->buf.height);
	if (!glyph->buf.data) {
		ret = -ENOMEM;
		goto out_glyph;
	}

	memset(glyph->buf.data, 0, glyph->buf.stride * glyph->buf.height);

	/* compute width-offsets and relative width-differences */
	if (slot->bitmap_left >= glyph->buf.width) {
		wmax = 0;
		woff1 = 0;
		woff2 = 0;
	} else if (slot->bitmap_left < 0) {
		if (glyph->buf.width > bmap->width)
			wmax = bmap->width;
		else
			wmax = glyph->buf.width;
		woff1 = 0;
		woff2 = 0;
	} else {
		wmax = glyph->buf.width - slot->bitmap_left;
		if (wmax > bmap->width)
			wmax = bmap->width;
		woff1 = slot->bitmap_left;
		woff2 = 0;
	}

	/* compute height-offsets and relative height-differences */
	hoff1 = (int)glyph->buf.height - face->baseline;
	if (hoff1 > slot->bitmap_top) {
		hoff1 -= slot->bitmap_top;
		hoff2 = 0;
	} else {
		hoff2 = slot->bitmap_top - hoff1;
		hoff1 = 0;
	}

	if (bmap->rows - hoff2 > glyph->buf.height - hoff1)
		hmax = glyph->buf.height - hoff1;
	else
		hmax = bmap->rows - hoff2;

	/* copy bitmap into glyph buffer */
	for (i = 0; i < hmax; ++i) {
		for (j = 0; j < wmax; ++j) {
			idx1 = (i + hoff1) * glyph->buf.stride + (j + woff1);
			idx2 = (i + hoff2) * bmap->pitch + (j + woff2);
			glyph->buf.data[idx1] = bmap->buffer[idx2];
		}
	}

	pthread_mutex_lock(&face->glyph_lock);
	ret = shl_hashtable_insert(face->glyphs, (void*)(long)id, glyph);
	pthread_mutex_unlock(&face->glyph_lock);
	if (ret) {
		log_error("cannot add glyph to hashtable");
		goto out_buffer;
	}

	*out = glyph;
	goto out_unlock;

out_buffer:
	free(glyph->buf.data);
out_glyph:
	free(glyph);
out_unlock:
	manager_unlock();
	return ret;
}

static void free_glyph(void *data)
{
	struct kmscon_glyph *glyph = data;

	free(glyph->buf.data);
	free(glyph);
}

static int manager_get_face(struct face **out, struct kmscon_font_attr *attr)
{
	struct shl_dlist *iter;
	struct face *face, *f;
	FcPattern *pat, *mat;
	FcResult res;
	FcChar8 *fname;
	FT_Error err;
	int ret, tmp, idx, weight, slant;
	double s, em, xsc, ysc;

	manager_lock();

	if (!attr->height) {
		ret = -EINVAL;
		goto out_unlock;
	}

	if (!attr->width)
		attr->width = attr->height;

	shl_dlist_for_each(iter, &manager__list) {
		face = shl_dlist_entry(iter, struct face, list);
		if (kmscon_font_attr_match(&face->attr, attr)) {
			++face->ref;
			*out = face;
			ret = 0;
			goto out_unlock;
		}
	}

	ret = manager__ref();
	if (ret)
		goto out_unlock;

	face = malloc(sizeof(*face));
	if (!face) {
		log_error("cannot allocate memory for new face");
		ret = -ENOMEM;
		goto err_manager;
	}
	memset(face, 0, sizeof(*face));
	face->ref = 1;
	memcpy(&face->attr, attr, sizeof(*attr));
	memcpy(&face->real_attr, attr, sizeof(*attr));

	ret = pthread_mutex_init(&face->glyph_lock, NULL);
	if (ret) {
		log_error("cannot initialize glyph lock");
		goto err_free;
	}

	ret = shl_hashtable_new(&face->glyphs, shl_direct_hash,
				shl_direct_equal, NULL, free_glyph);
	if (ret) {
		log_error("cannot allocate hashtable");
		goto err_lock;
	}

	s = face->attr.height;
	weight = face->attr.bold ? FC_WEIGHT_BOLD : FC_WEIGHT_NORMAL;
	slant = face->attr.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN;
	pat = FcPatternBuild(NULL, FC_FAMILY, FcTypeString, face->attr.name,
				   FC_PIXEL_SIZE, FcTypeDouble, s,
				   FC_WEIGHT, FcTypeInteger, weight,
				   FC_SLANT, FcTypeInteger, slant,
				   NULL);
	if (!pat) {
		log_error("cannot create font-config pattern");
		ret = -EFAULT;
		goto err_htable;
	}

	if (!FcConfigSubstitute(NULL, pat, FcMatchPattern)) {
		FcPatternDestroy(pat);
		log_error("cannot perform font-config substitutions");
		ret = -ENOMEM;
		goto err_htable;
	}

	res = FcResultMatch;
	mat = FcFontMatch(NULL, pat, &res);
	if (res != FcResultMatch) {
		if (mat)
			FcPatternDestroy(mat);
		FcPatternDestroy(pat);
		log_error("font-config cannot find font: %d", res);
		ret = -EFAULT;
		goto err_htable;
	}

	res = FcPatternGetString(mat, FC_FILE, 0, &fname);
	if (res != FcResultMatch) {
		FcPatternDestroy(mat);
		FcPatternDestroy(pat);
		log_error("font-config cannot find font (name)");
		ret = -EFAULT;
		goto err_htable;
	}
	res = FcPatternGetInteger(mat, FC_INDEX, 0, &idx);
	if (res != FcResultMatch) {
		FcPatternDestroy(mat);
		FcPatternDestroy(pat);
		log_error("font-config cannot find font (index)");
		ret = -EFAULT;
		goto err_htable;
	}

	log_debug("loading font %s:%d", (const char*)fname, idx);
	err = FT_New_Face(manager__lib, (const char*)fname, idx, &face->face);
	FcPatternDestroy(mat);
	FcPatternDestroy(pat);

	if (err) {
		if (err == FT_Err_Unknown_File_Format)
			log_error("unknown font file format");
		else
			log_error("cannot load font");

		ret = -EFAULT;
		goto err_htable;
	}

	if (!face->face->charmap) {
		log_warn("cannot load charmap of new font");
		ret = -EFAULT;
		goto err_face;
	}

	if (!FT_IS_SCALABLE(face->face)) {
		log_warn("non-scalable font");
		ret = -EFAULT;
		goto err_face;
	}

	err = FT_Set_Pixel_Sizes(face->face, face->attr.width,
				 face->attr.height);
	if (err) {
		log_warn("cannot set pixel size of font");
		ret = -EFAULT;
		goto err_face;
	}

	/* Every font provides an ascender/descender value which we use to
	 * compute glyph-height and the baseline offset. We need monospace fonts
	 * as we have the same fixed bounding-box for every glyph. However, if
	 * the font is not a monospace font, then the most straight-forward
	 * approach would be using the biggest bounding box. This, however, will
	 * not work as some characters are extremely wide and the text will look
	 * horrible. Therefore, we use the ascender/descender values provided
	 * with each font. This guarantees that special characters like
	 * line-segments are properly aligned without spacing. If the font does
	 * not provide proper asc/desc values, then freetype2 will return proper
	 * substitutions. */

	em = face->face->units_per_EM;
	xsc = face->face->size->metrics.x_ppem / em;
	ysc = face->face->size->metrics.y_ppem / em;

	tmp = face->face->descender * ysc;
	if (tmp > 0)
		tmp = 0;
	face->baseline = -tmp;

	tmp = face->face->ascender * ysc + face->baseline;
	if (tmp < 0 || tmp < face->baseline) {
		log_warn("invalid ascender/descender values for font");
		ret = -EFAULT;
		goto err_face;
	}
	face->real_attr.height = tmp;

	/* For font-width we use the biggest bounding-box-width. After the font
	 * has been loaded, this is cut down by pre-rendering some characters
	 * and computing a better average. */

	tmp = 1 + (int)(xsc * (face->face->bbox.xMax - face->face->bbox.xMin));
	if (tmp < 0)
		tmp = 0;
	face->real_attr.width = tmp;

	kmscon_font_attr_normalize(&face->real_attr);
	if (!face->real_attr.height || !face->real_attr.width) {
		log_warn("invalid scaled font sizes");
		ret = -EFAULT;
		goto err_face;
	}

	/* The real metrics probably differ from the requested metrics so try
	 * again to find a suitable cached font. */
	shl_dlist_for_each(iter, &manager__list) {
		f = shl_dlist_entry(iter, struct face, list);
		if (kmscon_font_attr_match(&f->real_attr, &face->real_attr)) {
			++f->ref;
			*out = f;
			ret = 0;
			goto err_face;
		}
	}

	shl_dlist_link(&manager__list, &face->list);
	*out = face;
	ret = 0;
	goto out_unlock;

err_face:
	FT_Done_Face(face->face);
err_htable:
	shl_hashtable_free(face->glyphs);
err_lock:
	pthread_mutex_destroy(&face->glyph_lock);
err_free:
	free(face);
err_manager:
	manager__unref();
out_unlock:
	manager_unlock();
	return ret;
}

static void manager_put_face(struct face *face)
{
	manager_lock();

	if (!--face->ref) {
		shl_dlist_unlink(&face->list);
		shl_hashtable_free(face->glyphs);
		pthread_mutex_destroy(&face->glyph_lock);
		FT_Done_Face(face->face);
		free(face);
		manager__unref();
	}

	manager_unlock();
}

static int generate_specials(struct face *face)
{
	size_t s;
	struct kmscon_glyph *g;
	int ret;
	static const uint32_t question_mark = '?';

	face->empty.width = 1;
	face->empty.data = NULL;
	face->empty.buf.width = face->real_attr.width;
	face->empty.buf.height = face->real_attr.height;
	face->empty.buf.stride = face->empty.buf.width;
	face->empty.buf.format = UTERM_FORMAT_GREY;
	s = face->empty.buf.stride * face->empty.buf.height;
	face->empty.buf.data = malloc(s);
	if (!face->empty.buf.data)
		return -ENOMEM;

	memset(face->empty.buf.data, 0, s);

	ret = get_glyph(face, &g, question_mark, &question_mark, 1);
	if (ret) {
		memcpy(&face->inval, &face->empty, sizeof(face->inval));
	} else {
		memcpy(&face->inval, g, sizeof(face->inval));
	}

	return 0;
}

static int kmscon_font_freetype2_init(struct kmscon_font *out,
				      const struct kmscon_font_attr *attr)
{
	struct face *face = NULL;
	int ret;
	unsigned int width;
	uint32_t i;
	struct kmscon_glyph *glyph;
	struct glyph *data;

	memcpy(&out->attr, attr, sizeof(*attr));
	kmscon_font_attr_normalize(&out->attr);

	log_debug("loading freetype2 font %s", out->attr.name);

	ret = manager_get_face(&face, &out->attr);
	if (ret)
		return ret;
	memcpy(&out->attr, &face->real_attr, sizeof(out->attr));
	out->baseline = face->baseline;

	/* Shrinking is done to get a better width-value for fonts. As not all
	 * fonts provide monospace-glyphs, we need to calculate a proper width
	 * by pre-rendering all ASCII characters and using the widest value.
	 * TODO: We should extend this with a better algorithm as there are
	 * common non-ASCII glyphs which are much wider.
	 * We enable shrinking by default as most fonts have a maximum-width
	 * which is about 3 times the size of 'M'.*/
	face->shrink = true;

	if (face->shrink) {
		width = 0;
		for (i = 0x20; i < 0x7f; ++i) {
			ret = get_glyph(face, &glyph, i, &i, 1);
			if (ret)
				continue;
			data = glyph->data;

			if (data->width > width)
				width = data->width;
		}

		if (!width) {
			log_warning("cannot measure font");
			face->shrink = false;
		} else if (width < face->real_attr.width) {
			face->real_attr.width = width;
			kmscon_font_attr_normalize(&face->real_attr);
			memcpy(&out->attr, &face->real_attr, sizeof(out->attr));
		}
	}

	/* generate inval/empty glyphs after shrinking */
	ret = generate_specials(face);
	if (ret)
		goto err_face;

	out->data = face;
	return 0;

err_face:
	manager_put_face(face);
	return ret;
}

static void kmscon_font_freetype2_destroy(struct kmscon_font *font)
{
	struct face *face;

	face = font->data;
	log_debug("unloading freetype2 font %s", face->real_attr.name);
	free(face->empty.buf.data);
	manager_put_face(face);
}

static int kmscon_font_freetype2_render(struct kmscon_font *font, uint32_t id,
					const uint32_t *ch, size_t len,
					const struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	struct glyph *data;
	struct face *face;
	int ret;

	ret = get_glyph(font->data, &glyph, id, ch, len);
	if (ret)
		return ret;

	face = font->data;
	data = glyph->data;
	if (face->shrink && !data->shrinked) {
		data->shrinked = true;
		glyph->buf.width = face->real_attr.width * glyph->width;
	}

	*out = glyph;
	return 0;
}

static int kmscon_font_freetype2_render_empty(struct kmscon_font *font,
					      const struct kmscon_glyph **out)
{
	struct face *face = font->data;

	*out = &face->empty;
	return 0;
}

static int kmscon_font_freetype2_render_inval(struct kmscon_font *font,
					      const struct kmscon_glyph **out)
{
	struct face *face = font->data;

	*out = &face->inval;
	return 0;
}

struct kmscon_font_ops kmscon_font_freetype2_ops = {
	.name = "freetype2",
	.owner = NULL,
	.init = kmscon_font_freetype2_init,
	.destroy = kmscon_font_freetype2_destroy,
	.render = kmscon_font_freetype2_render,
	.render_empty = kmscon_font_freetype2_render_empty,
	.render_inval = kmscon_font_freetype2_render_inval,
};
