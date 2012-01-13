/*
 * kmscon - Terminal
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
 * Terminal
 * A terminal gets assigned an input stream and several output objects and then
 * runs a fully functional terminal emulation on it.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "eloop.h"
#include "font.h"
#include "input.h"
#include "log.h"
#include "output.h"
#include "pty.h"
#include "terminal.h"
#include "unicode.h"
#include "vte.h"

struct term_out {
	struct term_out *next;
	struct kmscon_output *output;
};

struct kmscon_terminal {
	unsigned long ref;
	struct kmscon_eloop *eloop;
	struct kmscon_compositor *comp;

	struct term_out *outputs;
	unsigned int max_height;

	struct kmscon_console *console;
	struct kmscon_idle *redraw;
	struct kmscon_vte *vte;
	struct kmscon_pty *pty;

	kmscon_terminal_closed_cb closed_cb;
	void *closed_data;
};

static void draw_all(struct kmscon_idle *idle, void *data)
{
	struct kmscon_terminal *term = data;
	struct term_out *iter;
	struct kmscon_output *output;
	struct kmscon_context *ctx;
	int ret;

	ctx = kmscon_compositor_get_context(term->comp);
	kmscon_eloop_rm_idle(idle);

	iter = term->outputs;
	for (; iter; iter = iter->next) {
		output = iter->output;
		if (!kmscon_output_is_awake(output))
			continue;

		ret = kmscon_output_use(output);
		if (ret)
			continue;

		kmscon_context_clear(ctx);
		kmscon_console_map(term->console);
		kmscon_output_swap(output);
	}
}

static void schedule_redraw(struct kmscon_terminal *term)
{
	int ret;

	if (!term || !term->eloop)
		return;

	ret = kmscon_eloop_add_idle(term->eloop, term->redraw, draw_all, term);
	if (ret && ret != -EALREADY)
		log_warning("terminal: cannot schedule redraw\n");
}

static void pty_output(struct kmscon_pty *pty, char *u8, size_t len, void *data)
{
	size_t i;
	struct kmscon_terminal *term = data;

	/* FIXME: UTF-8. */
	for (i=0; i < len; i++)
		if (u8[i] < 128)
			kmscon_vte_input(term->vte, u8[i]);

	schedule_redraw(term);
}

int kmscon_terminal_new(struct kmscon_terminal **out,
	struct kmscon_font_factory *ff, struct kmscon_compositor *comp)
{
	struct kmscon_terminal *term;
	int ret;

	if (!out)
		return -EINVAL;

	log_debug("terminal: new terminal object\n");

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->comp = comp;

	ret = kmscon_idle_new(&term->redraw);
	if (ret)
		goto err_free;

	ret = kmscon_console_new(&term->console, ff, comp);
	if (ret)
		goto err_idle;

	ret = kmscon_vte_new(&term->vte);
	if (ret)
		goto err_con;
	kmscon_vte_bind(term->vte, term->console);

	ret = kmscon_pty_new(&term->pty, pty_output, term);
	if (ret)
		goto err_vte;

	kmscon_compositor_ref(term->comp);
	*out = term;

	return 0;

err_vte:
	kmscon_vte_unref(term->vte);
err_con:
	kmscon_console_unref(term->console);
err_idle:
	kmscon_idle_unref(term->redraw);
err_free:
	free(term);
	return ret;
}

void kmscon_terminal_ref(struct kmscon_terminal *term)
{
	if (!term)
		return;

	term->ref++;
}

void kmscon_terminal_unref(struct kmscon_terminal *term)
{
	if (!term || !term->ref)
		return;

	if (--term->ref)
		return;

	term->closed_cb = NULL;
	kmscon_terminal_close(term);
	kmscon_terminal_rm_all_outputs(term);
	kmscon_pty_unref(term->pty);
	kmscon_vte_unref(term->vte);
	kmscon_console_unref(term->console);
	kmscon_idle_unref(term->redraw);
	kmscon_compositor_unref(term->comp);
	free(term);
	log_debug("terminal: destroying terminal object\n");
}

int connect_eloop(struct kmscon_terminal *term, struct kmscon_eloop *eloop)
{
	if (!term || !eloop)
		return -EINVAL;

	if (term->eloop)
		return -EALREADY;

	kmscon_eloop_ref(eloop);
	term->eloop = eloop;

	return 0;
}

void disconnect_eloop(struct kmscon_terminal *term)
{
	if (!term)
		return;

	kmscon_eloop_unref(term->eloop);
	term->eloop = NULL;
}

static void pty_closed(struct kmscon_pty *pty, void *data)
{
	struct kmscon_terminal *term = data;
	kmscon_terminal_close(term);
}

int kmscon_terminal_open(struct kmscon_terminal *term,
				struct kmscon_eloop *eloop,
				kmscon_terminal_closed_cb closed_cb, void *data)
{
	int ret;
	unsigned short width, height;

	if (!term)
		return -EINVAL;

	ret = connect_eloop(term, eloop);
	if (ret == -EALREADY) {
		disconnect_eloop(term);
		ret = connect_eloop(term, eloop);
	}
	if (ret)
		return ret;

	width = kmscon_console_get_width(term->console);
	height = kmscon_console_get_height(term->console);
	ret = kmscon_pty_open(term->pty, eloop, width, height, pty_closed, term);
	if (ret) {
		disconnect_eloop(term);
		return ret;
	}

	term->closed_cb = closed_cb;
	term->closed_data = data;
	return 0;
}

void kmscon_terminal_close(struct kmscon_terminal *term)
{
	kmscon_terminal_closed_cb cb;
	void *data;

	if (!term)
		return;

	cb = term->closed_cb;
	data = term->closed_data;
	term->closed_data = NULL;
	term->closed_cb = NULL;

	disconnect_eloop(term);
	kmscon_pty_close(term->pty);

	if (cb)
		cb(term, data);
}

int kmscon_terminal_add_output(struct kmscon_terminal *term,
						struct kmscon_output *output)
{
	struct term_out *out;
	unsigned int height;
	struct kmscon_mode *mode;

	if (!term || !output)
		return -EINVAL;

	mode = kmscon_output_get_current(output);
	if (!mode) {
		log_warning("terminal: invalid output added to terminal\n");
		return -EINVAL;
	}

	out = malloc(sizeof(*out));
	if (!out)
		return -ENOMEM;

	memset(out, 0, sizeof(*out));
	kmscon_output_ref(output);
	out->output = output;
	out->next = term->outputs;
	term->outputs = out;

	height = kmscon_mode_get_height(mode);
	if (term->max_height < height) {
		term->max_height = height;
		kmscon_console_resize(term->console, 0, 0, term->max_height);
	}

	schedule_redraw(term);

	return 0;
}

void kmscon_terminal_rm_all_outputs(struct kmscon_terminal *term)
{
	struct term_out *tmp;

	if (!term)
		return;

	while (term->outputs) {
		tmp = term->outputs;
		term->outputs = tmp->next;
		kmscon_output_unref(tmp->output);
		free(tmp);
	}
}

void kmscon_terminal_input(struct kmscon_terminal *term, kmscon_symbol_t ch)
{
	/* FIXME: UTF-8. */
	if (ch < 128)
		kmscon_pty_input(term->pty, (char *)&ch, 1);
}
