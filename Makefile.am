ACLOCAL_AMFLAGS = -I m4

bin_PROGRAMS = kmscon
check_PROGRAMS = test_console test_output
noinst_LTLIBRARIES = libkmscon-core.la

AM_CFLAGS = \
	-Wall \
	-I $(srcdir)/src
AM_LDFLAGS = \
	-Wl,--as-needed

if DEBUG
AM_CFLAGS += -g
endif

libkmscon_core_la_SOURCES = \
	src/console.c src/console.h \
	src/output.c src/output.h \
	src/console_char.c

libkmscon_core_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(EGL_CFLAGS) \
	$(GBM_CFLAGS) \
	$(OPENGL_CFLAGS) \
	$(CAIRO_CFLAGS) \
	$(PANGO_CFLAGS)
libkmscon_core_la_LIBADD = \
	$(EGL_LIBS) \
	$(GBM_LIBS) \
	$(OPENGL_LIBS) \
	$(CAIRO_LIBS) \
	$(PANGO_LIBS)

kmscon_SOURCES = src/main.c
kmscon_LDADD = libkmscon-core.la
kmscon_CFLAGS = \
	$(AM_CFLAGS) \
	$(CAIRO_CFLAGS)

test_console_SOURCES = tests/test_console.c
test_console_LDADD = libkmscon-core.la
test_console_CFLAGS = $(kmscon_CFLAGS)

test_output_SOURCES = tests/test_output.c
test_output_LDADD = libkmscon-core.la
test_output_CPPFLAGS = $(kmscon_CFLAGS)

dist_doc_DATA = README TODO
