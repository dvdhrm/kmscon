/*
 * test_console - Test Console
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
 * Test Console
 * This prints a console onto all available outputs. The console is not
 * interactive, but instead all input from stdin is read and printed as
 * printable characters onto the console.
 * This is no terminal emulation but instead an example how to print text with
 * the console subsystem.
 *
 * This prints all text from stdin to all connected outputs:
 * $ ./test_console
 *
 * This prints the text from the command "ls -la" to all outptus:
 * $ ls -la | ./test_console
 */

#define _BSD_SOURCE
#define GL_GLEXT_PROTOTYPES

#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include "console.h"
#include "eloop.h"
#include "log.h"
#include "output.h"
#include "vt.h"

static volatile sig_atomic_t terminate;

struct console {
	struct kmscon_eloop *loop;
	struct kmscon_signal *sig_term;
	struct kmscon_signal *sig_int;
	struct kmscon_compositor *comp;
	struct kmscon_vt *vt;
	struct kmscon_console *con;
	struct kmscon_idle *idle;

	uint32_t max_x;
	uint32_t max_y;
};

static void map_outputs(struct console *con)
{
	int ret;
	struct kmscon_output *iter;

	if (kmscon_compositor_is_asleep(con->comp))
		return;

	kmscon_console_draw(con->con);

	iter = kmscon_compositor_get_outputs(con->comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		if (!kmscon_output_is_active(iter))
			continue;

		ret = kmscon_output_use(iter);
		if (ret)
			continue;

		glClearColor(0.0, 0.0, 0.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);

		kmscon_console_map(con->con);

		ret = kmscon_output_swap(iter);
		if (ret)
			continue;
	}
}

static void draw(struct kmscon_idle *idle, void *data)
{
	struct console *con = data;

	kmscon_eloop_rm_idle(idle);
	map_outputs(con);
}

static void schedule_draw(struct console *con)
{
	int ret;

	ret = kmscon_eloop_add_idle(con->loop, con->idle, draw, con);
	if (ret && ret != -EALREADY)
		log_warning("Cannot schedule draw function\n");
}

static void activate_outputs(struct console *con)
{
	struct kmscon_output *iter;
	struct kmscon_mode *mode;
	int ret;
	uint32_t x, y;

	con->max_x = 0;
	con->max_y = 0;

	iter = kmscon_compositor_get_outputs(con->comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		if (!kmscon_output_is_active(iter)) {
			ret = kmscon_output_activate(iter, NULL);
			if (ret)
				continue;
		}

		mode = kmscon_output_get_current(iter);
		x = kmscon_mode_get_width(mode);
		y = kmscon_mode_get_height(mode);
		if (x > con->max_x)
			con->max_x = x;
		if (y > con->max_y)
			con->max_y = y;
	}

	if (con->max_x == 0)
		con->max_x = 800;
	if (con->max_y == 0)
		con->max_y = 600;

	kmscon_console_set_res(con->con, con->max_x, con->max_y);
	schedule_draw(con);
}

static void sig_term(struct kmscon_signal *sig, int signum, void *data)
{
	terminate = 1;
}

static bool vt_switch(struct kmscon_vt *vt, int action, void *data)
{
	struct console *con = data;
	int ret;

	if (action == KMSCON_VT_ENTER) {
		ret = kmscon_compositor_wake_up(con->comp);
		if (ret == 0) {
			log_info("No output found\n");
		} else if (ret > 0) {
			ret = kmscon_compositor_use(con->comp);
			if (!ret)
				activate_outputs(con);
		}
	} else {
		kmscon_compositor_sleep(con->comp);
	}

	return true;
}

static void destroy_eloop(struct console *con)
{
	kmscon_eloop_rm_idle(con->idle);
	kmscon_idle_unref(con->idle);
	kmscon_compositor_unref(con->comp);
	kmscon_console_unref(con->con);
	kmscon_vt_unref(con->vt);
	kmscon_eloop_rm_signal(con->sig_int);
	kmscon_eloop_rm_signal(con->sig_term);
	kmscon_eloop_unref(con->loop);
}

static int setup_eloop(struct console *con)
{
	int ret;
	unsigned int i;
	struct kmscon_char *ch;

	ret = kmscon_eloop_new(&con->loop);
	if (ret)
		return ret;

	ret = kmscon_eloop_new_signal(con->loop, &con->sig_term, SIGTERM,
							sig_term, NULL);
	if (ret)
		goto err_loop;

	ret = kmscon_eloop_new_signal(con->loop, &con->sig_int, SIGINT,
							sig_term, NULL);
	if (ret)
		goto err_loop;

	ret = kmscon_vt_new(&con->vt, vt_switch, con);
	if (ret)
		goto err_loop;

	ret = kmscon_vt_open(con->vt, KMSCON_VT_NEW);
	if (ret)
		goto err_loop;

	ret = kmscon_vt_connect_eloop(con->vt, con->loop);
	if (ret)
		goto err_loop;

	ret = kmscon_console_new(&con->con);
	if (ret)
		goto err_loop;

	ret = kmscon_char_new_u8(&ch, "J", 1);
	if (ret)
		goto err_loop;

	for (i = 0; i < 80 * 24 - 1; ++i)
		kmscon_console_write(con->con, ch);

	kmscon_char_free(ch);

	ret = kmscon_compositor_new(&con->comp);
	if (ret)
		goto err_loop;

	ret = kmscon_idle_new(&con->idle);
	if (ret)
		goto err_loop;

	return 0;

err_loop:
	destroy_eloop(con);
	return ret;
}

int main(int argc, char **argv)
{
	struct console con;
	int ret;

	setlocale(LC_ALL, "");
	memset(&con, 0, sizeof(con));

	ret = setup_eloop(&con);
	if (ret) {
		log_err("Cannot setup eloop\n");
		return abs(ret);
	}

	schedule_draw(&con);

	while (!terminate) {
		ret = kmscon_eloop_dispatch(con.loop, -1);
		if (ret)
			break;
	}

	destroy_eloop(&con);
	return abs(ret);
}
