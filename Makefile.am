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
	NEWS \
	docs/kmscon.service \
	docs/kmsconvt@.service
CLEANFILES =
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA =
MANPAGES =
MANPAGES_ALIASES =
TPHONY =

bin_PROGRAMS =
check_PROGRAMS =
noinst_PROGRAMS = \
	genshader \
	genunifont
noinst_LTLIBRARIES =
lib_LTLIBRARIES =

moduledir = @libdir@/kmscon
module_LTLIBRARIES =

#
# Default CFlags
# Make all files include "config.h" by default. This shouldn't cause any
# problems and we cannot forget to include it anymore.
#
# Also make the linker discard all unused symbols.
#
# When compiling in debug mode, we enable debug symbols so debugging with gdb
# is easier. If optimizations are disabled, we pass -O0 to the compiler.
# Otherwise, we use standard optimizations -O2.
#

AM_CFLAGS = \
	-Wall
AM_CPPFLAGS = \
	-DBUILD_MODULE_DIR='"$(moduledir)"' \
	-include $(top_builddir)/config.h \
	-I $(srcdir)/src
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
# Simply include $(SHL_*) in your source-file list.
# SHL_MISC needs xkbcommon
# SHL_REGISTER needs pthread
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
SHL_REGISTER = \
	src/shl_register.h \
	$(SHL_DLIST)

#
# GIT-HEAD helper
# The file ./src/githead.h contains a constant BUILD_GIT_HEAD which is defined
# to the string returned by "git describe". We need to adjust this string for
# every build and correctly rebuild any sources that depend on it. Therefore,
# you should use this file rarely as it causes rebuilds on every git-commit.
#
# We have a helper-script ./src/genversion.sh that takes as argument the header
# file and creates it if necessary. It updates it only if the new git-describe
# string is different to the old one. So the file is only modified on changes.
# Hence, we can use it as normal dependency in this Makefile.
# However, we need to run this script on _every_ "make" invocation before any
# recipy is executed. To achieve this, we use $(shell ...) and assign it to a
# "simply expanded" variable (:=) so the shell command is executed on
# variable-declaration and not during expansion.
#
# Note that we must not clean ./src/githead.h ever! If we would, a distribution
# tarball might delete that file and have no way to recreate it.
# We could delete it on something like "make maintainerclean", but then again,
# it seems unnecessary so lets simply not clean it at all.
#
# If the helper-script is executed in a directory that is not a git-repository
# (like a distribution tarball) and githead.h exists, then it does nothing as it
# expects githead.h to be correctly written by "make dist".
# However, if githead.h does not exist, it will print a warning and write
# "<unknown>" as git-revision.
# This guarantees, that githead.h is always present and has the most correct
# value that we can get under any conditions.
#
# The $(emptyvariable) expansion below is used for broken $(shell ...)
# syntax-highlighting algorithms in many existing editors.
#

EXTRA_DIST += src/genversion.sh
GITHEAD:=$(shell $(emptyvariable)"$(srcdir)/src/genversion.sh" "$(srcdir)/src/githead.h")

#
# libeloop
# This library contains the whole event-loop implementation of kmscon. It is
# compiled into a separate object to allow using it in several other programs.
#

if BUILD_ENABLE_ELOOP
lib_LTLIBRARIES += libeloop.la
include_HEADERS += src/eloop.h
pkgconfig_DATA += docs/pc/libeloop.pc
endif

libeloop_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_LLOG) \
	$(SHL_HOOK) \
	src/eloop.h \
	src/eloop.c

libeloop_la_CPPFLAGS = \
	$(AM_CPPFLAGS)
libeloop_la_LIBADD =
libeloop_la_LDFLAGS = \
	-version-info 1:0:0


if BUILD_ENABLE_ELOOP_DBUS
libeloop_la_SOURCES += \
	external/dbus-common.h \
	external/dbus-loop.h \
	external/dbus-loop.c
libeloop_la_CPPFLAGS += \
	$(DBUS_CFLAGS)
libeloop_la_LIBADD += \
	$(DBUS_LIBS)
endif

#
# libtsm
# The Terminal-emulator State Machine is a library that implements the whole VTE
# layer and everything related to it. It has no external dependencies so it can
# be used to implement any kind of terminal emulator or debugger.
#

if BUILD_ENABLE_TSM
lib_LTLIBRARIES += \
	libtsm.la
include_HEADERS += \
	src/tsm_screen.h \
	src/tsm_unicode.h \
	src/tsm_vte.h
pkgconfig_DATA += \
	docs/pc/libtsm.pc
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
	src/tsm_vte_charsets.c \
	external/wcwidth.h \
	external/wcwidth.c

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
include_HEADERS += \
	src/uterm.h \
	src/uterm_video.h
pkgconfig_DATA += docs/pc/libuterm.pc
endif

libuterm_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_HOOK) \
	$(SHL_MISC) \
	src/uterm.h \
	src/uterm_video.h \
	src/uterm_input_internal.h \
	src/uterm_video_internal.h \
	src/uterm_systemd_internal.h \
	src/uterm_video.c \
	src/uterm_monitor.c \
	src/uterm_vt.c \
	src/uterm_input.c \
	src/uterm_input_uxkb.c

nodist_libuterm_la_SOURCES =

libuterm_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(XKBCOMMON_CFLAGS)
libuterm_la_LIBADD = \
	$(XKBCOMMON_LIBS) \
	libeloop.la
libuterm_la_LDFLAGS = \
	-version-info 1:0:0

if BUILD_ENABLE_MULTI_SEAT
libuterm_la_SOURCES += src/uterm_systemd.c
libuterm_la_CPPFLAGS += $(SYSTEMD_CFLAGS)
libuterm_la_LIBADD += $(SYSTEMD_LIBS)
endif

if BUILD_ENABLE_HOTPLUG
libuterm_la_CPPFLAGS += $(UDEV_CFLAGS)
libuterm_la_LIBADD += $(UDEV_LIBS)
endif

if BUILD_ENABLE_VIDEO_FBDEV
libuterm_la_SOURCES += src/uterm_video_fbdev.c
endif

if BUILD_ENABLE_VIDEO_DUMB
libuterm_la_SOURCES += src/uterm_video_dumb.c
libuterm_la_CPPFLAGS += $(DRM_CFLAGS)
libuterm_la_LIBADD += $(DRM_LIBS)
endif

if BUILD_ENABLE_VIDEO_DRM
libuterm_la_SOURCES += \
	src/uterm_video_drm.c \
	src/static_gl.h \
	src/static_gl_math.c \
	src/static_gl_shader.c
nodist_libuterm_la_SOURCES += \
	src/static_shaders.c
libuterm_la_CPPFLAGS += \
	$(DRM_CFLAGS) \
	$(EGL_CFLAGS) \
	$(GBM_CFLAGS) \
	$(GLES2_CFLAGS)
libuterm_la_LIBADD += \
	$(DRM_LIBS) \
	$(EGL_LIBS) \
	$(GBM_LIBS) \
	$(GLES2_LIBS)
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

EXTRA_DIST += $(SHADERS)
CLEANFILES += src/static_shaders.c
genshader_SOURCES = src/genshader.c

src/static_shaders.c: $(SHADERS) genshader$(EXEEXT)
	$(AM_V_GEN)./genshader$(EXEEXT) src/static_shaders.c $(SHADERS)

#
# Unifont Generator
# This generates the unifont sources from raw hex-encoded font data.
#

UNIFONT = src/font_unifont_data.hex

EXTRA_DIST += $(UNIFONT)
CLEANFILES += src/font_unifont_data.c
genunifont_SOURCES = src/genunifont.c

src/font_unifont_data.c: $(UNIFONT) genunifont$(EXEEXT)
	$(AM_V_GEN)./genunifont$(EXEEXT) src/font_unifont_data.c $(UNIFONT)

#
# Kmscon Modules
#

if BUILD_ENABLE_FONT_UNIFONT
module_LTLIBRARIES += mod-unifont.la
endif

mod_unifont_la_SOURCES = \
	src/kmscon_module_interface.h \
	src/githead.h \
	src/font_unifont.c \
	src/kmscon_mod_unifont.c
nodist_mod_unifont_la_SOURCES = \
	src/font_unifont_data.c
mod_unifont_la_LDFLAGS = \
	-module \
	-avoid-version

if BUILD_ENABLE_FONT_FREETYPE2
module_LTLIBRARIES += mod-freetype2.la
endif

mod_freetype2_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_HASHTABLE) \
	src/kmscon_module_interface.h \
	src/githead.h \
	src/font_freetype2.c \
	src/kmscon_mod_freetype2.c
mod_freetype2_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(FREETYPE2_CFLAGS)
mod_freetype2_la_LIBADD = \
	$(FREETYPE2_LIBS) \
	-lpthread \
	libtsm.la
mod_freetype2_la_LDFLAGS = \
	-module \
	-avoid-version

if BUILD_ENABLE_FONT_PANGO
module_LTLIBRARIES += mod-pango.la
endif

mod_pango_la_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_HASHTABLE) \
	src/kmscon_module_interface.h \
	src/githead.h \
	src/font_pango.c \
	src/kmscon_mod_pango.c
mod_pango_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(PANGO_CFLAGS)
mod_pango_la_LIBADD = \
	$(PANGO_LIBS) \
	-lpthread \
	libtsm.la
mod_pango_la_LDFLAGS = \
	-module \
	-avoid-version

if BUILD_ENABLE_RENDERER_BBULK
module_LTLIBRARIES += mod-bbulk.la
endif

mod_bbulk_la_SOURCES = \
	src/kmscon_module_interface.h \
	src/githead.h \
	src/text_bbulk.c \
	src/kmscon_mod_bbulk.c
mod_bbulk_la_LDFLAGS = \
	-module \
	-avoid-version

if BUILD_ENABLE_RENDERER_GLTEX
module_LTLIBRARIES += mod-gltex.la
endif

mod_gltex_la_SOURCES = \
	src/kmscon_module_interface.h \
	src/githead.h \
	src/text_gltex.c \
	src/static_gl.h \
	src/static_gl_math.c \
	src/static_gl_shader.c \
	src/kmscon_mod_gltex.c
nodist_mod_gltex_la_SOURCES = \
	src/static_shaders.c
mod_gltex_la_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(GLES2_CFLAGS)
mod_gltex_la_LIBADD = \
	$(GLES2_LIBS)
mod_gltex_la_LDFLAGS = \
	-module \
	-avoid-version

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
MANPAGES += docs/man/kmscon.1
endif

kmscon_SOURCES = \
	$(SHL_DLIST) \
	$(SHL_MISC) \
	$(SHL_ARRAY) \
	$(SHL_HASHTABLE) \
	$(SHL_RING) \
	$(SHL_TIMER) \
	$(SHL_HOOK) \
	$(SHL_REGISTER) \
	src/githead.h \
	src/conf.h \
	src/conf.c \
	src/log.h \
	src/log.c \
	src/pty.h \
	src/pty.c \
	src/font.h \
	src/font.c \
	src/font_8x16.c \
	src/text.h \
	src/text.c \
	src/text_bblit.c \
	src/kmscon_module_interface.h \
	src/kmscon_module.h \
	src/kmscon_module.c \
	src/kmscon_terminal.h \
	src/kmscon_dummy.h \
	src/kmscon_cdev.h \
	src/kmscon_seat.h \
	src/kmscon_seat.c \
	src/kmscon_conf.h \
	src/kmscon_conf.c \
	src/kmscon_main.c
nodist_kmscon_SOURCES =

kmscon_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(XKBCOMMON_CFLAGS)
kmscon_LDADD = \
	$(XKBCOMMON_LIBS) \
	libeloop.la \
	libuterm.la \
	-lpthread
kmscon_LDFLAGS = \
	-rdynamic

if BUILD_ENABLE_SESSION_DUMMY
kmscon_SOURCES += src/kmscon_dummy.c
endif

if BUILD_ENABLE_SESSION_TERMINAL
kmscon_SOURCES += src/kmscon_terminal.c
kmscon_LDADD += libtsm.la
endif

if BUILD_ENABLE_SESSION_CDEV
kmscon_SOURCES += src/kmscon_cdev.c
kmscon_CPPFLAGS += $(FUSE_CFLAGS)
kmscon_LDADD += $(FUSE_LIBS)
endif

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
	$(WAYLAND_LIBS) \
	$(XKBCOMMON_LIBS) \
	libeloop.la \
	libtsm.la \
	-lpthread

#
# Tests
#

test_sources = \
	src/log.h \
	src/log.c \
	src/conf.h \
	src/conf.c \
	tests/test_include.h
test_cflags = \
	$(XKBCOMMON_CFLAGS)
test_libs = \
	$(XKBCOMMON_LIBS) \
	libeloop.la

test_output_SOURCES = \
	$(test_sources) \
	tests/test_output.c
test_output_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(test_cflags)
test_output_LDADD = \
	$(test_libs) \
	libuterm.la

test_vt_SOURCES = \
	$(test_sources) \
	tests/test_vt.c
test_vt_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(test_cflags)
test_vt_LDADD = \
	$(test_libs) \
	libuterm.la

test_input_SOURCES = \
	$(test_sources) \
	tests/test_input.c
test_input_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(test_cflags)
test_input_LDADD = \
	$(test_libs) \
	libuterm.la

test_key_SOURCES = \
	$(test_sources) \
	tests/test_key.c
test_key_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(test_cflags)
test_key_LDADD = \
	$(test_libs)

#
# Manpages
#

man_MANS =
EXTRA_DIST += ${patsubst %.1,%.xml,${patsubst %.3,%.xml,${patsubst %.5,%.xml,${patsubst %.7,%.xml,$(MANPAGES)}}}}
CLEANFILES += $(MANPAGES) $(MANPAGES_ALIASES) .man_fixup

if BUILD_HAVE_XSLTPROC
if BUILD_HAVE_MANPAGES_STYLESHEET

man_MANS += $(MANPAGES) $(MANPAGES_ALIASES)

XSLTPROC_FLAGS = \
	--stringparam man.authors.section.enabled 0 \
	--stringparam man.copyright.section.enabled 0 \
	--stringparam funcsynopsis.style ansi \
	--stringparam man.output.quietly 1 \
	--nonet

XSLTPROC_PROCESS_MAN = \
	$(AM_V_GEN)$(MKDIR_P) $(dir $@) && \
	$(XSLTPROC) -o "$@" $(XSLTPROC_FLAGS) $(BUILD_MANPAGES_STYLESHEET) "$<" && \
	touch .man_fixup

# Force .man_fixup if $(MANPAGES) are not built
.man_fixup: | $(MANPAGES)
	@touch .man_fixup

$(MANPAGES_ALIASES): $(MANPAGES) .man_fixup
	$(AM_V_GEN)if test -n "$@" ; then $(SED) -i -e 's/^\.so \([a-z_]\+\)\.\([0-9]\)$$/\.so man\2\/\1\.\2/' "$@" ; fi

docs/man/%.1: docs/man/%.xml
	$(XSLTPROC_PROCESS_MAN)

docs/man/%.3: docs/man/%.xml
	$(XSLTPROC_PROCESS_MAN)

docs/man/%.5: docs/man/%.xml
	$(XSLTPROC_PROCESS_MAN)

docs/man/%.7: docs/man/%.xml
	$(XSLTPROC_PROCESS_MAN)

endif # BUILD_HAVE_MANPAGES_STYLESHEET
endif # BUILD_HAVE_XSLTPROC

#
# Phony targets
#

.PHONY: $(TPHONY)
