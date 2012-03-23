ACLOCAL_AMFLAGS = -I m4
EXTRA_DIST = README TODO COPYING
CLEANFILES =

bin_PROGRAMS = kmscon
check_PROGRAMS = test_console test_output test_vt test_buffer test_terminal \
	test_input
noinst_PROGRAMS = genshader
noinst_LTLIBRARIES = libkmscon-core.la

AM_CFLAGS = \
	-Wall
AM_CPPFLAGS = \
	-include $(top_builddir)/config.h \
	-I $(srcdir)/src \
	-I $(srcdir)/external
AM_LDFLAGS = \
	-Wl,--as-needed

if DEBUG
AM_CFLAGS += -O0 -g
else
AM_CFLAGS += -O2
endif

EXTRA_DIST += src/output_shader_def.vert src/output_shader_def.frag \
	src/output_shader_tex.vert src/output_shader_tex.frag
CLEANFILES += src/output_shaders.c

nodist_genshader_SOURCES = \
	src/genshader.c

src/output_shaders.c: src/output_shader_def.vert src/output_shader_def.frag \
	src/output_shader_tex.vert src/output_shader_tex.frag genshader$(EXEEXT)
	./genshader$(EXEEXT)

nodist_libkmscon_core_la_SOURCES = \
	src/output_shaders.c

libkmscon_core_la_SOURCES = \
	src/misc.c src/misc.h \
	src/console.c src/console.h \
	src/unicode.c src/unicode.h \
	src/log.c src/log.h \
	src/eloop.c src/eloop.h \
	src/vt.c src/vt.h \
	src/input.c src/input.h \
	src/vte.c src/vte.h \
	src/terminal.c src/terminal.h \
	src/pty.c src/pty.h \
	src/uterm.h src/uterm_internal.h \
	src/uterm_video.c \
	src/uterm_video_drm.c \
	src/gl.h \
	src/gl_math.c \
	src/gl_shader.c

if USE_XKBCOMMON
libkmscon_core_la_SOURCES += \
	src/kbd_xkb.c src/kbd.h \
	external/imKStoUCS.c external/imKStoUCS.h
else
libkmscon_core_la_SOURCES += \
	src/kbd_dumb.c src/kbd.h \
	external/imKStoUCS.c external/imKStoUCS.h
endif

if USE_PANGO
libkmscon_core_la_SOURCES += \
	src/font_pango.c src/font.h
else
libkmscon_core_la_SOURCES += \
	src/font_freetype.c src/font.h
endif

libkmscon_core_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(DRM_CFLAGS) \
	$(EGL_CFLAGS) \
	$(GBM_CFLAGS) \
	$(OPENGL_CFLAGS) \
	$(PANGO_CFLAGS) \
	$(UDEV_CFLAGS) \
	$(FREETYPE2_CFLAGS) \
	$(XPROTO_CFLAGS) \
	$(XKBCOMMON_CFLAGS) \
	$(GLIB_CFLAGS)
libkmscon_core_la_LIBADD = \
	$(DRM_LIBS) \
	$(EGL_LIBS) \
	$(GBM_LIBS) \
	$(OPENGL_LIBS) \
	$(PANGO_LIBS) \
	$(UDEV_LIBS) \
	$(FREETYPE2_LIBS) \
	$(XPROTO_LIBS) \
	$(XKBCOMMON_LIBS) \
	$(GLIB_LIBS)

kmscon_SOURCES = src/main.c
kmscon_LDADD = libkmscon-core.la

test_console_SOURCES = tests/test_console.c
test_console_LDADD = libkmscon-core.la

test_output_SOURCES = tests/test_output.c
test_output_LDADD = libkmscon-core.la

test_vt_SOURCES = tests/test_vt.c
test_vt_LDADD = libkmscon-core.la

test_buffer_SOURCES = tests/test_buffer.c
test_buffer_LDADD = libkmscon-core.la

test_terminal_SOURCES = tests/test_terminal.c
test_terminal_LDADD = libkmscon-core.la

test_input_SOURCES = tests/test_input.c
test_input_LDADD = libkmscon-core.la
