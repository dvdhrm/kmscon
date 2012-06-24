#
# Kmscon - Global Makefile
# Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
#

ACLOCAL_AMFLAGS = -I m4
SUBDIRS = \
	. \
	docs/reference
EXTRA_DIST = \
	README \
	TODO \
	COPYING
CLEANFILES =

#
# Build targets
#
# kmscon: Main kmscon program
# test_output: Test program for the display graphics subsystem
# test_vt: Test program for the VT subsystem
# test_input: Test program for the input subsystem
# libkmscon-core: Static core library for kmscon and test programs
# libkmscon-static: Static library for all subsystems
# genshader: Program used to convert shaders into C-source files
#

bin_PROGRAMS = \
	kmscon
check_PROGRAMS = \
	test_output \
	test_vt \
	test_input
noinst_PROGRAMS = \
	genshader
noinst_LTLIBRARIES = \
	libkmscon-core.la \
	libkmscon-static.la

#
# Default CFlags
# Make all files include "config.h" by default. This shouldn't cause any
# problems and we cannot forget to include it anymore.
#
# Also make the linker discard all unused symbols as we are not building a
# shared library.
#
# When compiling in debug mode, we disable optimization and enable debug symbols
# so debugging with gdb is easier.
#

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

#
# Shaders
# As there is no need to modify shaders at run-time, we statically compile them
# into object files. As autotools would ignore them, we need to add them to
# EXTRA_DIST.
# The program that converts the shaders into C-source files is "genshader". It's
# pretty simple and just creates a string with the shader source as content.
#

EXTRA_DIST += \
	src/output_shader_def.vert \
	src/output_shader_def.frag \
	src/output_shader_tex.vert \
	src/output_shader_tex.frag
CLEANFILES += \
	src/output_shaders.c

nodist_genshader_SOURCES = \
	src/genshader.c

src/output_shaders.c: \
		src/output_shader_def.vert \
		src/output_shader_def.frag \
		src/output_shader_tex.vert \
		src/output_shader_tex.frag \
		genshader$(EXEEXT)
	./genshader$(EXEEXT)

#
# libkmscon-core
# This static library contains all the source files used in kmscon. We build
# them as separate library to allow linking them to the test programs.
# Only "main.c" is not included here as it contains the main() function.
#

nodist_libkmscon_core_la_SOURCES = \
	src/output_shaders.c

libkmscon_core_la_SOURCES = \
	src/conf.c src/conf.h \
	src/ui.c src/ui.h \
	src/console.c src/console.h \
	src/unicode.c src/unicode.h \
	src/log.c src/log.h \
	src/eloop.c src/eloop.h \
	src/vt.c src/vt.h \
	src/vte.c src/vte.h \
	src/vte_charsets.c \
	src/terminal.c src/terminal.h \
	src/pty.c src/pty.h \
	src/uterm.h src/uterm_internal.h \
	src/uterm_video.c \
	src/uterm_video_drm.c \
	src/uterm_video_fbdev.c \
	src/uterm_monitor.c \
	src/uterm_input.c \
	src/uterm_vt.c \
	src/gl.h \
	src/gl_math.c \
	src/gl_shader.c \
	src/font_pango.c

if USE_XKBCOMMON
libkmscon_core_la_SOURCES += \
	external/imKStoUCS.h \
	external/imKStoUCS.c \
	src/uterm_input_xkb.c
else
libkmscon_core_la_SOURCES += \
	external/imKStoUCS.h \
	external/imKStoUCS.c \
	src/uterm_input_dumb.c
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
	$(SYSTEMD_CFLAGS) \
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
	$(SYSTEMD_LIBS) \
	$(DRM_LIBS) \
	$(EGL_LIBS) \
	$(GBM_LIBS) \
	$(OPENGL_LIBS) \
	$(PANGO_LIBS) \
	$(UDEV_LIBS) \
	$(FREETYPE2_LIBS) \
	$(XPROTO_LIBS) \
	$(XKBCOMMON_LIBS) \
	$(GLIB_LIBS) \
	-lpthread

#
# libkmscon-static
# This static library contains all small helpers that are used in several other
# libraries and programs that are part of kmscon. To avoid putting these small
# pieces into a library and thus having to keep backwards compatibility, we
# simply link them statically into all other libraries/programs.
#

libkmscon_static_la_SOURCES = \
	src/static_llog.h \
	src/static_misc.h \
	src/static_misc.c

libkmscon_static_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(GLIB_CFLAGS)
libkmscon_static_la_LIBADD = \
	$(GLIB_LIBS)

#
# Binaries
# These are the sources for the main binaries and test programs. They mostly
# consists of a single source file only and include all the libraries that are
# built as part of kmscon.
#

kmscon_SOURCES = src/main.c
kmscon_LDADD = libkmscon-core.la libkmscon-static.la

test_output_SOURCES = tests/test_output.c tests/test_include.h
test_output_LDADD = libkmscon-core.la

test_vt_SOURCES = tests/test_vt.c
test_vt_LDADD = libkmscon-core.la

test_input_SOURCES = tests/test_input.c
test_input_LDADD = libkmscon-core.la
