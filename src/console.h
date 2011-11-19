/*
 * kmscon - Console Management
 * Written 2011 by David Herrmann <dh.herrmann@googlemail.com>
 */

/*
 * Console Management
 * The console management uses OpenGL, cairo and pango to draw a console to a
 * framebuffer. It is independent of the other subsystems and can also be used
 * in other applications.
 *
 * This console does not emulate any terminal at all. This subsystem just
 * provides functions to draw a console to a framebuffer and modifying the state
 * of it.
 */

#include <stdlib.h>

struct kmscon_console;

/* console objects */

int kmscon_console_new(struct kmscon_console **out);
void kmscon_console_ref(struct kmscon_console *con);
void kmscon_console_unref(struct kmscon_console *con);
