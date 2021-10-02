AC_INIT(Makefile.am)
AM_INIT_AUTOMAKE(rpivideo, 0.0.2)
AM_MAINTAINER_MODE

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_PROG_CPP
AC_CONFIG_HEADERS(config.h)
AC_PROG_INSTALL
AC_PROG_LIBTOOL
AC_DEFINE(_GNU_SOURCE, 1, [Enable GNU Extensions])
AC_CONFIG_MACRO_DIR(m4)
PKG_PROG_PKG_CONFIG

dnl Debugging options to enable debug macros
DEBUG_FLAGS=
AC_ARG_ENABLE(debug,     [  --enable-debug                compile with DEBUG],,enable_debug=no)

if test "x$enable_debug" = "xyes"; then
    DEBUG_FLAGS="$DEBUG_FLAGS -DDEBUG -g -fno-omit-frame-pointer"
else
    DEBUG_FLAGS="$DEBUG_FLAGS -DG_DISABLE_CHECKS -DG_DISABLE_CAST_CHECKS -DG_DISABLE_ASSERT"
fi

CFLAGS="$CFLAGS $DEBUG_FLAGS -Wall -Wextra -Wmissing-prototypes -Wmissing-declarations"

PKG_CHECK_MODULES(GLIB, glib-2.0 libmosquitto)
AC_SUBST(GLIB_LIBS)
AC_SUBST(GLIB_CFLAGS)

PKG_CHECK_MODULES(GST, gstreamer-1.0 gstreamer-plugins-base-1.0)
AC_SUBST(GST_LIBS)
AC_SUBST(GST_CFLAGS)

AC_OUTPUT(
	Makefile \
	src/Makefile
)

