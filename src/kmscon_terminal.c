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
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "conf.h"
#include "eloop.h"
#include "kmscon_conf.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "pty.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "text.h"
#include "tsm_screen.h"
#include "tsm_vte.h"
#include "uterm_input.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "terminal"

struct screen {
	struct shl_dlist list;
	struct kmscon_terminal *term;
	struct uterm_display *disp;
	struct kmscon_text *txt;

	bool swapping;
	bool pending;
};

struct kmscon_terminal {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct uterm_input *input;
	bool opened;
	bool awake;

	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct kmscon_session *session;

	struct shl_dlist screens;
	unsigned int min_cols;
	unsigned int min_rows;

	struct tsm_screen *console;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *ptyfd;

	struct kmscon_font_attr font_attr;
	struct kmscon_font *font;
	struct kmscon_font *bold_font;
};

static void do_clear_margins(struct screen *scr)
{
	unsigned int w, h, sw, sh;
	struct uterm_mode *mode;
	int dw, dh;

	mode = uterm_display_get_current(scr->disp);
	if (!mode)
		return;

	sw = uterm_mode_get_width(mode);
	sh = uterm_mode_get_height(mode);
	w = scr->txt->font->attr.width * scr->txt->cols;
	h = scr->txt->font->attr.height * scr->txt->rows;
	dw = sw - w;
	dh = sh - h;

	if (dw > 0)
		uterm_display_fill(scr->disp, 0, 0, 0,
				   w, 0,
				   dw, h);
	if (dh > 0)
		uterm_display_fill(scr->disp, 0, 0, 0,
				   0, h,
				   sw, dh);
}

static void do_redraw_screen(struct screen *scr)
{
	int ret;

	if (!scr->term->awake)
		return;

	scr->pending = false;
	do_clear_margins(scr);
	tsm_screen_draw(scr->term->console, kmscon_text_prepare_cb,
			kmscon_text_draw_cb, kmscon_text_render_cb, scr->txt);
	ret = uterm_display_swap(scr->disp, false);
	if (ret) {
		log_warning("cannot swap display %p", scr->disp);
		return;
	}

	scr->swapping = true;
}

static void redraw_screen(struct screen *scr)
{
	if (!scr->term->awake)
		return;

	if (scr->swapping)
		scr->pending = true;
	else
		do_redraw_screen(scr);
}

static void redraw_all(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		redraw_screen(scr);
	}
}

static void redraw_all_test(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		if (uterm_display_is_swapping(scr->disp))
			scr->swapping = true;
		redraw_screen(scr);
	}
}

static void display_event(struct uterm_display *disp,
			  struct uterm_display_event *ev, void *data)
{
	struct screen *scr = data;

	if (ev->action != UTERM_PAGE_FLIP)
		return;

	scr->swapping = false;
	if (scr->pending)
		do_redraw_screen(scr);
}

/*
 * Resize terminal
 * We support multiple monitors per terminal. As some software-rendering
 * backends to not support scaling, we always use the smallest cols/rows that are
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
	if (!term->min_cols || !term->min_rows)
		return;

	tsm_screen_resize(term->console, term->min_cols, term->min_rows);
	kmscon_pty_resize(term->pty, term->min_cols, term->min_rows);
	redraw_all(term);
}

static int font_set(struct kmscon_terminal *term)
{
	int ret;
	struct kmscon_font *font, *bold_font;
	struct shl_dlist *iter;
	struct screen *ent;

	term->font_attr.bold = false;
	ret = kmscon_font_find(&font, &term->font_attr,
			       term->conf->font_engine);
	if (ret)
		return ret;

	term->font_attr.bold = true;
	ret = kmscon_font_find(&bold_font, &term->font_attr,
			       term->conf->font_engine);
	if (ret) {
		log_warning("cannot create bold font: %d", ret);
		bold_font = font;
		kmscon_font_ref(bold_font);
	}

	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
	term->font = font;
	term->bold_font = bold_font;

	term->min_cols = 0;
	term->min_rows = 0;
	shl_dlist_for_each(iter, &term->screens) {
		ent = shl_dlist_entry(iter, struct screen, list);

		ret = kmscon_text_set(ent->txt, font, bold_font, ent->disp);
		if (ret)
			log_warning("cannot change text-renderer font: %d",
				    ret);

		terminal_resize(term,
				kmscon_text_get_cols(ent->txt),
				kmscon_text_get_rows(ent->txt),
				false, false);
	}

	terminal_resize(term, 0, 0, true, true);
	return 0;
}

static int add_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;
	int ret;
	const char *be;
	bool opengl;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			return 0;
	}

	scr = malloc(sizeof(*scr));
	if (!scr) {
		log_error("cannot allocate memory for display %p", disp);
		return -ENOMEM;
	}
	memset(scr, 0, sizeof(*scr));
	scr->term = term;
	scr->disp = disp;

	ret = uterm_display_register_cb(scr->disp, display_event, scr);
	if (ret) {
		log_error("cannot register display callback: %d", ret);
		goto err_free;
	}

	ret = uterm_display_use(scr->disp, &opengl);
	if (term->conf->render_engine)
		be = term->conf->render_engine;
	else if (ret >= 0 && opengl)
		be = "gltex";
	else
		be = "bbulk";

	ret = kmscon_text_new(&scr->txt, be);
	if (ret) {
		log_error("cannot create text-renderer");
		goto err_cb;
	}

	ret = kmscon_text_set(scr->txt, term->font, term->bold_font,
			      scr->disp);
	if (ret) {
		log_error("cannot set text-renderer parameters");
		goto err_text;
	}

	terminal_resize(term,
			kmscon_text_get_cols(scr->txt),
			kmscon_text_get_rows(scr->txt),
			false, true);

	shl_dlist_link(&term->screens, &scr->list);

	log_debug("added display %p to terminal %p", disp, term);
	redraw_screen(scr);
	uterm_display_ref(scr->disp);
	return 0;

err_text:
	kmscon_text_unref(scr->txt);
err_cb:
	uterm_display_unregister_cb(scr->disp, display_event, scr);
err_free:
	free(scr);
	return ret;
}

static void free_screen(struct screen *scr, bool update)
{
	struct shl_dlist *iter;
	struct screen *ent;
	struct kmscon_terminal *term = scr->term;

	log_debug("destroying terminal screen %p", scr);
	shl_dlist_unlink(&scr->list);
	kmscon_text_unref(scr->txt);
	uterm_display_unregister_cb(scr->disp, display_event, scr);
	uterm_display_unref(scr->disp);
	free(scr);

	if (!update)
		return;

	term->min_cols = 0;
	term->min_rows = 0;
	shl_dlist_for_each(iter, &term->screens) {
		ent = shl_dlist_entry(iter, struct screen, list);
		terminal_resize(term,
				kmscon_text_get_cols(ent->txt),
				kmscon_text_get_rows(ent->txt),
				false, false);
	}

	terminal_resize(term, 0, 0, true, true);
}

static void rm_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			break;
	}

	if (iter == &term->screens)
		return;

	log_debug("removed display %p from terminal %p", disp, term);
	free_screen(scr, true);
}

static void input_event(struct uterm_input *input,
			struct uterm_input_event *ev,
			void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened || !term->awake || ev->handled)
		return;

	if (conf_grab_matches(term->conf->grab_scroll_up,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_scroll_down,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_up,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_down,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_in,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.points + 1 < term->font_attr.points)
			return;

		++term->font_attr.points;
		if (font_set(term))
			--term->font_attr.points;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_out,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.points <= 1)
			return;

		--term->font_attr.points;
		if (font_set(term))
			++term->font_attr.points;
		return;
	}

	/* TODO: xkbcommon supports multiple keysyms, but it is currently
	 * unclear how this feature will be used. There is no keymap, which
	 * uses this, yet. */
	if (ev->num_syms > 1)
		return;

	if (tsm_vte_handle_keyboard(term->vte, ev->keysyms[0], ev->ascii,
				    ev->mods, ev->codepoints[0])) {
		tsm_screen_sb_reset(term->console);
		redraw_all(term);
		ev->handled = true;
	}
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	while ((iter = term->screens.next) != &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		free_screen(scr, false);
	}

	term->min_cols = 0;
	term->min_rows = 0;
}

static int terminal_open(struct kmscon_terminal *term)
{
	int ret;
	unsigned short width, height;

	if (term->opened)
		return -EALREADY;

	tsm_vte_hard_reset(term->vte);
	width = tsm_screen_get_width(term->console);
	height = tsm_screen_get_height(term->console);
	ret = kmscon_pty_open(term->pty, width, height);
	if (ret)
		return ret;

	term->opened = true;
	redraw_all(term);
	return 0;
}

static void terminal_close(struct kmscon_terminal *term)
{
	kmscon_pty_close(term->pty);
	term->opened = false;
}

static void terminal_destroy(struct kmscon_terminal *term)
{
	log_debug("free terminal object %p", term);

	terminal_close(term);
	rm_all_screens(term);
	uterm_input_unregister_cb(term->input, input_event, term);
	ev_eloop_rm_fd(term->ptyfd);
	kmscon_pty_unref(term->pty);
	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->console);
	uterm_input_unref(term->input);
	ev_eloop_unref(term->eloop);
	free(term);
}

static int session_event(struct kmscon_session *session,
			 struct kmscon_session_event *ev, void *data)
{
	struct kmscon_terminal *term = data;

	switch (ev->type) {
	case KMSCON_SESSION_DISPLAY_NEW:
		add_display(term, ev->disp);
		break;
	case KMSCON_SESSION_DISPLAY_GONE:
		rm_display(term, ev->disp);
		break;
	case KMSCON_SESSION_DISPLAY_REFRESH:
		redraw_all_test(term);
		break;
	case KMSCON_SESSION_ACTIVATE:
		term->awake = true;
		if (!term->opened)
			terminal_open(term);
		redraw_all_test(term);
		break;
	case KMSCON_SESSION_DEACTIVATE:
		term->awake = false;
		break;
	case KMSCON_SESSION_UNREGISTER:
		terminal_destroy(term);
		break;
	}

	return 0;
}

static void pty_input(struct kmscon_pty *pty, const char *u8, size_t len,
								void *data)
{
	struct kmscon_terminal *term = data;

	if (!len) {
		terminal_close(term);
		terminal_open(term);
	} else {
		tsm_vte_input(term->vte, u8, len);
		redraw_all(term);
	}
}

static void pty_event(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_dispatch(term->pty);
}

static void write_event(struct tsm_vte *vte, const char *u8, size_t len,
			void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

int kmscon_terminal_register(struct kmscon_session **out,
			     struct kmscon_seat *seat, unsigned int vtnr)
{
	struct kmscon_terminal *term;
	int ret;

	if (!out || !seat)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->eloop = kmscon_seat_get_eloop(seat);
	term->input = kmscon_seat_get_input(seat);
	shl_dlist_init(&term->screens);

	term->conf_ctx = kmscon_seat_get_conf(seat);
	term->conf = conf_ctx_get_mem(term->conf_ctx);

	strncpy(term->font_attr.name, term->conf->font_name,
		KMSCON_FONT_MAX_NAME - 1);
	term->font_attr.ppi = term->conf->font_ppi;
	term->font_attr.points = term->conf->font_size;

	ret = tsm_screen_new(&term->console, log_llog, NULL);
	if (ret)
		goto err_free;
	tsm_screen_set_max_sb(term->console, term->conf->sb_size);
	if (term->conf->render_timing)
		tsm_screen_set_opts(term->console,
				    TSM_SCREEN_OPT_RENDER_TIMING);

	ret = tsm_vte_new(&term->vte, term->console, write_event, term,
			  log_llog, NULL);
	if (ret)
		goto err_con;
	tsm_vte_set_palette(term->vte, term->conf->palette);

	ret = font_set(term);
	if (ret)
		goto err_vte;

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret)
		goto err_font;

	kmscon_pty_set_env_reset(term->pty, term->conf->reset_env);

	ret = kmscon_pty_set_term(term->pty, term->conf->term);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_colorterm(term->pty, "kmscon");
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_argv(term->pty, term->conf->argv);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_seat(term->pty, kmscon_seat_get_name(seat));
	if (ret)
		goto err_pty;

	if (vtnr > 0) {
		ret = kmscon_pty_set_vtnr(term->pty, vtnr);
		if (ret)
			goto err_pty;
	}

	ret = ev_eloop_new_fd(term->eloop, &term->ptyfd,
			      kmscon_pty_get_fd(term->pty),
			      EV_READABLE, pty_event, term);
	if (ret)
		goto err_pty;

	ret = uterm_input_register_cb(term->input, input_event, term);
	if (ret)
		goto err_ptyfd;

	ret = kmscon_seat_register_session(seat, &term->session, session_event,
					   term);
	if (ret) {
		log_error("cannot register session for terminal: %d", ret);
		goto err_input;
	}

	ev_eloop_ref(term->eloop);
	uterm_input_ref(term->input);
	*out = term->session;
	log_debug("new terminal object %p", term);
	return 0;

err_input:
	uterm_input_unregister_cb(term->input, input_event, term);
err_ptyfd:
	ev_eloop_rm_fd(term->ptyfd);
err_pty:
	kmscon_pty_unref(term->pty);
err_font:
	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
err_vte:
	tsm_vte_unref(term->vte);
err_con:
	tsm_screen_unref(term->console);
err_free:
	free(term);
	return ret;
}
