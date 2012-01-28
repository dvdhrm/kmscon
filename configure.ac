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

AC_MSG_CHECKING([whether to use OpenGLES2 instead of OpenGL])
AC_ARG_ENABLE([gles2],
              [AS_HELP_STRING([--enable-gles2], [whether to use OpenGLES2 instead of OpenGL])],
              [force_gles2="$enableval";
               AC_DEFINE([USE_GLES2], [1], [Define if OpenGLES2 should be used])],
              [force_gles2=no])
AC_MSG_RESULT([$force_gles2])

if test x$force_gles2 = xyes ; then
        PKG_CHECK_MODULES([OPENGLES2], [glesv2])
        OPENGL_CFLAGS=$OPENGLES2_CFLAGS
        OPENGL_LIBS=$OPENGLES2_LIBS
else
        PKG_CHECK_MODULES([OPENGL], [gl])
fi

AC_SUBST(OPENGL_CFLAGS)
AC_SUBST(OPENGL_LIBS)

PKG_CHECK_MODULES([UDEV], [libudev])
AC_SUBST(UDEV_CFLAGS)
AC_SUBST(UDEV_LIBS)

PKG_CHECK_MODULES([XPROTO], [xproto])
AC_SUBST(XPROTO_CFLAGS)
AC_SUBST(XPROTO_LIBS)

PKG_CHECK_MODULES([XKBCOMMON], [xkbcommon],
                  [have_xkbcommon=yes], [have_xkbcommon=no])
AC_SUBST(XKBCOMMON_CFLAGS)
AC_SUBST(XKBCOMMON_LIBS)

AC_MSG_CHECKING([whether to use xkbcommon keyboard backend])
AC_ARG_ENABLE([xkbcommon],
              [AS_HELP_STRING([--disable-xkbcommon],
                              [disable xkbcommon keyboard backend])])

if test ! x$enable_xkbcommon = xno ; then
        if test x$enable_xkbcommon = xyes -a x$have_xkbcommon = xno ; then
                AC_ERROR([--enable-xkbcommon given but library not found])
        fi
        enable_xkbcommon=$have_xkbcommon
fi

AM_CONDITIONAL([USE_XKBCOMMON], [test x$enable_xkbcommon = xyes])
AC_MSG_RESULT([$enable_xkbcommon])

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
              [AS_HELP_STRING([--enable-debug], [whether to build with debugging on])],
              [debug="$enableval"], [debug=no])
AM_CONDITIONAL([DEBUG], [test x$debug = xyes])
AC_MSG_RESULT([$debug])

if test ! x$debug = xyes ; then
        AC_DEFINE([NDEBUG], [1], [No Debug])
fi

AC_CONFIG_FILES([Makefile])
AC_OUTPUT([src/genshader.c])
