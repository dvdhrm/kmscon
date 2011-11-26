/*
 * kmscon - Console Characters
 * Written 2011 by David Herrmann <dh.herrmann@googlemail.com>
 */

/*
 * Console Characters
 * A console always has a fixed width and height measured in number of
 * characters. This interfaces describes a single character.
 *
 * To be Unicode compatible, the most straightforward way would be using a UCS
 * number for each character and printing them. However, Unicode allows
 * combining marks, that is, a single printable character is constructed of
 * multiple characters. We support this by allowing to append characters to an
 * existing character. This should only be used with combining chars, though.
 * Otherwise you end up with multiple printable characters in a cell and the
 * output may get corrupted.
 *
 * We store each character (sequence) as UTF8 string because the pango library
 * accepts only UTF8. Hence, we avoid conversion to UCS or wide-characters.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <glib.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include "console.h"

/* maximum size of a single character */
#define KMSCON_CHAR_SIZE 6

struct kmscon_char {
	char *buf;
	size_t size;
	size_t len;
};

enum glyph_type {
	GLYPH_NONE,
	GLYPH_LAYOUT,
};

struct kmscon_glyph {
	size_t ref;
	struct kmscon_char *ch;

	int type;

	union {
		struct layout {
			PangoLayout *layout;
		} layout;
	} src;
};

struct kmscon_font {
	size_t ref;

	GHashTable *glyphs;
	PangoContext *ctx;
};

int kmscon_char_new(struct kmscon_char **out)
{
	struct kmscon_char *ch;

	if (!out)
		return -EINVAL;

	ch = malloc(sizeof(*ch));
	if (!ch)
		return -ENOMEM;

	memset(ch, 0, sizeof(*ch));

	ch->size = KMSCON_CHAR_SIZE;
	ch->buf = malloc(ch->size);
	if (!ch->buf) {
		free(ch);
		return -ENOMEM;
	}

	memset(ch->buf, 0, ch->size);

	*out = ch;
	return 0;
}

int kmscon_char_dup(struct kmscon_char **out, const struct kmscon_char *orig)
{
	struct kmscon_char *ch;

	if (!out || !orig)
		return -EINVAL;

	ch = malloc(sizeof(*ch));
	if (!ch)
		return -ENOMEM;

	memset(ch, 0, sizeof(*ch));

	ch->len = orig->len;
	ch->size = orig->size;
	ch->buf = malloc(ch->size);
	if (!ch->buf) {
		free(ch);
		return -ENOMEM;
	}

	memcpy(ch->buf, orig->buf, ch->size);

	*out = ch;
	return 0;
}

void kmscon_char_free(struct kmscon_char *ch)
{
	if (!ch)
		return;

	free(ch->buf);
	free(ch);
}

int kmscon_char_set_u8(struct kmscon_char *ch, const char *str, size_t len)
{
	char *buf;

	if (!ch)
		return -EINVAL;

	if (ch->size < len) {
		buf = realloc(ch->buf, len);
		if (!buf)
			return -ENOMEM;
		ch->buf = buf;
		ch->size = len;
	}

	memcpy(ch->buf, str, len);
	ch->len = len;

	return 0;
}

const char *kmscon_char_get_u8(const struct kmscon_char *ch)
{
	if (!ch || !ch->len)
		return NULL;

	return ch->buf;
}

size_t kmscon_char_get_len(const struct kmscon_char *ch)
{
	if (!ch)
		return 0;

	return ch->len;
}

int kmscon_char_append_u8(struct kmscon_char *ch, const char *str, size_t len)
{
	char *buf;
	size_t nlen;

	if (!ch)
		return -EINVAL;

	nlen = ch->len + len;

	if (ch->size < nlen) {
		buf = realloc(ch->buf, nlen);
		if (!buf)
			return -EINVAL;
		ch->buf = buf;
		ch->size = nlen;
	}

	memcpy(&ch->buf[ch->len], str, len);
	ch->len += len;

	return 0;
}

/*
 * Create a hash for a kmscon_char. This uses a simple hash technique described
 * by Daniel J. Bernstein.
 */
static guint kmscon_char_hash(gconstpointer key)
{
	guint val = 5381;
	size_t i;
	const struct kmscon_char *ch = (void*)key;

	for (i = 0; i < ch->len; ++i)
		val = val * 33 + ch->buf[i];

	return val;
}

/* compare two kmscon_char for equality */
static gboolean kmscon_char_equal(gconstpointer a, gconstpointer b)
{
	const struct kmscon_char *ch1 = (void*)a;
	const struct kmscon_char *ch2 = (void*)b;

	if (ch1->len != ch2->len)
		return FALSE;

	return (memcpy(ch1->buf, ch2->buf, ch1->len) == 0);
}

/*
 * Glyphs
 * Glyphs are for internal use only! The outside world uses kmscon_char
 * objects in combination with kmscon_font to draw characters. Internally, we
 * cache a kmscon_glyph for every character that is drawn.
 * This allows us to speed up the drawing operations because most characters are
 * already cached.
 *
 * Glyphs are cached in a hash-table by each font. If a character is drawn, we
 * look it up in the hash-table (or create a new one if none is found) and draw
 * it to the framebuffer.
 * A glyph may use several ways to cache the glyph description:
 *   GLYPH_NONE:
 *     No information is currently attached so the glyph cannot be drawn.
 *   GLYPH_LAYOUT:
 *     The most basic drawing operation. This is the slowest of all but can draw
 *     any text you want. It uses a PangoLayout internally and recalculates the
 *     character sizes each time we draw them.
 */
static int kmscon_glyph_new(struct kmscon_glyph **out,
						const struct kmscon_char *ch)
{
	struct kmscon_glyph *glyph;
	int ret;

	if (!out || !ch || !ch->len)
		return -EINVAL;

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;
	glyph->ref = 1;
	glyph->type = GLYPH_NONE;

	ret = kmscon_char_dup(&glyph->ch, ch);
	if (ret)
		goto err_free;

	*out = glyph;
	return 0;

err_free:
	free(glyph);
	return ret;
}

/*
 * Reset internal glyph description. You must use kmscon_glyph_set() again to
 * attach new glyph descriptions.
 */
static void kmscon_glyph_reset(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	switch (glyph->type) {
	case GLYPH_LAYOUT:
		g_object_unref(glyph->src.layout.layout);
		break;
	}

	glyph->type = GLYPH_NONE;
}

static void kmscon_glyph_ref(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	++glyph->ref;
}

static void kmscon_glyph_unref(struct kmscon_glyph *glyph)
{
	if (!glyph || !glyph->ref)
		return;

	if (--glyph->ref)
		return;

	kmscon_glyph_reset(glyph);
	kmscon_char_free(glyph->ch);
	free(glyph);
}

/*
 * Generate glyph description.
 * This connects the glyph with the given font an generates the fastest glyph
 * description.
 * Returns 0 on success.
 */
static int kmscon_glyph_set(struct kmscon_glyph *glyph,
						struct kmscon_font *font)
{
	PangoLayout *layout;

	if (!glyph || !font)
		return -EINVAL;

	layout = pango_layout_new(font->ctx);
	if (!layout)
		return -EINVAL;

	pango_layout_set_text(layout, glyph->ch->buf, glyph->ch->len);

	kmscon_glyph_reset(glyph);
	glyph->type = GLYPH_LAYOUT;
	glyph->src.layout.layout = layout;

	return 0;
}

/*
 * Creates a new font
 * Returns 0 on success and stores the new font in \out.
 */
int kmscon_font_new(struct kmscon_font **out)
{
	struct kmscon_font *font;
	int ret;
	PangoFontDescription *desc;
	PangoFontMap *map;
	PangoLanguage *lang;
	cairo_font_options_t *opt;

	if (!out)
		return -EINVAL;

	font = malloc(sizeof(*font));
	if (!font)
		return -ENOMEM;
	font->ref = 1;

	map = pango_cairo_font_map_get_default();
	if (!map) {
		ret = -EFAULT;
		goto err_free;
	}

	font->ctx = pango_font_map_create_context(map);
	if (!font->ctx) {
		ret = -EFAULT;
		goto err_free;
	}

	pango_context_set_base_dir(font->ctx, PANGO_DIRECTION_LTR);
	pango_cairo_context_set_resolution(font->ctx, 72);

	desc = pango_font_description_from_string("monospace 18");
	if (!desc) {
		ret = -EFAULT;
		goto err_ctx;
	}

	pango_context_set_font_description(font->ctx, desc);
	pango_font_description_free(desc);

	lang = pango_language_get_default();
	if (!lang) {
		ret = -EFAULT;
		goto err_ctx;
	}

	pango_context_set_language(font->ctx, lang);

	if (!pango_cairo_context_get_font_options(font->ctx)) {
		opt = cairo_font_options_create();
		if (!opt) {
			ret = -EFAULT;
			goto err_ctx;
		}

		pango_cairo_context_set_font_options(font->ctx, opt);
		cairo_font_options_destroy(opt);
	}

	font->glyphs = g_hash_table_new_full(kmscon_char_hash,
			kmscon_char_equal, (GDestroyNotify) kmscon_char_free,
					(GDestroyNotify) kmscon_glyph_unref);
	if (!font->glyphs) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	*out = font;
	return 0;

err_ctx:
	g_object_unref(font->ctx);
err_free:
	free(font);
	return ret;
}

void kmscon_font_ref(struct kmscon_font *font)
{
	if (!font)
		return;

	++font->ref;
}

void kmscon_font_unref(struct kmscon_font *font)
{
	if (!font || !font->ref)
		return;

	if (--font->ref)
		return;

	g_hash_table_unref(font->glyphs);
	g_object_unref(font->ctx);
	free(font);
}

/*
 * Get glyph for given key. If no glyph can be found in the hash-table, then a
 * new glyph is created and added to the hash-table.
 * Returns 0 on success and stores the glyph with a new reference in \out.
 */
static int kmscon_font_lookup(struct kmscon_font *font,
		const struct kmscon_char *key, struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	struct kmscon_char *ch;
	int ret;

	if (!font || !key || !out)
		return -EINVAL;

	glyph = g_hash_table_lookup(font->glyphs, key);
	if (!glyph) {
		ret = kmscon_char_dup(&ch, key);
		if (ret)
			return ret;

		ret = kmscon_glyph_new(&glyph, key);
		if (ret)
			goto err_char;

		ret = kmscon_glyph_set(glyph, font);
		if (ret)
			goto err_glyph;

		g_hash_table_insert(font->glyphs, ch, glyph);
	}


	kmscon_glyph_ref(glyph);
	*out = glyph;
	return 0;

err_glyph:
	kmscon_glyph_unref(glyph);
err_char:
	kmscon_char_free(ch);
	return ret;
}

/*
 * This draws a glyph for characters \ch into the given cairo context \cr.
 * The glyph will be drawn with the upper-left corner at x/y.
 * Returns 0 on success.
 */
int kmscon_font_draw(struct kmscon_font *font, const struct kmscon_char *ch,
					cairo_t *cr, uint32_t x, uint32_t y)
{
	struct kmscon_glyph *glyph;
	int ret;

	if (!font || !ch || !cr)
		return -EINVAL;

	ret = kmscon_font_lookup(font, ch, &glyph);
	if (ret)
		return ret;

	cairo_move_to(cr, x, y);

	switch (glyph->type) {
	case GLYPH_LAYOUT:
		pango_cairo_update_layout(cr, glyph->src.layout.layout);
		pango_cairo_show_layout(cr, glyph->src.layout.layout);
		break;
	default:
		ret = -EFAULT;
		break;
	}

	kmscon_glyph_unref(glyph);

	return 0;
}
