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
	struct ev_eloop *eloop;
	struct kmscon_compositor *comp;

	struct term_out *outputs;
	unsigned int max_height;

	struct kmscon_console *console;
	struct ev_idle *redraw;
	struct kmscon_vte *vte;
	struct kmscon_pty *pty;

	kmscon_terminal_closed_cb closed_cb;
	void *closed_data;
};

static void draw_all(struct ev_idle *idle, void *data)
{
	struct kmscon_terminal *term = data;
	struct term_out *iter;
	struct kmscon_output *output;
	struct kmscon_context *ctx;
	int ret;

	ctx = kmscon_compositor_get_context(term->comp);
	ev_eloop_rm_idle(idle);

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

	ret = ev_eloop_add_idle(term->eloop, term->redraw, draw_all, term);
	if (ret && ret != -EALREADY)
		log_warn("terminal: cannot schedule redraw\n");
}

static void pty_input(struct kmscon_pty *pty, const char *u8, size_t len,
								void *data)
{
	struct kmscon_terminal *term = data;

	if (!len) {
		if (term->closed_cb)
			term->closed_cb(term, term->closed_data);
	} else {
		kmscon_vte_input(term->vte, u8, len);
		schedule_redraw(term);
	}
}

int kmscon_terminal_new(struct kmscon_terminal **out,
		struct ev_eloop *loop, struct kmscon_font_factory *ff,
		struct kmscon_compositor *comp, struct kmscon_symbol_table *st)
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
	term->eloop = loop;
	term->comp = comp;

	ret = ev_idle_new(&term->redraw);
	if (ret)
		goto err_free;

	ret = kmscon_console_new(&term->console, ff, comp);
	if (ret)
		goto err_idle;

	ret = kmscon_vte_new(&term->vte, st);
	if (ret)
		goto err_con;
	kmscon_vte_bind(term->vte, term->console);

	ret = kmscon_pty_new(&term->pty, term->eloop, pty_input, term);
	if (ret)
		goto err_vte;

	ev_eloop_ref(term->eloop);
	kmscon_compositor_ref(term->comp);
	*out = term;

	return 0;

err_vte:
	kmscon_vte_unref(term->vte);
err_con:
	kmscon_console_unref(term->console);
err_idle:
	ev_idle_unref(term->redraw);
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

	kmscon_terminal_close(term);
	kmscon_terminal_rm_all_outputs(term);
	kmscon_pty_unref(term->pty);
	kmscon_vte_unref(term->vte);
	kmscon_console_unref(term->console);
	ev_idle_unref(term->redraw);
	kmscon_compositor_unref(term->comp);
	ev_eloop_unref(term->eloop);
	free(term);
	log_debug("terminal: destroying terminal object\n");
}

int kmscon_terminal_open(struct kmscon_terminal *term,
			kmscon_terminal_closed_cb closed_cb, void *data)
{
	int ret;
	unsigned short width, height;

	if (!term)
		return -EINVAL;

	width = kmscon_console_get_width(term->console);
	height = kmscon_console_get_height(term->console);
	ret = kmscon_pty_open(term->pty, width, height);
	if (ret)
		return ret;

	term->closed_cb = closed_cb;
	term->closed_data = data;
	return 0;
}

void kmscon_terminal_close(struct kmscon_terminal *term)
{
	if (!term)
		return;

	kmscon_pty_close(term->pty);
	term->closed_data = NULL;
	term->closed_cb = NULL;
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
		log_warn("terminal: invalid output added to terminal\n");
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

int kmscon_terminal_input(struct kmscon_terminal *term,
					const struct kmscon_input_event *ev)
{
	int ret;
	const char *u8;
	size_t len;

	if (!term || !ev)
		return -EINVAL;

	ret = kmscon_vte_handle_keyboard(term->vte, ev, &u8, &len);
	switch (ret) {
		case KMSCON_VTE_SEND:
			ret = kmscon_pty_write(term->pty, u8, len);
			if (ret)
				return ret;
			break;
		case KMSCON_VTE_DROP:
		default:
			break;
	}

	return 0;
}
