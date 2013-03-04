/*
 * kmscon - Fixed unifont font
 *
 * Copyright (c) 2012 Ted Kotz <ted@kotz.us>
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

/**
 * SECTION:font_unifont.c
 * @short_description: Fixed unifont font
 * @include: font.h
 *
 * This is a fixed font renderer backend that supports just one font which is
 * statically compiled into the file. This bitmap font has 8x16 and 16x16
 * glyphs. This can statically compile in any font defined as a unifont style
 * hex format. This font is from the GNU unifont project available at:
 *   http://unifoundry.com/unifont.html
 *
 * This file is heavily based on font_8x16.c
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_unifont"

/*
 * Glyph data is linked to the binary externally as binary data. The data layout
 * is a size-byte followed by 32 data bytes. The data bytes are padded with 0 if
 * the size is smaller than 32.
 * Sizes bigger than 32 are not used.
 */

struct unifont_data {
	uint8_t len;
	uint8_t data[32];
} __attribute__((__packed__));

extern const struct unifont_data _binary_src_font_unifont_data_bin_start[];
extern const struct unifont_data _binary_src_font_unifont_data_bin_end[];

/*
 * Global glyph cache
 * The linked binary glyph data cannot be directly passed to the caller as it
 * has the wrong format. Hence, use a glyph-cache with all used glyphs and add
 * new ones as soon as they are used.
 */

static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct shl_hashtable *cache;
static unsigned long cache_refnum;

static void cache_ref(void)
{
	pthread_mutex_lock(&cache_mutex);
	++cache_refnum;
	pthread_mutex_unlock(&cache_mutex);
}

static void cache_unref(void)
{
	pthread_mutex_lock(&cache_mutex);
	if (!--cache_refnum) {
		shl_hashtable_free(cache);
		cache = NULL;
	}
	pthread_mutex_unlock(&cache_mutex);
}

static void free_glyph(void *data)
{
	struct kmscon_glyph *g = data;

	free(g->buf.data);
	free(g);
}

static void unfold(uint8_t *dst, uint8_t val)
{
	*dst = 0xff * !!val;
}

static int find_glyph(uint32_t id, const struct kmscon_glyph **out)
{
	struct kmscon_glyph *g;
	int ret;
	bool res;
	const struct unifont_data *start, *end, *d;
	unsigned int i, w;

	pthread_mutex_lock(&cache_mutex);

	if (!cache) {
		ret = shl_hashtable_new(&cache, shl_direct_hash,
					shl_direct_equal, NULL, free_glyph);
		if (ret) {
			log_error("cannot create unifont hashtable: %d", ret);
			goto out_unlock;
		}
	} else {
		res = shl_hashtable_find(cache, (void**)out,
					 (void*)(long)id);
		if (res) {
			ret = 0;
			goto out_unlock;
		}
	}

	if (id > 0xffff) {
		ret = -ERANGE;
		goto out_unlock;
	}

	start = _binary_src_font_unifont_data_bin_start;
	end = _binary_src_font_unifont_data_bin_end;
	d = &start[id];

	if (d >= end) {
		ret = -ERANGE;
		goto out_unlock;
	}

	switch (d->len) {
	case 16:
		w = 1;
		break;
	case 32:
		w = 2;
		break;
	default:
		ret = -EFAULT;
		goto out_unlock;
	}

	g = malloc(sizeof(*g));
	if (!g) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(g, 0, sizeof(*g));
	g->width = w;
	g->buf.width = w * 8;
	g->buf.height = 16;
	g->buf.stride = w * 8;
	g->buf.format = UTERM_FORMAT_GREY;

	g->buf.data = malloc(g->buf.stride * g->buf.height);
	if (!g->buf.data) {
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < d->len; ++i) {
		unfold(&g->buf.data[i * 8 + 0], d->data[i] & 0x80);
		unfold(&g->buf.data[i * 8 + 1], d->data[i] & 0x40);
		unfold(&g->buf.data[i * 8 + 2], d->data[i] & 0x20);
		unfold(&g->buf.data[i * 8 + 3], d->data[i] & 0x10);
		unfold(&g->buf.data[i * 8 + 4], d->data[i] & 0x08);
		unfold(&g->buf.data[i * 8 + 5], d->data[i] & 0x04);
		unfold(&g->buf.data[i * 8 + 6], d->data[i] & 0x02);
		unfold(&g->buf.data[i * 8 + 7], d->data[i] & 0x01);
	}

	ret = shl_hashtable_insert(cache, (void*)(long)id, g);
	if (ret) {
		log_error("cannot insert glyph into glyph-cache: %d", ret);
		goto err_data;
	}

	*out = g;
	ret = 0;
	goto out_unlock;

err_data:
	free(g->buf.data);
err_free:
	free(g);
out_unlock:
	pthread_mutex_unlock(&cache_mutex);
	return ret;
}

static int kmscon_font_unifont_init(struct kmscon_font *out,
				    const struct kmscon_font_attr *attr)
{
	static const char name[] = "static-unifont";
	const struct unifont_data *start, *end;

	log_debug("loading static unifont font");

	start = _binary_src_font_unifont_data_bin_start;
	end = _binary_src_font_unifont_data_bin_end;
	if (start == end) {
		log_error("unifont glyph information not found in binary");
		return -EFAULT;
	}


	memset(&out->attr, 0, sizeof(out->attr));
	memcpy(out->attr.name, name, sizeof(name));
	out->attr.bold = false;
	out->attr.italic = false;
	out->attr.width = 8;
	out->attr.height = 16;
	kmscon_font_attr_normalize(&out->attr);
	out->baseline = 4;

	cache_ref();
	return 0;
}

static void kmscon_font_unifont_destroy(struct kmscon_font *font)
{
	log_debug("unloading static unifont font");
	cache_unref();
}

static int kmscon_font_unifont_render(struct kmscon_font *font, uint32_t id,
				      const uint32_t *ch, size_t len,
				      const struct kmscon_glyph **out)
{
	if (len > 1)
		return -ERANGE;

	return find_glyph(id, out);
}

static int kmscon_font_unifont_render_inval(struct kmscon_font *font,
					    const struct kmscon_glyph **out)
{
	return find_glyph(0xfffd, out);
}

static int kmscon_font_unifont_render_empty(struct kmscon_font *font,
					    const struct kmscon_glyph **out)
{
	return find_glyph(' ', out);
}

struct kmscon_font_ops kmscon_font_unifont_ops = {
	.name = "unifont",
	.owner = NULL,
	.init = kmscon_font_unifont_init,
	.destroy = kmscon_font_unifont_destroy,
	.render = kmscon_font_unifont_render,
	.render_empty = kmscon_font_unifont_render_empty,
	.render_inval = kmscon_font_unifont_render_inval,
};
