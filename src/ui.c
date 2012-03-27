/*
 * User Interface
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * User Interface
 * Implementation of the user interface.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "conf.h"
#include "eloop.h"
#include "font.h"
#include "input.h"
#include "log.h"
#include "terminal.h"
#include "ui.h"
#include "unicode.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "config"

struct kmscon_ui {
	struct ev_eloop *eloop;
	struct uterm_video *video;
	struct kmscon_input *input;
	struct kmscon_symbol_table *st;
	struct kmscon_font_factory *ff;
	struct kmscon_terminal *term;
};

static void video_event(struct uterm_video *video,
			struct uterm_video_hotplug *ev,
			void *data)
{
	struct kmscon_ui *ui = data;
	int ret;

	if (ev->action == UTERM_NEW) {
		if (uterm_display_get_state(ev->display) == UTERM_DISPLAY_INACTIVE) {
			ret = uterm_display_activate(ev->display, NULL);
			if (ret)
				return;
			ret = uterm_display_set_dpms(ev->display, UTERM_DPMS_ON);
			if (ret)
				return;
		}
		kmscon_terminal_add_display(ui->term, ev->display);
	}
}

static void input_event(struct kmscon_input *input,
			struct kmscon_input_event *ev,
			void *data)
{
}

int kmscon_ui_new(struct kmscon_ui **out,
			struct ev_eloop *eloop,
			struct uterm_video *video,
			struct kmscon_input *input)
{
	struct kmscon_ui *ui;
	int ret;

	if (!out || !eloop || !video || !input)
		return -EINVAL;

	ui = malloc(sizeof(*ui));
	if (!ui)
		return -ENOMEM;
	memset(ui, 0, sizeof(*ui));
	ui->eloop = eloop;
	ui->video = video;
	ui->input = input;

	ret = kmscon_symbol_table_new(&ui->st);
	if (ret)
		goto err_free;

	ret = kmscon_font_factory_new(&ui->ff, ui->st);
	if (ret)
		goto err_st;

	ret = kmscon_terminal_new(&ui->term, eloop, ui->st, ui->ff, ui->video,
					ui->input);
	if (ret)
		goto err_ff;

	ret = uterm_video_register_cb(ui->video, video_event, ui);
	if (ret)
		goto err_term;

	ret = kmscon_input_register_cb(ui->input, input_event, ui);
	if (ret)
		goto err_video;

	ret = kmscon_terminal_open(ui->term, NULL, NULL);
	if (ret)
		goto err_input;

	ev_eloop_ref(ui->eloop);
	uterm_video_ref(ui->video);
	kmscon_input_ref(ui->input);
	*out = ui;
	return 0;

err_input:
	kmscon_input_unregister_cb(ui->input, input_event, ui);
err_video:
	uterm_video_unregister_cb(ui->video, video_event, ui);
err_term:
	kmscon_terminal_unref(ui->term);
err_ff:
	kmscon_font_factory_unref(ui->ff);
err_st:
	kmscon_symbol_table_unref(ui->st);
err_free:
	free(ui);
	return ret;
}

void kmscon_ui_free(struct kmscon_ui *ui)
{
	if (!ui)
		return;

	kmscon_input_unregister_cb(ui->input, input_event, ui);
	uterm_video_unregister_cb(ui->video, video_event, ui);
	kmscon_terminal_unref(ui->term);
	kmscon_font_factory_unref(ui->ff);
	kmscon_symbol_table_unref(ui->st);
	kmscon_input_unref(ui->input);
	uterm_video_unref(ui->video);
	ev_eloop_unref(ui->eloop);
	free(ui);
}
