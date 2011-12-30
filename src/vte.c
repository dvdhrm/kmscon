/*
 * kmscon - VT Emulator
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
 * Virtual Terminal Emulator
 * This is a vt100 implementation. It is written from scratch. It uses the
 * console subsystem as output and is tightly bound to it.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "log.h"
#include "vte.h"

struct kmscon_vte {
	unsigned long ref;
	struct kmscon_console *con;
};

int kmscon_vte_new(struct kmscon_vte **out)
{
	struct kmscon_vte *vte;

	if (!out)
		return -EINVAL;

	log_debug("vte: new vte object\n");

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->ref = 1;

	*out = vte;
	return 0;
}

void kmscon_vte_ref(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

void kmscon_vte_unref(struct kmscon_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	kmscon_console_unref(vte->con);
	free(vte);
	log_debug("vte: destroying vte object\n");
}

void kmscon_vte_bind(struct kmscon_vte *vte, struct kmscon_console *con)
{
	if (!vte)
		return;

	kmscon_console_unref(vte->con);
	vte->con = con;
	kmscon_console_ref(vte->con);
}

void kmscon_vte_input(struct kmscon_vte *vte, const struct kmscon_char *ch)
{
	size_t len;
	const char *val;

	if (!vte || !vte->con)
		return;

	len = kmscon_char_get_len(ch);
	val = kmscon_char_get_u8(ch);

	if (len == 1) {
		if (*val == '\n')
			kmscon_console_newline(vte->con);
		else
			goto write_default;
	} else {
		goto write_default;
	}

	return;

write_default:
	kmscon_console_write(vte->con, ch);
}
