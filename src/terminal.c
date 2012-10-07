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
#include "eloop.h"
#include "log.h"
#include "main.h"
#include "pty.h"
#include "shl_dlist.h"
#include "terminal.h"
#include "text.h"
#include "tsm_screen.h"
#include "tsm_vte.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "terminal"

struct screen {
	struct shl_dlist list;
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
	bool awake;

	struct shl_dlist screens;
	unsigned int min_cols;
	unsigned int min_rows;

	unsigned long fps;
	unsigned int redraw;
	struct ev_timer *redraw_timer;
	struct tsm_screen *console;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *ptyfd;

	kmscon_terminal_event_cb cb;
	void *data;
};

static void redraw(struct kmscon_terminal *term)
{
	struct uterm_screen *screen;
	struct shl_dlist *iter;
	struct screen *ent;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens) {
		ent = shl_dlist_entry(iter, struct screen, list);
		screen = ent->screen;

		tsm_screen_draw(term->console,
				    kmscon_text_prepare_cb,
				    kmscon_text_draw_cb,
				    kmscon_text_render_cb,
				    ent->txt);
		uterm_screen_swap(screen);
	}
}

static void redraw_timer_event(struct ev_timer *timer, uint64_t num, void *data)
{
	struct kmscon_terminal *term = data;

	/* When a redraw is scheduled, the redraw-counter is set to the current
	 * frame-rate. If this counter is full, we know that data changed and we
	 * need to redraw the terminal. We then decrement the counter until it
	 * drops to zero. This guarantees that we stay active for 1s without
	 * a call to ev_timer_enable/disable() which required syscalls which can
	 * be quite slow.
	 * If a redraw is schedule in the meantime, the counter is reset to the
	 * framerate and we have to redraw the screen. If it drops to zero, that
	 * is, 1s after the last redraw, we disable the timer to stop consuming
	 * CPU-power.
	 * TODO: On _really_ slow machines we might want to avoid fps-limits
	 * and redraw on change. We also should check whether waking up for the
	 * fps-timeouts for 1s is really faster than calling
	 * ev_timer_enable/disable() all the time. */

	if (num > 1)
		log_debug("CPU is too slow; skipping %" PRIu64 " frames", num - 1);

	if (term->redraw-- != term->fps) {
		if (!term->redraw) {
			ev_timer_disable(term->redraw_timer);
		}
		return;
	}

	redraw(term);
}

static void schedule_redraw(struct kmscon_terminal *term)
{
	if (!term->awake)
		return;

	if (!term->redraw) {
		ev_timer_enable(term->redraw_timer);
		ev_timer_drain(term->redraw_timer, NULL);
	}

	if (term->redraw < term->fps)
		term->redraw = term->fps;
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
	tsm_screen_resize(term->console, term->min_cols, term->min_rows);
	kmscon_pty_resize(term->pty, term->min_cols, term->min_rows);
}

static int add_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;
	int ret;
	unsigned int cols, rows;
	struct kmscon_font_attr attr = { "", 0, 20, false, false, 0, 0 };
	const char *be;

	attr.ppi = kmscon_conf.font_ppi;
	attr.points = kmscon_conf.font_size;
	strncpy(attr.name, kmscon_conf.font_name, KMSCON_FONT_MAX_NAME - 1);
	attr.name[KMSCON_FONT_MAX_NAME - 1] = 0;

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
	if (kmscon_conf.render_engine)
		be = kmscon_conf.render_engine;
	else if (!ret)
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

	shl_dlist_link(&term->screens, &scr->list);

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
	struct shl_dlist *iter;
	struct screen *ent;

	log_debug("destroying terminal screen %p", scr);
	shl_dlist_unlink(&scr->list);
	kmscon_text_unref(scr->txt);
	kmscon_font_unref(scr->font);
	uterm_screen_unref(scr->screen);
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
	free_screen(term, scr, true);
	if (shl_dlist_empty(&term->screens) && term->cb)
		term->cb(term, KMSCON_TERMINAL_NO_DISPLAY, term->data);
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	while ((iter = term->screens.next) != &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
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
		tsm_vte_input(term->vte, u8, len);
		schedule_redraw(term);
	}
}

static void pty_event(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_dispatch(term->pty);
}

static void input_event(struct uterm_input *input,
			struct uterm_input_event *ev,
			void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened || !term->awake)
		return;

	if (UTERM_INPUT_HAS_MODS(ev, kmscon_conf.grab_scroll_up->mods) &&
	    ev->keysym == kmscon_conf.grab_scroll_up->keysym) {
		tsm_screen_sb_up(term->console, 1);
		schedule_redraw(term);
		return;
	}
	if (UTERM_INPUT_HAS_MODS(ev, kmscon_conf.grab_scroll_down->mods) &&
	    ev->keysym == kmscon_conf.grab_scroll_down->keysym) {
		tsm_screen_sb_down(term->console, 1);
		schedule_redraw(term);
		return;
	}
	if (UTERM_INPUT_HAS_MODS(ev, kmscon_conf.grab_page_up->mods) &&
	    ev->keysym == kmscon_conf.grab_page_up->keysym) {
		tsm_screen_sb_page_up(term->console, 1);
		schedule_redraw(term);
		return;
	}
	if (UTERM_INPUT_HAS_MODS(ev, kmscon_conf.grab_page_down->mods) &&
	    ev->keysym == kmscon_conf.grab_page_down->keysym) {
		tsm_screen_sb_page_down(term->console, 1);
		schedule_redraw(term);
		return;
	}

	if (tsm_vte_handle_keyboard(term->vte, ev->keysym, ev->mods,
				       ev->unicode)) {
		tsm_screen_sb_reset(term->console);
		schedule_redraw(term);
	}
}

static void write_event(struct tsm_vte *vte, const char *u8, size_t len,
			void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

int kmscon_terminal_new(struct kmscon_terminal **out,
			struct ev_eloop *loop,
			struct uterm_input *input,
			const char *seat)
{
	struct kmscon_terminal *term;
	int ret;
	struct itimerspec spec;
	unsigned long fps;

	if (!out || !loop || !input)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->eloop = loop;
	term->input = input;
	shl_dlist_init(&term->screens);

	if (kmscon_conf.fps) {
		fps = 1000000000ULL / kmscon_conf.fps;
		if (fps == 0)
			fps = 1000000000ULL / 100;
		else if (fps > 200000000ULL)
			fps = 200000000ULL;
	} else {
		fps = 1000000000ULL / 25;
	}

	term->fps = 1000000000ULL / fps;
	log_debug("FPS: %lu TIMER: %lu", term->fps, fps);

	ret = tsm_screen_new(&term->console, log_llog);
	if (ret)
		goto err_free;
	tsm_screen_set_max_sb(term->console, kmscon_conf.sb_size);
	if (kmscon_conf.render_timing)
		tsm_screen_set_opts(term->console,
				    TSM_SCREEN_OPT_RENDER_TIMING);

	ret = tsm_vte_new(&term->vte, term->console, write_event, term,
			  log_llog);
	if (ret)
		goto err_con;
	tsm_vte_set_palette(term->vte, kmscon_conf.palette);

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret)
		goto err_vte;

	ret = kmscon_pty_set_term(term->pty, kmscon_conf.term);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_argv(term->pty, kmscon_conf.argv);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_seat(term->pty, seat);
	if (ret)
		goto err_pty;

	ret = ev_eloop_new_fd(term->eloop, &term->ptyfd,
			      kmscon_pty_get_fd(term->pty),
			      EV_READABLE, pty_event, term);
	if (ret)
		goto err_pty;

	ret = uterm_input_register_cb(term->input, input_event, term);
	if (ret)
		goto err_ptyfd;

	memset(&spec, 0, sizeof(spec));
	spec.it_value.tv_nsec = 1;
	spec.it_interval.tv_nsec = fps;

	ret = ev_timer_new(&term->redraw_timer, &spec, redraw_timer_event,
			   term, log_llog);
	if (ret)
		goto err_input;
	ev_timer_disable(term->redraw_timer);

	ret = ev_eloop_add_timer(term->eloop, term->redraw_timer);
	if (ret)
		goto err_timer;

	ev_eloop_ref(term->eloop);
	uterm_input_ref(term->input);
	*out = term;

	log_debug("new terminal object %p", term);
	return 0;

err_timer:
	ev_timer_unref(term->redraw_timer);
err_input:
	uterm_input_unregister_cb(term->input, input_event, term);
err_ptyfd:
	ev_eloop_rm_fd(term->ptyfd);
err_pty:
	kmscon_pty_unref(term->pty);
err_vte:
	tsm_vte_unref(term->vte);
err_con:
	tsm_screen_unref(term->console);
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
	ev_eloop_rm_timer(term->redraw_timer);
	ev_timer_unref(term->redraw_timer);
	uterm_input_unregister_cb(term->input, input_event, term);
	ev_eloop_rm_fd(term->ptyfd);
	kmscon_pty_unref(term->pty);
	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->console);
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
	tsm_vte_hard_reset(term->vte);
	width = tsm_screen_get_width(term->console);
	height = tsm_screen_get_height(term->console);
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

void kmscon_terminal_wake_up(struct kmscon_terminal *term)
{
	if (!term || term->awake)
		return;

	term->awake = true;
	schedule_redraw(term);
}

void kmscon_terminal_sleep(struct kmscon_terminal *term)
{
	if (!term || !term->awake)
		return;

	term->awake = false;
}

bool kmscon_terminal_is_awake(struct kmscon_terminal *term)
{
	return term && term->awake;
}
