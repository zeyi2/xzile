# configure.ac
#
# Copyright (c) 1997-2021 Free Software Foundation, Inc.
#
# This file is part of GNU Zile.
#
# GNU Zile is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published
# by the Free Software Foundation; either version 3, or (at your
# option) any later version.
#
# GNU Zile is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

AC_PREREQ([2.69])

# Initialise autoconf and automake
AC_INIT([Zile],[2.6.3],[bug-zile@gnu.org])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([-Wall std-options])

# Checks for programs
gl_EARLY
AM_PROG_CC_C_O
AM_PROG_AR
PKG_PROG_PKG_CONFIG
AX_CHECK_GNU_MAKE(,[AC_MSG_WARN([GNU make is required to build from Vala sources])])

# Vala
AM_PROG_VALAC(0.56,,[AC_MSG_WARN([valac not found or too old: cannot compile Vala sources])])
PKG_CHECK_MODULES(GLIB, [glib-2.0 gobject-2.0 gio-2.0 gee-0.8])

# help2man
# Set a value even if not found, so that an invocation via build-aux/missing works
AC_PATH_PROG([HELP2MAN], [help2man], [help2man])

# Force use of included (patched) regex
AC_MSG_WARN([Forcing --with-included-regex, to use our patched version])
with_included_regex=yes

# Initialize gnulib
gl_INIT

# Curses
AX_WITH_CURSES
if test "$ax_cv_curses" != "yes"; then
  AC_MSG_ERROR([cannot find curses])
fi
AC_ARG_VAR(CURSES_LIB, [linker flags for curses library])

# Perl
AC_PATH_PROG([PERL], [perl])
if test -z "$PERL"; then
  AC_MSG_FAILURE([cannot find perl])
fi

# Generate output
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile lib/Makefile])
AC_OUTPUT
