AC_PREREQ(2.68)

AC_INIT([kmscon], [0.0])
AC_SUBST(PACKAGE_URL, [https://github.com/dvdhrm/kmscon])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADER(config.h)

AM_INIT_AUTOMAKE([foreign 1.11 subdir-objects dist-bzip2 no-dist-gzip tar-pax -Wall -Werror])
AM_SILENT_RULES([yes])

# Don't add a default "-g -O2" if CFLAGS wasn't specified
: ${CFLAGS=""}

AC_USE_SYSTEM_EXTENSIONS
AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O

LT_PREREQ(2.2)
LT_INIT

PKG_CHECK_MODULES([DRM], [libdrm])
AC_SUBST(DRM_CFLAGS)
AC_SUBST(DRM_LIBS)

PKG_CHECK_MODULES([EGL], [egl])
AC_SUBST(EGL_CFLAGS)
AC_SUBST(EGL_LIBS)

PKG_CHECK_MODULES([GBM], [gbm])
AC_SUBST(GBM_CFLAGS)
AC_SUBST(GBM_LIBS)

PKG_CHECK_MODULES([OPENGL], [gl])
AC_SUBST(OPENGL_CFLAGS)
AC_SUBST(OPENGL_LIBS)

PKG_CHECK_MODULES([UDEV], [libudev])
AC_SUBST(UDEV_CFLAGS)
AC_SUBST(UDEV_LIBS)

PKG_CHECK_MODULES([XKBCOMMON], [xkbcommon])
AC_SUBST(XKBCOMMON_CFLAGS)
AC_SUBST(XKBCOMMON_LIBS)

PKG_CHECK_MODULES([GLIB], [glib-2.0])
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES([FREETYPE2], [freetype2])
AC_SUBST(FREETYPE2_CFLAGS)
AC_SUBST(FREETYPE2_LIBS)

PKG_CHECK_MODULES([PANGO], [pango pangocairo cairo],
                  [have_pango=yes], [have_pango=no])
AC_SUBST(PANGO_CFLAGS)
AC_SUBST(PANGO_LIBS)

AC_MSG_CHECKING([whether to use pango font backend])
AC_ARG_ENABLE([pango],
              [AS_HELP_STRING([--enable-pango], [whether to use pango font backend])],
              [force_pango="$enableval";
               AC_DEFINE([USE_PANGO], [1], [Define if pango should be used])],
              [force_pango=no])
AC_MSG_RESULT([$force_pango])

if test x$force_pango = xyes -a $have_pango = xno ; then
	AC_ERROR([Pango library not found])
fi

AM_CONDITIONAL([USE_PANGO], [test x$force_pango = xyes])

AC_MSG_CHECKING([whether to build with debugging on])
AC_ARG_ENABLE([debug],
              [AS_HELP_STRING([--enable-debug], [whether to build with debugging on)])],
              [debug="$enableval"],
              [debug=no; AC_DEFINE([NDEBUG], [1], [No Debug])])
AM_CONDITIONAL([DEBUG], [test x$debug = xyes])
AC_MSG_RESULT([$debug])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
