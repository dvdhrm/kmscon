ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS = kmscon
check_PROGRAMS = test_console test_output test_vt test_buffer
noinst_LTLIBRARIES = libkmscon-core.la

AM_CFLAGS = \
	-Wall
AM_CPPFLAGS = \
	-I $(srcdir)/src
AM_LDFLAGS = \
	-Wl,--as-needed

if DEBUG
AM_CFLAGS += -O0 -g
else
AM_CFLAGS += -O2
endif

libkmscon_core_la_SOURCES = \
	src/console.c src/console.h \
	src/output.c src/output.h \
	src/console_char.c \
	src/console_cell.c \
	src/log.c src/log.h \
	src/eloop.c src/eloop.h \
	src/vt.c src/vt.h

libkmscon_core_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(DRM_CFLAGS) \
	$(EGL_CFLAGS) \
	$(GBM_CFLAGS) \
	$(OPENGL_CFLAGS) \
	$(CAIRO_CFLAGS) \
	$(PANGO_CFLAGS)
libkmscon_core_la_LIBADD = \
	$(DRM_LIBS) \
	$(EGL_LIBS) \
	$(GBM_LIBS) \
	$(OPENGL_LIBS) \
	$(CAIRO_LIBS) \
	$(PANGO_LIBS)

kmscon_SOURCES = src/main.c
kmscon_LDADD = libkmscon-core.la

test_console_SOURCES = tests/test_console.c
test_console_LDADD = \
	libkmscon-core.la \
	$(OPENGL_LIBS)
test_console_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(OPENGL_CFLAGS) \
	$(CAIRO_CFLAGS)

test_output_SOURCES = tests/test_output.c
test_output_LDADD = \
	libkmscon-core.la \
	$(OPENGL_LIBS)
test_output_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(OPENGL_CFLAGS)

test_vt_SOURCES = tests/test_vt.c
test_vt_LDADD = libkmscon-core.la

test_buffer_SOURCES = tests/test_buffer.c
test_buffer_LDADD = libkmscon-core.la

dist_doc_DATA = README TODO
