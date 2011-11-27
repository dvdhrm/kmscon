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

#include <cairo.h>
#include <inttypes.h>
#include <stdlib.h>

struct kmscon_char;
struct kmscon_font;
struct kmscon_console;

/* single printable characters */

int kmscon_char_new(struct kmscon_char **out);
int kmscon_char_dup(struct kmscon_char **out, const struct kmscon_char *orig);
void kmscon_char_free(struct kmscon_char *ch);

int kmscon_char_set_u8(struct kmscon_char *ch, const char *str, size_t len);
const char *kmscon_char_get_u8(const struct kmscon_char *ch);
size_t kmscon_char_get_len(const struct kmscon_char *ch);
int kmscon_char_append_u8(struct kmscon_char *ch, const char *str, size_t len);

/* font objects with cached glyphs */

int kmscon_font_new(struct kmscon_font **out, uint32_t height);
void kmscon_font_ref(struct kmscon_font *font);
void kmscon_font_unref(struct kmscon_font *font);

int kmscon_font_draw(struct kmscon_font *font, const struct kmscon_char *ch,
					cairo_t *cr, uint32_t x, uint32_t y);

/* console objects */

int kmscon_console_new(struct kmscon_console **out);
void kmscon_console_ref(struct kmscon_console *con);
void kmscon_console_unref(struct kmscon_console *con);

int kmscon_console_set_res(struct kmscon_console *con, uint32_t x, uint32_t y);
void kmscon_console_draw(struct kmscon_console *con);
void kmscon_console_map(struct kmscon_console *con);

int kmscon_console_resize(struct kmscon_console *con, uint32_t x, uint32_t y);

void kmscon_console_cursor_get(struct kmscon_console *con, uint32_t *x,
								uint32_t *y);
void kmscon_console_cursor_move(struct kmscon_console *con, int32_t x,
								int32_t y);
void kmscon_console_cursor_goto(struct kmscon_console *con, uint32_t x,
								uint32_t y);
