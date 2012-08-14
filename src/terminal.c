/*
 * kmscon - Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
#include "log.h"
#include "main.h"
#include "pty.h"
#include "static_misc.h"
#include "terminal.h"
#include "text.h"
#include "unicode.h"
#include "uterm.h"
#include "vte.h"

#define LOG_SUBSYSTEM "terminal"

struct screen {
	struct kmscon_dlist list;
	struct uterm_display *disp;
	struct uterm_screen *screen;
	struct kmscon_font *font;
	struct kmscon_text *txt;
};

struct kmscon_terminal {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct uterm_input *input;
	bool opened;

	struct kmscon_dlist screens;
	unsigned int min_cols;
	unsigned int min_rows;

	bool redraw;
	struct kmscon_console *console;
	struct kmscon_vte *vte;
	struct kmscon_pty *pty;

	kmscon_terminal_event_cb cb;
	void *data;
};

static void draw_all(struct ev_eloop *eloop, void *unused, void *data)
{
	struct kmscon_terminal *term = data;
	struct uterm_screen *screen;
	struct kmscon_dlist *iter;
	struct screen *ent;

	ev_eloop_unregister_idle_cb(term->eloop, draw_all, term);
	term->redraw = false;

	kmscon_dlist_for_each(iter, &term->screens) {
		ent = kmscon_dlist_entry(iter, struct screen, list);
		screen = ent->screen;

		kmscon_console_draw(term->console, ent->txt);
		uterm_screen_swap(screen);
	}
}

static void schedule_redraw(struct kmscon_terminal *term)
{
	int ret;

	if (term->redraw)
		return;

	ret = ev_eloop_register_idle_cb(term->eloop, draw_all, term);
	if (ret)
		log_warn("cannot schedule redraw");
	else
		term->redraw = true;
}

/*
 * Resize terminal
 * We support multiple monitors per terminal. As some software-rendering
 * backends to not support scaling, we always use the smalles cols/rows that are
 * provided so wider displays will have black margins.
 * This can be extended to support scaling but that would mean we need to check
 * whether the text-renderer backend supports that, first (TODO).
 *
 * If @force is true, then the console/pty are notified even though the size did
 * not changed. If @notify is false, then console/pty are not notified even
 * though the size might have changed. force = true and notify = false doesn't
 * make any sense, though.
 */
static void terminal_resize(struct kmscon_terminal *term,
			    unsigned int cols, unsigned int rows,
			    bool force, bool notify)
{
	bool resize = false;

	if (!term->min_cols || (cols > 0 && cols < term->min_cols)) {
		term->min_cols = cols;
		resize = true;
	}
	if (!term->min_rows || (rows > 0 && rows < term->min_rows)) {
		term->min_rows = rows;
		resize = true;
	}

	if (!notify || (!resize && !force))
		return;

	/* shrinking always succeeds */
	kmscon_console_resize(term->console, term->min_cols, term->min_rows);
	kmscon_pty_resize(term->pty, term->min_cols, term->min_rows);
}

static int add_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct kmscon_dlist *iter;
	struct screen *scr;
	int ret;
	unsigned int cols, rows;
	struct kmscon_font_attr attr = { "", 0, 20, false, false, 0, 0 };
	const char *be;

	attr.points = kmscon_conf.font_size;
	strncpy(attr.name, kmscon_conf.font_name, KMSCON_FONT_MAX_NAME - 1);
	attr.name[KMSCON_FONT_MAX_NAME - 1] = 0;

	kmscon_dlist_for_each(iter, &term->screens) {
		scr = kmscon_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			return 0;
	}

	scr = malloc(sizeof(*scr));
	if (!scr) {
		log_error("cannot allocate memory for display %p");
		return -ENOMEM;
	}
	memset(scr, 0, sizeof(*scr));
	scr->disp = disp;

	ret = uterm_screen_new_single(&scr->screen, disp);
	if (ret) {
		log_error("cannot create screen for display %p", scr->disp);
		goto err_free;
	}

	ret = kmscon_font_find(&scr->font, &attr, kmscon_conf.font_engine);
	if (ret) {
		log_error("cannot create font");
		goto err_screen;
	}

	ret = uterm_screen_use(scr->screen);
	if (!ret)
		be = "gltex";
	else
		be = NULL;

	ret = kmscon_text_new(&scr->txt, be);
	if (ret) {
		log_error("cannot create text-renderer");
		goto err_font;
	}

	ret = kmscon_text_set(scr->txt, scr->font, scr->screen);
	if (ret) {
		log_error("cannot set text-renderer parameters");
		goto err_text;
	}

	cols = kmscon_text_get_cols(scr->txt);
	rows = kmscon_text_get_rows(scr->txt);
	terminal_resize(term, cols, rows, false, true);

	kmscon_dlist_link(&term->screens, &scr->list);

	log_debug("added display %p to terminal %p", disp, term);
	schedule_redraw(term);
	uterm_display_ref(scr->disp);
	return 0;

err_text:
	kmscon_text_unref(scr->txt);
err_font:
	kmscon_font_unref(scr->font);
err_screen:
	uterm_screen_unref(scr->screen);
err_free:
	free(scr);
	return ret;
}

static void free_screen(struct kmscon_terminal *term, struct screen *scr,
			bool update)
{
	struct kmscon_dlist *iter;
	struct screen *ent;

	log_debug("destroying terminal screen %p", scr);
	kmscon_dlist_unlink(&scr->list);
	kmscon_text_unref(scr->txt);
	kmscon_font_unref(scr->font);
	uterm_screen_unref(scr->screen);
	uterm_display_unref(scr->disp);
	free(scr);

	if (!update)
		return;

	term->min_cols = 0;
	term->min_rows = 0;
	kmscon_dlist_for_each(iter, &term->screens) {
		ent = kmscon_dlist_entry(iter, struct screen, list);
		terminal_resize(term,
				kmscon_text_get_cols(ent->txt),
				kmscon_text_get_rows(ent->txt),
				false, false);
	}

	terminal_resize(term, 0, 0, true, true);
}

static void rm_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct kmscon_dlist *iter;
	struct screen *scr;

	kmscon_dlist_for_each(iter, &term->screens) {
		scr = kmscon_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			break;
	}

	if (iter == &term->screens)
		return;

	log_debug("removed display %p from terminal %p", disp, term);
	free_screen(term, scr, true);
	if (kmscon_dlist_empty(&term->screens) && term->cb)
		term->cb(term, KMSCON_TERMINAL_NO_DISPLAY, term->data);
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct kmscon_dlist *iter;
	struct screen *scr;

	while ((iter = term->screens.next) != &term->screens) {
		scr = kmscon_dlist_entry(iter, struct screen, list);
		free_screen(term, scr, false);
	}

	term->min_cols = 0;
	term->min_rows = 0;
}

static void pty_input(struct kmscon_pty *pty, const char *u8, size_t len,
								void *data)
{
	struct kmscon_terminal *term = data;

	if (!len) {
		if (term->cb)
			term->cb(term, KMSCON_TERMINAL_HUP, term->data);
	} else {
		kmscon_vte_input(term->vte, u8, len);
		schedule_redraw(term);
	}
}

static void input_event(struct uterm_input *input,
			struct uterm_input_event *ev,
			void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened)
		return;

	kmscon_vte_handle_keyboard(term->vte, ev);
}

static void write_event(struct kmscon_vte *vte, const char *u8, size_t len,
			void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

int kmscon_terminal_new(struct kmscon_terminal **out,
			struct ev_eloop *loop,
			struct uterm_input *input)
{
	struct kmscon_terminal *term;
	int ret;

	if (!out || !loop || !input)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->eloop = loop;
	term->input = input;
	kmscon_dlist_init(&term->screens);

	ret = kmscon_console_new(&term->console);
	if (ret)
		goto err_free;
	kmscon_console_set_max_sb(term->console, kmscon_conf.sb_size);

	ret = kmscon_vte_new(&term->vte, term->console, write_event, term);
	if (ret)
		goto err_con;

	ret = kmscon_pty_new(&term->pty, term->eloop, pty_input, term);
	if (ret)
		goto err_vte;

	ret = uterm_input_register_cb(term->input, input_event, term);
	if (ret)
		goto err_pty;

	ev_eloop_ref(term->eloop);
	uterm_input_ref(term->input);
	*out = term;

	log_debug("new terminal object %p", term);
	return 0;

err_pty:
	kmscon_pty_unref(term->pty);
err_vte:
	kmscon_vte_unref(term->vte);
err_con:
	kmscon_console_unref(term->console);
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

	log_debug("free terminal object %p", term);
	kmscon_terminal_close(term);
	rm_all_screens(term);
	uterm_input_unregister_cb(term->input, input_event, term);
	kmscon_pty_unref(term->pty);
	kmscon_vte_unref(term->vte);
	kmscon_console_unref(term->console);
	if (term->redraw)
		ev_eloop_unregister_idle_cb(term->eloop, draw_all, term);
	uterm_input_unref(term->input);
	ev_eloop_unref(term->eloop);
	free(term);
}

int kmscon_terminal_open(struct kmscon_terminal *term,
			kmscon_terminal_event_cb cb, void *data)
{
	int ret;
	unsigned short width, height;

	if (!term)
		return -EINVAL;

	kmscon_pty_close(term->pty);
	width = kmscon_console_get_width(term->console);
	height = kmscon_console_get_height(term->console);
	ret = kmscon_pty_open(term->pty, width, height);
	if (ret)
		return ret;

	term->opened = true;
	term->cb = cb;
	term->data = data;
	return 0;
}

void kmscon_terminal_close(struct kmscon_terminal *term)
{
	if (!term)
		return;

	kmscon_pty_close(term->pty);
	term->data = NULL;
	term->cb = NULL;
	term->opened = false;
}

void kmscon_terminal_redraw(struct kmscon_terminal *term)
{
	if (!term)
		return;

	schedule_redraw(term);
}

int kmscon_terminal_add_display(struct kmscon_terminal *term,
				struct uterm_display *disp)
{
	if (!term || !disp)
		return -EINVAL;

	return add_display(term, disp);
}

void kmscon_terminal_remove_display(struct kmscon_terminal *term,
				    struct uterm_display *disp)
{
	if (!term || !disp)
		return;

	rm_display(term, disp);
}
