/*
 * test_terminal - Test Terminal
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
 * Test Terminal
 * This runs a terminal emulator with default settings on all connected outputs.
 * This is supposed to be a fully functional VT. It's only missing
 * configurability and extended features.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "eloop.h"
#include "input.h"
#include "log.h"
#include "output.h"
#include "terminal.h"
#include "vt.h"

struct app {
	struct kmscon_eloop *eloop;
	struct kmscon_signal *sig_term;
	struct kmscon_signal *sig_int;
	struct kmscon_symbol_table *st;
	struct kmscon_font_factory *ff;
	struct kmscon_compositor *comp;
	struct kmscon_input *input;
	struct kmscon_vt *vt;
	struct kmscon_terminal *term;
};

static volatile sig_atomic_t terminate;

static void sig_term(struct kmscon_signal *sig, int signum, void *data)
{
	terminate = 1;
}

static void read_input(struct kmscon_input *input,
				struct kmscon_input_event *ev, void *data)
{
	struct app *app = data;
	kmscon_symbol_t ch;

	if (ev->unicode == KMSCON_INPUT_INVALID)
		return;

	ch = kmscon_symbol_make(ev->unicode);
	kmscon_terminal_input(app->term, ch);
}

static void activate_outputs(struct app *app)
{
	struct kmscon_output *iter;
	int ret;

	iter = kmscon_compositor_get_outputs(app->comp);

	for ( ; iter; iter = kmscon_output_next(iter)) {
		if (!kmscon_output_is_active(iter)) {
			ret = kmscon_output_activate(iter, NULL);
			if (ret) {
				log_err("test: cannot activate output: %d\n",
									ret);
				continue;
			}
		}

		ret = kmscon_terminal_add_output(app->term, iter);
		if (ret) {
			log_err("test: cannot assign output: %d\n", ret);
			continue;
		}
	}
}

static bool vt_switch(struct kmscon_vt *vt, int action, void *data)
{
	struct app *app = data;
	int ret;

	if (action == KMSCON_VT_ENTER) {
		ret = kmscon_compositor_wake_up(app->comp);
		if (ret == 0)
			log_info("test: running without active outputs\n");
		else if (ret > 0)
			activate_outputs(app);

		kmscon_input_wake_up(app->input);
	} else if (action == KMSCON_VT_LEAVE) {
		kmscon_input_sleep(app->input);
		kmscon_terminal_rm_all_outputs(app->term);
		kmscon_compositor_sleep(app->comp);
	}

	return true;
}

static void destroy_app(struct app *app)
{
	kmscon_terminal_unref(app->term);
	kmscon_vt_unref(app->vt);
	kmscon_input_unref(app->input);
	kmscon_compositor_unref(app->comp);
	kmscon_font_factory_unref(app->ff);
	kmscon_symbol_table_unref(app->st);
	kmscon_eloop_rm_signal(app->sig_int);
	kmscon_eloop_rm_signal(app->sig_term);
	kmscon_eloop_unref(app->eloop);
}

static int setup_app(struct app *app)
{
	int ret;

	ret = kmscon_eloop_new(&app->eloop);
	if (ret)
		goto err_loop;

	ret = kmscon_eloop_new_signal(app->eloop, &app->sig_term, SIGTERM,
							sig_term, NULL);
	if (ret)
		goto err_loop;

	ret = kmscon_eloop_new_signal(app->eloop, &app->sig_int, SIGINT,
							sig_term, NULL);
	if (ret)
		goto err_loop;

	ret = kmscon_symbol_table_new(&app->st);
	if (ret)
		goto err_loop;

	ret = kmscon_font_factory_new(&app->ff, app->st);
	if (ret)
		goto err_loop;

	ret = kmscon_compositor_new(&app->comp);
	if (ret)
		goto err_loop;

	ret = kmscon_compositor_use(app->comp);
	if (ret)
		goto err_loop;

	ret = kmscon_input_new(&app->input);
	if (ret)
		goto err_loop;

	ret = kmscon_vt_new(&app->vt, vt_switch, app);
	if (ret)
		goto err_loop;

	ret = kmscon_vt_open(app->vt, KMSCON_VT_NEW, app->eloop);
	if (ret)
		goto err_loop;

	ret = kmscon_terminal_new(&app->term, app->ff, app->comp);
	if (ret)
		goto err_loop;

	ret = kmscon_terminal_connect_eloop(app->term, app->eloop);
	if (ret)
		goto err_loop;

	ret = kmscon_input_connect_eloop(app->input, app->eloop, read_input,
									app);
	if (ret)
		goto err_loop;

	return 0;

err_loop:
	destroy_app(app);
	return ret;
}

int main(int argc, char **argv)
{
	struct app app;
	int ret;

	log_info("test: starting\n");
	memset(&app, 0, sizeof(app));

	ret = setup_app(&app);
	if (ret)
		goto err_out;

	log_info("test: starting main-loop\n");

	while (!terminate) {
		ret = kmscon_eloop_dispatch(app.eloop, -1);
		if (ret)
			break;
	}

	log_info("test: stopping main-loop\n");

err_out:
	destroy_app(&app);

	if (ret) {
		log_err("test: failed with: %d\n", ret);
		return EXIT_FAILURE;
	} else {
		log_info("test: terminating\n");
		return EXIT_SUCCESS;
	}
}
