/*
 * test_console - Test Console
 * Written 2011 by David Herrmann <dh.herrmann@googlemail.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include "console.h"
#include "output.h"

static int map_outputs(struct kmscon_compositor *comp,
						struct kmscon_console *con)
{
	int ret;
	struct kmscon_output *iter;

	iter = kmscon_compositor_get_outputs(comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		if (!kmscon_output_is_active(iter))
			continue;

		ret = kmscon_output_use(iter);
		if (ret) {
			printf("Cannot use output %p: %d\n", iter, ret);
			continue;
		}

		glClearColor(0.0, 0.0, 0.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);

		kmscon_console_map(con);

		ret = kmscon_output_swap(iter);
		if (ret) {
			printf("Cannot swap buffers of output %p: %d\n",
								iter, ret);
			continue;
		}
	}

	return 0;
}

static int run_console(struct kmscon_compositor *comp)
{
	struct kmscon_output *iter;
	struct kmscon_mode *mode;
	struct kmscon_console *con;
	int ret;
	uint32_t max_x, max_y, x, y;

	max_x = 0;
	max_y = 0;

	iter = kmscon_compositor_get_outputs(comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		printf("Activating output %p...\n", iter);
		ret = kmscon_output_activate(iter, NULL);
		if (ret) {
			printf("Cannot activate output: %d\n", ret);
			continue;
		}

		mode = kmscon_output_get_current(iter);
		x = kmscon_mode_get_width(mode);
		y = kmscon_mode_get_height(mode);
		if (x > max_x)
			max_x = x;
		if (y > max_y)
			max_y = y;
	}

	if (max_x == 0 || max_y == 0) {
		printf("Cannot retrieve output resolution\n");
		return -EINVAL;
	}

	ret = kmscon_console_new(&con);
	if (ret) {
		printf("Cannot create console: %d\n", ret);
		return ret;
	}

	ret = kmscon_console_set_res(con, max_x, max_y);
	if (ret) {
		printf("Cannot set console resolution: %d\n", ret);
		goto err_unref;
	}

	while (1) {
		kmscon_console_draw(con);
		map_outputs(comp, con);
		usleep(10000);
	}

err_unref:
	kmscon_console_unref(con);
	return ret;
}

int main(int argc, char **argv)
{
	struct kmscon_compositor *comp;
	int ret;

	ret = kmscon_compositor_new(&comp);
	if (ret) {
		printf("Cannot create compositor: %d\n", ret);
		return abs(ret);
	}

	ret = kmscon_compositor_wake_up(comp);
	if (ret < 0) {
		printf("Cannot wake up compositor: %d\n", ret);
		goto err_unref;
	}

	if (ret == 0) {
		printf("No output available\n");
		ret = -EINVAL;
		goto err_unref;
	}

	kmscon_compositor_use(comp);
	ret = run_console(comp);

err_unref:
	kmscon_compositor_unref(comp);
	return abs(ret);
}
