#
# Kmscon - Global Makefile
# Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
#

ACLOCAL_AMFLAGS = -I m4
SUBDIRS = \
	.
include_HEADERS =
EXTRA_DIST = \
	README \
	COPYING \
	NEWS
CLEANFILES =
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA =

#
# Build targets
#
# kmscon: Main kmscon program
# test_output: Test program for the display graphics subsystem
# test_vt: Test program for the VT subsystem
# test_input: Test program for the input subsystem
# libkmscon-core: Static core library for kmscon and test programs
# genshader: Program used to convert shaders into C-source files
#

bin_PROGRAMS =
check_PROGRAMS =
noinst_PROGRAMS = \
	genshader \
	genunifont
noinst_LTLIBRARIES =
lib_LTLIBRARIES =

#
# Default CFlags
# Make all files include "config.h" by default. This shouldn't cause any
# problems and we cannot forget to include it anymore.
#
# Also make the linker discard all unused symbols as we are not building a
# shared library.
#
# When compiling in debug mode, we enable debug symbols so debugging with gdb
# is easier. If optimizations are disabled, we pass -O0 to the compiler.
# Otherwise, we use standard optimizations -O2.
#

AM_CFLAGS = \
	-Wall
AM_CPPFLAGS = \
	-include $(top_builddir)/config.h \
	-I $(srcdir)/src \
	-I $(srcdir)/external
AM_LDFLAGS = \
	-Wl,--as-needed

if BUILD_ENABLE_DEBUG
AM_CFLAGS += -g
endif

if BUILD_ENABLE_OPTIMIZATIONS
AM_CFLAGS += -O2
else
AM_CFLAGS += -O0
endif

#
# SHL - Static Helper Library
# The SHL subsystem contains several small code pieces used all over kmscon and
# other applications.
#

SHL_DLIST = \
	src/shl_dlist.h
SHL_ARRAY = \
	src/shl_array.h
SHL_HASHTABLE = \
	src/shl_hashtable.h \
	external/htable.h \
	external/htable.c
SHL_RING = \
	src/shl_ring.h
SHL_TIMER = \
	src/shl_timer.h
SHL_LLOG = \
	src/shl_llog.h
SHL_HOOK = \
	src/shl_hook.h \
	$(SHL_DLIST)
SHL_MISC = \
	src/shl_misc.h

#
# libeloop
# This library contains the whole event-loop implementation of kmscon. It is
# compiled into a separate object to allow using it in several other programs.
#

if BUILD_ENABLE_ELOOP
lib_LTLIBRARIES += libeloop.la
include_HEADERS += src/eloop.h
pkgconfig_DATA += libeloop.pc
endif

libeloop_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_LLOG) \
	$(SHL_HOOK) \
	src/eloop.h \
	src/eloop.c

if BUILD_ENABLE_DBUS
libeloop_la_SOURCES += \
	external/dbus-common.h \
	external/dbus-loop.h \
	external/dbus-loop.c
endif

libeloop_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(DBUS_CFLAGS)
libeloop_la_LIBADD = \
	$(DBUS_LIBS)
libeloop_la_LDFLAGS = \
	-version-info 1:0:0

#
# libtsm
# The Terminal-emulator State Machine is a library that implements the whole VTE
# layer and everything related to it. It has no external dependencies so it can
# be used to implement any kind of terminal emulator or debugger.
#

if BUILD_ENABLE_TSM
lib_LTLIBRARIES += libtsm.la
include_HEADERS += \
	src/tsm_screen.h \
	src/tsm_unicode.h \
	src/tsm_vte.h
pkgconfig_DATA += libtsm.pc
endif

libtsm_la_SOURCES = \
	$(SHL_LLOG) \
	$(SHL_TIMER) \
	$(SHL_ARRAY) \
	$(SHL_HASHTABLE) \
	src/tsm_screen.h \
	src/tsm_screen.c \
	src/tsm_unicode.h \
	src/tsm_unicode.c \
	src/tsm_vte.h \
	src/tsm_vte.c \
	src/tsm_vte_charsets.c

libtsm_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(XKBCOMMON_CFLAGS)
libtsm_la_LDFLAGS = \
	$(XKBCOMMON_LIBS) \
	-version-info 1:0:0

#
# libuterm
# The uterm library provides helpers to create terminals in user-space. They
# are not limited to text-based terminals but rather provide graphics contexts
# so arbitrary output can be displayed. Additionally, they provide VT
# abstractions and an input layer
#

if BUILD_ENABLE_UTERM
lib_LTLIBRARIES += libuterm.la
include_HEADERS += src/uterm.h
pkgconfig_DATA += libuterm.pc
endif

libuterm_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_HOOK) \
	$(SHL_MISC) \
	src/uterm.h \
	src/uterm_input.h \
	src/uterm_video.h \
	src/uterm_video.c \
	src/uterm_monitor.c \
	src/uterm_vt.c \
	src/uterm_input.c \
	src/uterm_input_uxkb.c
nodist_libuterm_la_SOURCES =

libuterm_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(SYSTEMD_CFLAGS) \
	$(DRM_CFLAGS) \
	$(EGL_CFLAGS) \
	$(GBM_CFLAGS) \
	$(GLES2_CFLAGS) \
	$(UDEV_CFLAGS) \
	$(XKBCOMMON_CFLAGS)
libuterm_la_LIBADD = \
	$(SYSTEMD_LIBS) \
	$(DRM_LIBS) \
	$(EGL_LIBS) \
	$(GBM_LIBS) \
	$(GLES2_LIBS) \
	$(UDEV_LIBS) \
	$(XKBCOMMON_LIBS) \
	libeloop.la
libuterm_la_LDFLAGS = \
	-version-info 1:0:0

if BUILD_ENABLE_FBDEV
libuterm_la_SOURCES += \
	src/uterm_video_fbdev.c
endif

if BUILD_ENABLE_DRM
libuterm_la_SOURCES += \
	src/uterm_video_drm.c
libuterm_la_SOURCES += \
	src/static_gl.h \
	src/static_gl_math.c \
	src/static_gl_shader.c
nodist_libuterm_la_SOURCES += \
	src/static_shaders.c
endif

if BUILD_ENABLE_DUMB
libuterm_la_SOURCES += \
	src/uterm_video_dumb.c
endif

#
# Shaders
# As there is no need to modify shaders at run-time, we statically compile them
# into object files. As autotools would ignore them, we need to add them to
# EXTRA_DIST.
# The program that converts the shaders into C-source files is "genshader". It's
# pretty simple and just creates a string with the shader source as content.
#

SHADERS = \
	$(srcdir)/src/static_fill.vert \
	$(srcdir)/src/static_fill.frag \
	$(srcdir)/src/static_blend.vert \
	$(srcdir)/src/static_blend.frag \
	$(srcdir)/src/static_blit.vert \
	$(srcdir)/src/static_blit.frag \
	$(srcdir)/src/static_gltex.vert \
	$(srcdir)/src/static_gltex.frag

EXTRA_DIST += \
	$(SHADERS)
CLEANFILES += \
	src/static_shaders.c

genshader_SOURCES = \
	src/genshader.c

src/static_shaders.c: $(SHADERS) genshader$(EXEEXT)
	$(AM_V_GEN)./genshader$(EXEEXT) src/static_shaders.c $(SHADERS)

#
# Unifont Generator
# This generates the unifont sources from raw hex-encoded font data.
#

UNIFONT = \
	src/text_font_unifont_data.hex

EXTRA_DIST += \
	$(UNIFONT)
CLEANFILES += \
	src/text_font_unifont_data.c

genunifont_SOURCES = \
	src/genunifont.c

src/text_font_unifont_data.c: $(UNIFONT) genunifont$(EXEEXT)
	$(AM_V_GEN)./genunifont$(EXEEXT) src/text_font_unifont_data.c $(UNIFONT)

#
# Text-font library
# The text-font library is used by kmscon _and_ wlterm but is currently linked
# statically as it hasn't been cleaned up entirely.
# It has a build-time dependency to UTERM and runtime dependencies to TSM.
#

if BUILD_ENABLE_KMSCON
noinst_LTLIBRARIES += libtext-font.la
else
if BUILD_ENABLE_WLTERM
noinst_LTLIBRARIES += libtext-font.la
endif
endif

libtext_font_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_HASHTABLE) \
	$(SHL_HOOK) \
	src/text.h \
	src/text_font.c
nodist_libtext_font_la_SOURCES =

if BUILD_ENABLE_UNIFONT
libtext_font_la_SOURCES += \
	src/text_font_unifont.c
nodist_libtext_font_la_SOURCES += \
	src/text_font_unifont_data.c
endif

if BUILD_ENABLE_8X16
libtext_font_la_SOURCES += \
	src/text_font_8x16.c
endif

if BUILD_ENABLE_FREETYPE2
libtext_font_la_SOURCES += \
	src/text_font_freetype2.c
endif

if BUILD_ENABLE_PANGO
libtext_font_la_SOURCES += \
	src/text_font_pango.c
endif

libtext_font_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(PANGO_CFLAGS) \
	$(FREETYPE2_CFLAGS)
libtext_font_la_LIBADD = \
	$(PANGO_LIBS) \
	$(FREETYPE2_LIBS) \
	-lpthread \
	libtsm.la

#
# libkmscon-core
# This static library contains all the source files used in kmscon. We build
# them as separate library to allow linking them to the test programs.
# Only "main.c" is not included here as it contains the main() function.
#

if BUILD_ENABLE_KMSCON
noinst_LTLIBRARIES += libkmscon-core.la
endif

libkmscon_core_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_ARRAY) \
	$(SHL_HASHTABLE) \
	$(SHL_RING) \
	$(SHL_TIMER) \
	$(SHL_HOOK) \
	$(SHL_MISC) \
	src/conf.c src/conf.h \
	src/log.c src/log.h \
	src/pty.c src/pty.h \
	src/text.h \
	src/text.c
nodist_libkmscon_core_la_SOURCES =

if BUILD_ENABLE_BBLIT
libkmscon_core_la_SOURCES += \
	src/text_bblit.c
endif

if BUILD_ENABLE_BBULK
libkmscon_core_la_SOURCES += \
	src/text_bbulk.c
endif

if BUILD_ENABLE_GLES2
libkmscon_core_la_SOURCES += \
	src/text_gltex.c \
	src/static_gl.h \
	src/static_gl_math.c \
	src/static_gl_shader.c
nodist_libkmscon_core_la_SOURCES += \
	src/static_shaders.c
endif

libkmscon_core_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(GLES2_CFLAGS) \
	$(XKBCOMMON_CFLAGS)
libkmscon_core_la_LIBADD = \
	$(GLES2_LIBS) \
	$(XKBCOMMON_LIBS) \
	-lpthread \
	libeloop.la \
	libtsm.la \
	libuterm.la \
	libtext-font.la

#
# Binaries
# These are the sources for the main binaries and test programs. They mostly
# consists of a single source file only and include all the libraries that are
# built as part of kmscon.
#

if BUILD_ENABLE_KMSCON
bin_PROGRAMS += kmscon
check_PROGRAMS += \
	test_output \
	test_vt \
	test_input \
	test_key
endif

kmscon_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_MISC) \
	src/kmscon_terminal.h \
	src/kmscon_terminal.c \
	src/kmscon_dummy.h \
	src/kmscon_dummy.c \
	src/kmscon_seat.h \
	src/kmscon_seat.c \
	src/kmscon_conf.h \
	src/kmscon_conf.c \
	src/kmscon_main.c
kmscon_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(WAYLAND_CFLAGS) \
	$(XKBCOMMON_CFLAGS)
kmscon_LDADD = \
	$(WAYLAND_LIBS) \
	$(XKBCOMMON_LIBS) \
	libuterm.la \
	libeloop.la \
	libtext-font.la \
	libkmscon-core.la

if BUILD_ENABLE_WAYLAND
kmscon_SOURCES += \
	src/kmscon_compositor.h \
	src/kmscon_compositor.c
endif

test_output_SOURCES = tests/test_output.c tests/test_include.h
test_output_LDADD = libkmscon-core.la

test_vt_SOURCES = tests/test_vt.c
test_vt_LDADD = libkmscon-core.la

test_input_SOURCES = tests/test_input.c
test_input_LDADD = libkmscon-core.la

test_key_SOURCES = tests/test_key.c

#
# Wayland Terminal
#

if BUILD_ENABLE_WLTERM
bin_PROGRAMS += wlterm
endif

wlterm_SOURCES = \
	$(SHL_MISC) \
	$(SHL_ARRAY) \
	$(SHL_DLIST) \
	$(SHL_HOOK) \
	src/wlt_main.h \
	src/wlt_main.c \
	src/wlt_toolkit.h \
	src/wlt_toolkit.c \
	src/wlt_theme.h \
	src/wlt_theme.c \
	src/wlt_terminal.h \
	src/wlt_terminal.c \
	src/log.h \
	src/log.c \
	src/conf.h \
	src/conf.c \
	src/pty.h \
	src/pty.c
wlterm_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(WAYLAND_CFLAGS) \
	$(XKBCOMMON_CFLAGS)
wlterm_LDADD = \
	libeloop.la \
	libtsm.la \
	libtext-font.la \
	-lpthread \
	$(WAYLAND_LIBS) \
	$(XKBCOMMON_LIBS)
