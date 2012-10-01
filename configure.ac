#
# Kmscon - build configuration script
# Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
#

AC_PREREQ(2.68)

AC_INIT([kmscon], [5])
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
# pkg-config dependencies
# This unconditionally checks for all dependencies even if they are disabled. We
# later look whether all required depedencies are met and finish the
# configuration. We group similar packages into one logical group here to avoid
# having variables for each single library.
# This, however, makes ./configure output very unintuitive error messages if a
# package is not found so we must make sure we print more verbose messages
# ourself.
#

PKG_CHECK_MODULES([SYSTEMD], [libsystemd-login],
                  [have_systemd=yes], [have_systemd=no])

PKG_CHECK_MODULES([UDEV], [libudev],
                  [have_udev=yes], [have_udev=no])

PKG_CHECK_MODULES([DBUS], [dbus-1],
                  [have_dbus=yes], [have_dbus=no])

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

PKG_CHECK_MODULES([FREETYPE2], [freetype2 fontconfig],
                  [have_freetype2=yes], [have_freetype2=no])

PKG_CHECK_MODULES([PANGO], [pango pangoft2],
                  [have_pango=yes], [have_pango=no])

PKG_CHECK_MODULES([WAYLAND], [wayland-client wayland-server wayland-cursor],
                  [have_wayland=yes], [have_wayland=no])

#
# Parse arguments for applications
# Parse all command-line arguments that enable or disable build of applications.
# This allows us to compute which dependencies we need.
#

AC_MSG_CHECKING([whether to compile eloop])
AC_ARG_ENABLE([eloop],
              [AS_HELP_STRING([--enable-eloop],
                              [build eloop library])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to compile TSM])
AC_ARG_ENABLE([tsm],
              [AS_HELP_STRING([--enable-tsm],
                              [build tsm library])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to compile uterm])
AC_ARG_ENABLE([uterm],
              [AS_HELP_STRING([--enable-uterm],
                              [build uterm library])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to compile kmscon])
AC_ARG_ENABLE([kmscon],
              [AS_HELP_STRING([--disable-kmscon],
                              [do not build kmscon])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to build wlterm])
AC_ARG_ENABLE([wlterm],
              [AS_HELP_STRING([--enable-wlterm],
                              [build wlterm])])
AC_MSG_RESULT([ok])

#
# Parse depedency arguments
# This parses all command-line arguments to check for which dependencies should
# be used and how the applications should be built.
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

AC_MSG_CHECKING([whether to use dbus for IPC])
AC_ARG_ENABLE([dbus],
              [AS_HELP_STRING([--enable-dbus],
                              [enable dbus IPC])])
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

AC_MSG_CHECKING([whether to use static unifont backend])
AC_ARG_ENABLE([unifont],
              [AS_HELP_STRING([--enable-unifont],
                              [enable static unifont font backend (GPL)])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use static 8x16 font backend])
AC_ARG_ENABLE([f8x16],
              [AS_HELP_STRING([--disable-f8x16],
                              [disable static 8x16 font backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use freetype2 font backend])
AC_ARG_ENABLE([freetype2],
              [AS_HELP_STRING([--disable-freetype2],
                              [disable freetype2 font backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use pango font backend])
AC_ARG_ENABLE([pango],
              [AS_HELP_STRING([--disable-pango],
                              [disable pango font backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use bblit rendering backend])
AC_ARG_ENABLE([bblit],
              [AS_HELP_STRING([--disable-bblit],
                              [disable bblit rendering backend])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to use bbulk rendering backend])
AC_ARG_ENABLE([bbulk],
              [AS_HELP_STRING([--disable-bbulk],
                              [disable bbulk rendering backend])])
AC_MSG_RESULT([ok])

#
# Parse miscellaneous arguments
# These don't belong into the categories above for several reason so they are
# handled specially.
# They configure compiler options and similar.
#

AC_MSG_CHECKING([whether to build with debugging on])
AC_ARG_ENABLE([debug],
              [AS_HELP_STRING([--enable-debug],
                              [whether to build with debugging on])])
AC_MSG_RESULT([ok])

AC_MSG_CHECKING([whether to disable code optimizations])
AC_ARG_ENABLE([optimizations],
              [AS_HELP_STRING([--disable-optimizations],
                              [whether to disable code optimizations])])
AC_MSG_RESULT([ok])

#
# Debug mode and code optimizations
# In debug mode we compile with -g and enable several debug-messages and flags.
# With optimizations (default), we add -O2 to compile-flags.
#

debug_enabled=no
if test x$enable_debug = xyes ; then
        debug_enabled=yes
fi

optimizations_enabled=no
if test ! x$enable_optimizations = xno ; then
        optimizations_enabled=yes
fi

if test x$debug_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_DEBUG], [1],
                  [Enable debug mode])
else
        AC_DEFINE([NDEBUG], [1], [No Debug])
fi

AM_CONDITIONAL([BUILD_ENABLE_DEBUG],
               [test x$debug_enabled = xyes])
AM_CONDITIONAL([BUILD_ENABLE_OPTIMIZATIONS],
               [test x$optimizations_enabled = xyes])

# check for _Static_assert
AC_MSG_CHECKING([whether _Static_assert() is supported])
AC_LANG([C])
have_static_assert=yes
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[_Static_assert(1, "What?");]])],
                  [AC_DEFINE([BUILD_HAVE_STATIC_ASSERT],
                             [1],
                             [Define to 1 if _Static_assert() is supported])],
                  [have_static_assert=no])
AC_MSG_RESULT([$have_static_assert])

#
# Main applications
# This checks which applications to build so we can optionally disable unused
# dependencies below.
#
# Dependency Calculations
# The first thing we do is checking which applications to build. Then we
# forcibly enable all libraries that are needed by these and then we forcibly
# enable all other source-code dependecies.
#
# The "enable_XY" flag is user input that is set to "yes" if they enabled it
# explicitly, "no" if they disabled it explicitly or "" (empty) if they did
# not specify it explicitly. In the latter case we enable it if all dependencies
# are met, otherwise, we disable it. This does not work for applications,
# though. If an application is not specified explicitly by the user, then we
# force the default value.
# An application also forces all _hard_ dependencies to "yes" and so fails if
# they cannot be met.
#

kmscon_enabled=no
if test ! x$enable_kmscon = xno ; then
        kmscon_enabled=yes
        enable_eloop=yes
        enable_tsm=yes
        enable_uterm=yes
fi

AM_CONDITIONAL([BUILD_ENABLE_KMSCON],
               [test x$kmscon_enabled = xyes])

wlterm_enabled=no
if test x$enable_wlterm = xyes ; then
        wlterm_enabled=yes
        enable_eloop=yes
        enable_tsm=yes
        enable_wayland=yes
fi

AM_CONDITIONAL([BUILD_ENABLE_WLTERM],
               [test x$wlterm_enabled = xyes])

uterm_enabled=no
if test x$enable_uterm = xyes ; then
        uterm_enabled=yes
        eloop_enabled=yes
fi

AM_CONDITIONAL([BUILD_ENABLE_UTERM],
               [test x$uterm_enabled = xyes])

eloop_enabled=no
if test x$enable_eloop = xyes ; then
        eloop_enabled=yes
fi

AM_CONDITIONAL([BUILD_ENABLE_ELOOP],
               [test x$eloop_enabled = xyes])

tsm_enabled=no
if test x$enable_tsm = xyes ; then
        tsm_enabled=yes
fi

AM_CONDITIONAL([BUILD_ENABLE_TSM],
               [test x$tsm_enabled = xyes])

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
        AC_DEFINE([BUILD_ENABLE_SYSTEMD], [1],
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
        AC_DEFINE([BUILD_ENABLE_UDEV], [1],
                  [Use udev for device hotplug support])
else
        UDEV_CFLAGS=""
        UDEV_LIBS=""
fi

#
# DBus dependency
# For IPC mechanisms we use DBus. Especially multi-seat enabled multi-session
# capable applications need DBus to manage application and terminal switching.
#

dbus_enabled=no
if test x$enable_dbus = xyes ; then
        if test x$have_dbus = xyes ; then
                dbus_enabled=yes
        elif test x$enable_dbus = xyes; then
                AC_ERROR([dbus libraries not found])
        fi
fi

if test x$dbus_enabled = xyes; then
        AC_DEFINE([BUILD_ENABLE_DBUS], [1],
                  [Use dbus for IPC])
else
        DBUS_CFLAGS=""
        DBUS_LIBS=""
fi

AM_CONDITIONAL([BUILD_ENABLE_DBUS], [test x$dbus_enabled = xyes])

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
        AC_DEFINE([BUILD_ENABLE_FBDEV], [1],
                  [Use uterm fbdev video backend])
fi

AM_CONDITIONAL([BUILD_ENABLE_FBDEV], [test x$fbdev_enabled = xyes])

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
        AC_DEFINE([BUILD_ENABLE_DUMB], [1],
                  [Use uterm dumb drm video backend])

        if test x$drm_enabled = xyes ; then
                AC_DEFINE([BUILD_ENABLE_DRM], [1],
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
        AC_DEFINE([BUILD_ENABLE_GLES2], [1],
                  [Use OpenGLESv2 as drawing backend])
else
        GLES2_CFLAGS=""
        GLES2_LIBS=""
fi

AM_CONDITIONAL([BUILD_ENABLE_DUMB], [test x$dumb_enabled = xyes])
AM_CONDITIONAL([BUILD_ENABLE_DRM], [test x$drm_enabled = xyes])
AM_CONDITIONAL([BUILD_ENABLE_GLES2], [test x$gles2_enabled = xyes])

# check for gbm_bo_get_stride() function, otherwise gbm_bo_get_pitch() is used
if test x$have_gbm = xyes ; then
        save_CFLAGS="$CFLAGS"
        save_LIBS="$LIBS"
        CFLAGS="$CFLAGS $GBM_CFLAGS $DRM_CFLAGS"
        LIBS="$LIBS $GBM_LIBS $DRM_LIBS"
        AC_CHECK_LIB([gbm],
                     [gbm_bo_get_stride],
                     [AC_DEFINE([BUILD_HAVE_GBM_BO_GET_STRIDE],
                                [1],
                                [Define to 1 if your libgbm provides gbm_bo_get_stride])])
        CFLAGS="$save_CFLAGS"
        LIBS="$save_LIBS"
fi

#
# xkbcommon keyboard backend
# This checks for the xkbcommon library for keyboard handling in uterm. We
# require this library as there is no other way to handle keyboard input
# properly.
#

xkbcommon_enabled=no
if test ! x$have_xkbcommon = xno ; then
        xkbcommon_enabled=yes
else
        AC_ERROR([xkbcommon not found for keyboard backend])
fi

#
# Font backends
# This checks for the unifont, 8x16, freetype2 and pango font backends and
# enables them if requested and available.
#
# Please note that the Unifont-data is GPL'ed and we compile this statically
# into our application. I do not consider this a "derivative" but a lawyer may
# disagree. So please make sure you enable this only if you use the GPL as
# license for kmscon.
#

unifont_enabled=no
if test x$enable_unifont = xyes ; then
        unifont_enabled=yes
fi

if test x$unifont_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_UNIFONT], [1],
                  [Use static unifont backend])
fi

AM_CONDITIONAL([BUILD_ENABLE_UNIFONT], [test x$unifont_enabled = xyes])

f8x16_enabled=no
if test ! x$enable_f8x16 = xno ; then
        f8x16_enabled=yes
fi

if test x$f8x16_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_8X16], [1],
                  [Use static 8x16 font backend])
fi

AM_CONDITIONAL([BUILD_ENABLE_8X16], [test x$f8x16_enabled = xyes])

freetype2_enabled=no
if test ! x$enable_freetype2 = xno ; then
        if test x$have_freetype2 = xyes ; then
                freetype2_enabled=yes
        elif test x$enable_freetype2 = xyes ; then
                AC_ERROR([freetype2/fontconfig not found for freetype2 backend])
        fi
fi

if test x$freetype2_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_FREETYPE2], [1],
                  [Use freetype2 as font backend])
else
        FREETYPE2_CFLAGS=""
        FREETYPE2_LIBS=""
fi

AM_CONDITIONAL([BUILD_ENABLE_FREETYPE2], [test x$freetype2_enabled = xyes])

pango_enabled=no
if test ! x$enable_pango = xno ; then
        if test x$have_pango = xyes ; then
                pango_enabled=yes
        elif test x$enable_pango = xyes ; then
                AC_ERROR([pango not found for pango font backend])
        fi
fi

if test x$pango_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_PANGO], [1],
                  [Use pango as font backend])
else
        PANGO_CFLAGS=""
        PANGO_LIBS=""
fi

AM_CONDITIONAL([BUILD_ENABLE_PANGO], [test x$pango_enabled = xyes])

#
# BBlit Rendering backend
#

bblit_enabled=no
if test ! x$enable_bblit = xno ; then
        bblit_enabled=yes
fi

if test x$bblit_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_BBLIT], [1],
                  [Use bblit rendering backend])
fi

AM_CONDITIONAL([BUILD_ENABLE_BBLIT], [test x$bblit_enabled = xyes])

#
# BBulk Rendering backend
#

bbulk_enabled=no
if test ! x$enable_bbulk = xno ; then
        bbulk_enabled=yes
fi

if test x$bbulk_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_BBULK], [1],
                  [Use bbulk rendering backend])
fi

AM_CONDITIONAL([BUILD_ENABLE_BBULK], [test x$bbulk_enabled = xyes])

#
# OpenGL Texture rendering backend
# This is not really an option but automatically enabled if OpenGLES2 support
# was selected.
#

gltex_enabled=no
if test x$gles2_enabled = xyes ; then
        gltex_enabled=yes
fi

#
# Wayland
# This checks whether the wayland libraries are needed and available.
#

wayland_enabled=no
if test ! x$enable_wayland = xno ; then
        if test x$have_wayland = xyes ; then
                wayland_enabled=yes
        elif test x$enable_wayland = xyes ; then
                AC_ERROR([wayland libraries not found])
        fi
fi

if test x$wayland_enabled = xyes ; then
        AC_DEFINE([BUILD_ENABLE_WAYLAND], [1],
                  [Enable wayland backends])
else
        WAYLAND_CFLAGS=""
        WAYLAND_LIBS=""
fi

#
# Makefile vars
# After everything is configured, we correctly substitute the values for the
# makefiles.
#

AC_SUBST(SYSTEMD_CFLAGS)
AC_SUBST(SYSTEMD_LIBS)
AC_SUBST(DBUS_CFLAGS)
AC_SUBST(DBUS_LIBS)
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
AC_SUBST(FREETYPE2_CFLAGS)
AC_SUBST(FREETYPE2_LIBS)
AC_SUBST(PANGO_CFLAGS)
AC_SUBST(PANGO_LIBS)
AC_SUBST(WAYLAND_CFLAGS)
AC_SUBST(WAYLAND_LIBS)

AC_CONFIG_FILES([Makefile libeloop.pc libtsm.pc libuterm.pc])
AC_OUTPUT

#
# Configuration output
# Show configuration to the user so they can check whether everything was
# configured as expected.
#

AC_MSG_NOTICE([Build configuration:

  Applications and libraries:
	       kmscon: $kmscon_enabled
	       wlterm: $wlterm_enabled
	        uterm: $uterm_enabled
	        eloop: $eloop_enabled
	          tsm: $tsm_enabled

  Miscellaneous options:
	        debug: $debug_enabled
	optimizations: $optimizations_enabled

  Optional dependencies:
	      systemd: $systemd_enabled
	         udev: $udev_enabled
	         dbus: $dbus_enabled
	    xkbcommon: $xkbcommon_enabled

  libuterm video backends:
	        fbdev: $fbdev_enabled
	     dumb drm: $dumb_enabled
	          drm: $drm_enabled
	    OpenGLES2: $gles2_enabled

  font backends:
	      unifont: $unifont_enabled
	         8x16: $f8x16_enabled
	    freetype2: $freetype2_enabled
	        pango: $pango_enabled

  rendering backends:
	        bblit: $bblit_enabled
	        bbulk: $bbulk_enabled
	        gltex: $gltex_enabled

	Run "make" to start compilation process])
