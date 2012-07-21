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
#include "font.h"
#include "gl.h"
#include "log.h"
#include "pty.h"
#include "terminal.h"
#include "unicode.h"
#include "uterm.h"
#include "vte.h"

#define LOG_SUBSYSTEM "terminal"

struct screen {
	struct screen *next;
	struct screen *prev;
	struct uterm_display *disp;
	struct uterm_screen *screen;
	struct font_buffer *buf;
	struct font_screen *fscr;
};

struct kmscon_terminal {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct uterm_video *video;
	struct uterm_input *input;
	struct gl_shader *shader;
	bool opened;

	struct screen *screens;
	unsigned int max_width;
	unsigned int max_height;

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
	struct screen *iter;
	struct uterm_screen *screen;
	int ret;
	unsigned int cflags;

	ev_eloop_unregister_idle_cb(term->eloop, draw_all, term);
	term->redraw = false;
	cflags = kmscon_console_get_flags(term->console);

	iter = term->screens;
	for (; iter; iter = iter->next) {
		screen = iter->screen;

		ret = uterm_screen_use(screen);
		if (!ret) {
			gl_viewport(screen);
			if (cflags & KMSCON_CONSOLE_INVERSE)
				gl_clear_color(1.0, 1.0, 1.0, 1.0);
			else
				gl_clear_color(0.0, 0.0, 0.0, 1.0);
			gl_clear();
		}
		kmscon_console_draw(term->console, iter->fscr);
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

static int add_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct screen *scr;
	int ret;
	unsigned int width, height;
	struct screen *iter;

	for (iter = term->screens; iter; iter = iter->next) {
		if (iter->disp == disp)
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

	width = uterm_screen_width(scr->screen);
	height = uterm_screen_height(scr->screen);

	ret = font_buffer_new(&scr->buf, width, height);
	if (ret) {
		log_error("cannot create buffer for display %p", scr->disp);
		goto err_screen;
	}

	ret = font_screen_new_fixed(&scr->fscr, scr->buf, FONT_ATTR(NULL, 12, 0),
				80, 24, scr->screen, term->shader);
	if (ret) {
		log_error("cannot create font for display %p", scr->disp);
		goto err_buf;
	}

	scr->next = term->screens;
	if (scr->next)
		scr->next->prev = scr;
	term->screens = scr;

	log_debug("added display %p to terminal %p", disp, term);
	schedule_redraw(term);
	uterm_display_ref(scr->disp);
	return 0;

err_buf:
	font_buffer_free(scr->buf);
err_screen:
	uterm_screen_unref(scr->screen);
err_free:
	free(scr);
	return ret;
}

static void free_screen(struct screen *scr)
{
	log_debug("destroying terminal screen %p", scr);
	font_screen_free(scr->fscr);
	font_buffer_free(scr->buf);
	uterm_screen_unref(scr->screen);
	uterm_display_unref(scr->disp);
	free(scr);
}

static void rm_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct screen *scr;

	for (scr = term->screens; scr; scr = scr->next) {
		if (scr->disp == disp) {
			if (scr->prev)
				scr->prev->next = scr->next;
			if (scr->next)
				scr->next->prev = scr->prev;
			if (term->screens == scr)
				term->screens = scr->next;
			break;
		}
	}

	if (!scr)
		return;

	log_debug("removed display %p from terminal %p", disp, term);
	free_screen(scr);
	if (!term->screens && term->cb)
		term->cb(term, KMSCON_TERMINAL_NO_DISPLAY, term->data);
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct screen *scr;

	while ((scr = term->screens)) {
		term->screens = scr->next;
		free_screen(scr);
	}
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

static void video_event(struct uterm_video *video,
			struct uterm_video_hotplug *ev,
			void *data)
{
	struct kmscon_terminal *term = data;

	if (ev->action == UTERM_GONE)
		rm_display(term, ev->display);
	else if (ev->action == UTERM_WAKE_UP)
		schedule_redraw(term);
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
			struct uterm_video *video,
			struct uterm_input *input)
{
	struct kmscon_terminal *term;
	int ret;

	if (!out || !loop || !video || !input)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->eloop = loop;
	term->video = video;
	term->input = input;

	ret = kmscon_console_new(&term->console);
	if (ret)
		goto err_free;

	ret = kmscon_vte_new(&term->vte, term->console, write_event, term);
	if (ret)
		goto err_con;

	ret = kmscon_pty_new(&term->pty, term->eloop, pty_input, term);
	if (ret)
		goto err_vte;

	ret = uterm_video_use(term->video);
	if (!ret) {
		ret = gl_shader_new(&term->shader);
		if (ret)
			goto err_pty;
	}

	ret = uterm_video_register_cb(term->video, video_event, term);
	if (ret)
		goto err_shader;

	ret = uterm_input_register_cb(term->input, input_event, term);
	if (ret)
		goto err_video;

	ev_eloop_ref(term->eloop);
	uterm_video_ref(term->video);
	uterm_input_ref(term->input);
	*out = term;

	log_debug("new terminal object %p", term);
	return 0;

err_video:
	uterm_video_unregister_cb(term->video, video_event, term);
err_shader:
	gl_shader_unref(term->shader);
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
	uterm_video_unregister_cb(term->video, video_event, term);
	gl_shader_unref(term->shader);
	kmscon_pty_unref(term->pty);
	kmscon_vte_unref(term->vte);
	kmscon_console_unref(term->console);
	if (term->redraw)
		ev_eloop_unregister_idle_cb(term->eloop, draw_all, term);
	uterm_input_unref(term->input);
	uterm_video_unref(term->video);
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
