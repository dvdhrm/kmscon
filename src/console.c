/*
 * kmscon - Console Management
 * Written 2011 by David Herrmann <dh.herrmann@googlemail.com>
 */

/*
 * Console Management
 * This provides the console drawing and manipulation functions. It does not
 * provide the terminal emulation. It is just an abstraction layer to draw text
 * to a framebuffer as used by terminals and consoles.
 */

/*
 * TODO: Avoid using this hack and instead retrieve GL extension
 * pointers dynamically on initialization.
 */
#define GL_GLEXT_PROTOTYPES

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include "console.h"

struct kmscon_console {
	size_t ref;
};

int kmscon_console_new(struct kmscon_console **out)
{
	struct kmscon_console *con;

	if (!out)
		return -EINVAL;

	con = malloc(sizeof(*con));
	if (!con)
		return -ENOMEM;

	memset(con, 0, sizeof(*con));
	con->ref = 1;

	*out = con;
	return 0;
}

void kmscon_console_ref(struct kmscon_console *con)
{
	if (!con)
		return;

	++con->ref;
}

void kmscon_console_unref(struct kmscon_console *con)
{
	if (!con || !con->ref)
		return;

	if (--con->ref)
		return;

	free(con);
}
