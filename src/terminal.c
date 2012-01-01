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
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "eloop.h"
#include "log.h"
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

	struct term_out *outputs;
	unsigned int max_height;

	struct kmscon_console *console;
	struct kmscon_idle *redraw;
	struct kmscon_vte *vte;
};

static void draw_all(struct kmscon_idle *idle, void *data)
{
	struct kmscon_terminal *term = data;
	struct term_out *iter;
	struct kmscon_output *output;
	int ret;

	kmscon_eloop_rm_idle(idle);
	kmscon_console_draw(term->console);

	iter = term->outputs;
	for (; iter; iter = iter->next) {
		output = iter->output;
		if (!kmscon_output_is_awake(output))
			continue;

		ret = kmscon_output_use(output);
		if (ret)
			continue;

		glClearColor(0.0, 0.0, 0.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);

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

static const char help_text[] =
"terminal subsystem - KMS based console test\n"
"This is some default text to test the drawing operations.\n\n";

static void print_help(struct kmscon_terminal *term)
{
	unsigned int i, len;
	kmscon_symbol_t ch;

	len = sizeof(help_text) - 1;
	for (i = 0; i < len; ++i) {
		ch = kmscon_symbol_make(help_text[i]);
		kmscon_terminal_input(term, ch);
	}
}

int kmscon_terminal_new(struct kmscon_terminal **out,
						struct kmscon_symbol_table *st)
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

	ret = kmscon_idle_new(&term->redraw);
	if (ret)
		goto err_free;

	ret = kmscon_console_new(&term->console, st);
	if (ret)
		goto err_idle;

	ret = kmscon_vte_new(&term->vte);
	if (ret)
		goto err_con;
	kmscon_vte_bind(term->vte, term->console);
	print_help(term);

	*out = term;
	return 0;

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

	kmscon_terminal_rm_all_outputs(term);
	kmscon_vte_unref(term->vte);
	kmscon_console_unref(term->console);
	kmscon_terminal_disconnect_eloop(term);
	free(term);
	log_debug("terminal: destroying terminal object\n");
}

int kmscon_terminal_connect_eloop(struct kmscon_terminal *term,
						struct kmscon_eloop *eloop)
{
	if (!term || !eloop)
		return -EINVAL;

	if (term->eloop)
		return -EALREADY;

	kmscon_eloop_ref(eloop);
	term->eloop = eloop;

	return 0;
}

void kmscon_terminal_disconnect_eloop(struct kmscon_terminal *term)
{
	if (!term)
		return;

	kmscon_eloop_unref(term->eloop);
	term->eloop = NULL;
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
	kmscon_vte_input(term->vte, ch);
	schedule_redraw(term);
}
