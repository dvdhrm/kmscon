#
# Kmscon - build configuration script
# Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
#

AC_PREREQ(2.68)

AC_INIT([kmscon], [2])
AC_SUBST(PACKAGE_URL, [https://github.com/dvdhrm/kmscon])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADER(config.h)

AM_INIT_AUTOMAKE([foreign 1.11 subdir-objects dist-bzip2 no-dist-gzip tar-pax -Wall -Werror])
AM_SILENT_RULES([yes])

#
# Don't add a default "-g -O2" if CFLAGS wasn't specified. For debugging it is
# often more convenient to have "-g -O0". You can still override it by
# explicitely setting it on the command line.
#

: ${CFLAGS=""}

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O
AM_PROG_AR

LT_PREREQ(2.2)
LT_INIT

#
# check for gtk-doc
# Use weird syntax to make "gtkdocize" happy.
#

m4_ifdef([GTK_DOC_CHECK],[
GTK_DOC_CHECK([1.14],[--flavour no-tmpl])
], [AM_CONDITIONAL([ENABLE_GTK_DOC], false)])

#
# pkg-config dependencies
# This unconditionally checks for all dependencies even if they are disabled. We
# later look whether all required depedencies are met and finish the
# configuration. We group similar packages into one logical group here to avoid
# having variables for each single library.
# This, however, makes ./configure output very unintuitive error messages if a
# package is not found so we must make sure we print more verbose messages
# ourself.
#

PKG_CHECK_MODULES([GLIB], [glib-2.0],
                  [have_glib=yes], [have_glib=no])

PKG_CHECK_MODULES([SYSTEMD], [libsystemd-login],
                  [have_systemd=yes], [have_systemd=no])

PKG_CHECK_MODULES([UDEV], [libudev],
                  [have_udev=yes], [have_udev=no])

PKG_CHECK_MODULES([DRM], [libdrm],
                  [have_drm=yes], [have_drm=no])

PKG_CHECK_MODULES([GBM], [gbm],
                  [have_gbm=yes], [have_gbm=no])

PKG_CHECK_MODULES([EGL], [egl],
                  [have_egl=yes], [have_egl=no])

PKG_CHECK_MODULES([GLES2], [glesv2],
                  [have_gles2=yes], [have_gles2=no])

PKG_CHECK_MODULES([XKBCOMMON], [xkbcommon],
                  [have_xkbcommon=yes], [have_xkbcommon=no])

PKG_CHECK_MODULES([PANGO], [pango pangocairo cairo],
                  [have_pango=yes], [have_pango=no])

PKG_CHECK_MODULES([FREETYPE2], [freetype2],
                  [have_freetype2=yes], [have_freetype2=no])

#
# Parse arguments
# Parse all command line arguments and save the values so we can check them
# later when configuring kmscon.
#

AC_MSG_CHECKING([whether to use systemd for multi-seat support])
AC_ARG_ENABLE([systemd],
              [AS_HELP_STRING([--enable-systemd],
                              [enable multi-seat support with systemd])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use udev for device hotplug support])
AC_ARG_ENABLE([udev],
              [AS_HELP_STRING([--enable-udev],
                              [enable device hotplug support with udev])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use uterm fbdev video backend])
AC_ARG_ENABLE([fbdev],
              [AS_HELP_STRING([--enable-fbdev],
                              [enable uterm fbdev video backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use uterm drm video backend])
AC_ARG_ENABLE([drm],
              [AS_HELP_STRING([--enable-drm],
                              [enable uterm drm video backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to provide OpenGLES2 support])
AC_ARG_ENABLE([gles2],
              [AS_HELP_STRING([--enable-gles2],
                              [provide uterm OpenGLES2 support])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use xkbcommon keyboard backend])
AC_ARG_ENABLE([xkbcommon],
              [AS_HELP_STRING([--disable-xkbcommon],
                              [disable xkbcommon keyboard backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to build with debugging on])
AC_ARG_ENABLE([debug],
              [AS_HELP_STRING([--enable-debug],
                              [whether to build with debugging on])])
AC_MSG_RESULT([ok])

#
# Debug mode
# In debug mode we use -g -O0 for compilation and set several flags so verbose
# logging is possible.
#

debug_enabled=no
if test x$enable_debug = xyes ; then
        debug_enabled=yes
fi

if test x$debug_enabled = xyes ; then
        AC_DEFINE([KMSCON_ENABLE_DEBUG], [1],
                  [Enable debug for kmscon])
        AC_DEFINE([LOG_ENABLE_DEBUG], [1],
                  [Enable debug for log subsystem])
        AC_DEFINE([LLOG_ENABLE_DEBUG], [1],
                  [Enable debug for llog subsystem])
else
        AC_DEFINE([NDEBUG], [1], [No Debug])
fi

AM_CONDITIONAL([DEBUG], [test x$debug_enabled = xyes])

#
# Main dependencies
# This checks for all dependencies that are not optional.
# TODO: remove glib dependency
#

if test ! x$have_glib = xyes ; then
        AC_ERROR([glib-2.0 no found but is required by kmscon])
fi

#
# Systemd dependency
# We can optionally use systemd for multi-seat support. If systemd is not
# available or the system was not started with systemd, we simply fall back to
# single-seat mode.
#

systemd_enabled=no
if test ! x$enable_systemd = xno ; then
        if test x$have_systemd = xyes ; then
                systemd_enabled=yes
        elif test x$enable_systemd = xyes ; then
                AC_ERROR([systemd libraries not found for multi-seat support])
        fi
fi

if test x$systemd_enabled = xyes ; then
        AC_DEFINE([UTERM_HAVE_SYSTEMD], [1],
                  [Use systemd for multi-seat support])
else
        SYSTEMD_CFLAGS=""
        SYSTEMD_LIBS=""
fi

#
# Udev dependency
# For hotplugging support we need udev to notify us about system events. If udev
# is not available, we simply fall back to static mode. Periodic scanning is
# also supported.
#

udev_enabled=no
if test ! x$enable_udev = xno ; then
        if test x$have_udev = xyes ; then
                udev_enabled=yes
        elif test x$enable_udev = xyes ; then
                AC_ERROR([udev libraries not found for device hotplug support])
        fi
fi

if test x$udev_enabled = xyes ; then
        AC_DEFINE([UTERM_HAVE_UDEV], [1],
                  [Use udev for device hotplug support])
else
        UDEV_CFLAGS=""
        UDEV_LIBS=""
fi

#
# Uterm fbdev backend
# This checks whether the fbdev backend was requested and enables it then. There
# are no special dependencies for it except the kernel headers.
# TODO: check for kernel headers here
#

fbdev_enabled=no
if test ! x$enable_fbdev = xno ; then
        fbdev_enabled=yes
fi

if test x$fbdev_enabled = xyes ; then
        AC_DEFINE([UTERM_HAVE_FBDEV], [1],
                  [Use uterm fbdev video backend])
fi

AM_CONDITIONAL([UTERM_HAVE_FBDEV], [test x$fbdev_enabled = xyes])

#
# Uterm drm backend
# This checks whether libdrm is available and some combination of libgbm, egl
# and gl or glesv2. If it is not available, then the drm backend is simply not
# built.
#

dumb_enabled=no
drm_enabled=no
gles2_enabled=no
if test ! x$enable_drm = xno ; then
        if test x$have_drm = xyes ; then
                dumb_enabled=yes
        fi

        if test ! x$enable_gles2 = xno ; then
                if test x$have_drm = xyes -a x$have_gbm = xyes -a x$have_egl = xyes ; then
                        if test x$have_gles2 = xyes ; then
                                drm_enabled=yes
                                gles2_enabled=yes
                        fi
                fi
        fi

        if test x$enable_drm = xyes -a x$dumb_enabled = xno ; then
                AC_ERROR([drm library not found for uterm dumb drm backend])
        fi

        if test x$enable_gles2 = xyes -a x$drm_enabled = xno ; then
                AC_ERROR([drm, gbm, egl, gl or gles2 libraries not found for uterm drm backend])
        fi
fi

if test x$dumb_enabled = xyes ; then
        AC_DEFINE([UTERM_HAVE_DUMB], [1],
                  [Use uterm dumb drm video backend])

        if test x$drm_enabled = xyes ; then
                AC_DEFINE([UTERM_HAVE_DRM], [1],
                          [Use uterm DRM video backend])
        else
                GBM_CFLAGS=""
                GBM_LIBS=""
                EGL_CFLAGS=""
                EGL_LIBS=""
        fi
else
        DRM_CFLAGS=""
        DRM_LIBS=""
        GBM_CFLAGS=""
        GBM_LIBS=""
        EGL_CFLAGS=""
        EGL_LIBS=""
fi

if test x$gles2_enabled = xyes ; then
        AC_DEFINE([KMSCON_HAVE_GLES2], [1],
                  [Use OpenGLESv2 as drawing backend])
else
        GLES2_CFLAGS=""
        GLES2_LIBS=""
fi

AM_CONDITIONAL([UTERM_HAVE_DUMB], [test x$dumb_enabled = xyes])
AM_CONDITIONAL([UTERM_HAVE_DRM], [test x$drm_enabled = xyes])
AM_CONDITIONAL([KMSCON_HAVE_GLES2], [test x$gles2_enabled = xyes])

#
# xkbcommon keyboard backend
# This checks for the xkbcommon library for keyboard handling in uterm. If it is
# not available, we use a dumb-keyboard backend as fall-back.
#

xkbcommon_enabled=no
if test ! x$enable_xkbcommon = xno ; then
        if test x$have_xkbcommon = xyes ; then
                xkbcommon_enabled=yes
        elif test x$enable_xkbcommon = xyes ; then
                AC_ERROR([xkbcommon not found for keyboard backend])
        fi
fi

if test x$xkbcommon_enabled = xyes ; then
        test # dummy
else
        XKBCOMMON_CFLAGS=""
        XKBCOMMON_LIBS=""
fi

AM_CONDITIONAL([UTERM_HAVE_XKBCOMMON], [test x$xkbcommon_enabled = xyes])

#
# Makefile vars
# After everything is configured, we correctly substitute the values for the
# makefiles.
#

AC_SUBST(SYSTEMD_CFLAGS)
AC_SUBST(SYSTEMD_LIBS)
AC_SUBST(DRM_CFLAGS)
AC_SUBST(DRM_LIBS)
AC_SUBST(EGL_CFLAGS)
AC_SUBST(EGL_LIBS)
AC_SUBST(GBM_CFLAGS)
AC_SUBST(GBM_LIBS)
AC_SUBST(GLES2_CFLAGS)
AC_SUBST(GLES2_LIBS)
AC_SUBST(UDEV_CFLAGS)
AC_SUBST(UDEV_LIBS)
AC_SUBST(XKBCOMMON_CFLAGS)
AC_SUBST(XKBCOMMON_LIBS)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)
AC_SUBST(FREETYPE2_CFLAGS)
AC_SUBST(FREETYPE2_LIBS)
AC_SUBST(PANGO_CFLAGS)
AC_SUBST(PANGO_LIBS)

AC_CONFIG_FILES([Makefile docs/reference/Makefile docs/reference/version.xml])
AC_OUTPUT([src/genshader.c])

#
# Configuration output
# Show configuration to the user so they can check whether everything was
# configured as expected.
#

AC_MSG_NOTICE([Build configuration:

	        debug: $debug_enabled

	      systemd: $systemd_enabled
	         udev: $udev_enabled
	    xkbcommon: $xkbcommon_enabled

	libuterm video backends:
	        fbdev: $fbdev_enabled
	     dumb drm: $dumb_enabled
	          drm: $drm_enabled
	    OpenGLES2: $gles2_enabled

	Run "make" to start compilation process])
