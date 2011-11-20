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

#include "console.h"

/* maximum size of a single character */
#define KMSCON_CHAR_SIZE 6

struct kmscon_char {
	char *buf;
	size_t size;
	size_t len;
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
