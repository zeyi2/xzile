# Makefile for XZile
#
# Copyright (c) 1997-2020 Free Software Foundation, Inc.
# Copyright (c) 2025-2026 Zeyi2 <zeyi2@nekoarch.cc>
#
# This file is part of XZile.
#
# XZile is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# XZile is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

# The only dependency is the C library. This Makefile works with any
# POSIX make (GNU make, BSD make).

PACKAGE = xzile
PACKAGE_NAME = XZile
VERSION = 2.7.0
PACKAGE_BUGREPORT = bug-zile@gnu.org

prefix = /usr/local
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
datarootdir = $(prefix)/share
docdir = $(datarootdir)/doc/$(PACKAGE)
mandir = $(datarootdir)/man
man1dir = $(mandir)/man1
DESTDIR =

CC = cc
CFLAGS = -O2 -g
# POSIX threads, from the C library.
PTHREAD_FLAGS = -pthread
WARN_CFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS =
LDFLAGS =
LIBS =
INSTALL = install
SHELL = /bin/sh

ZILE_CPPFLAGS = -DPACKAGE=\"$(PACKAGE)\" -DPACKAGE_NAME=\"$(PACKAGE_NAME)\" \
	-DVERSION=\"$(VERSION)\" -DPACKAGE_BUGREPORT=\"$(PACKAGE_BUGREPORT)\" \
	-DPATH_DOCDIR=\"$(docdir)\"

ALL_CFLAGS = $(ZILE_CPPFLAGS) $(CPPFLAGS) $(WARN_CFLAGS) $(PTHREAD_FLAGS) $(CFLAGS)

OBJS = \
	src/basic.o \
	src/bind.o \
	src/buffer.o \
	src/clipboard.o \
	src/compile.o \
	src/completion.o \
	src/debug.o \
	src/dired.o \
	src/estr.o \
	src/eval.o \
	src/file.o \
	src/funcs.o \
	src/getkey.o \
	src/help.o \
	src/history.o \
	src/keycode.o \
	src/killring.o \
	src/layout.o \
	src/line.o \
	src/lisp.o \
	src/macro.o \
	src/magit.o \
	src/main.o \
	src/marker.o \
	src/minibuf.o \
	src/paren.o \
	src/picker.o \
	src/process.o \
	src/redisplay.o \
	src/regex.o \
	src/registers.o \
	src/search.o \
	src/selectors.o \
	src/shell.o \
	src/syntax.o \
	src/term.o \
	src/term_minibuf.o \
	src/term_redisplay.o \
	src/terminal.o \
	src/theme.o \
	src/undo.o \
	src/utf8.o \
	src/util.o \
	src/variables.o \
	src/vterm.o \
	src/window.o \
	src/winhist.o

ESTR_TEST_OBJS = src/estr-test.o src/estr.o src/util.o

DOCS = AUTHORS FAQ NEWS README.md src/dotzile.sample

all: src/xzile src/dotzile.sample

src/xzile: $(OBJS)
	$(CC) $(PTHREAD_FLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

$(OBJS) src/estr-test.o: src/xzile.h
src/variables.o src/mkdotzile: src/tbl_vars.h

.SUFFIXES: .c .o
.c.o:
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

src/mkdotzile: src/mkdotzile.c
	$(CC) $(ALL_CFLAGS) $(LDFLAGS) -o $@ src/mkdotzile.c

src/dotzile.sample: src/mkdotzile
	src/mkdotzile > $@

src/estr-test: $(ESTR_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(ESTR_TEST_OBJS) $(LIBS)

check: src/xzile src/estr-test
	src/estr-test
	XZILE=src/xzile sh tests/run-tests.sh

install: all
	$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(docdir) $(DESTDIR)$(man1dir)
	$(INSTALL) -m 755 src/xzile $(DESTDIR)$(bindir)/$(PACKAGE)
	$(INSTALL) -m 644 $(DOCS) $(DESTDIR)$(docdir)
	$(INSTALL) -m 644 doc/xzile.1 $(DESTDIR)$(man1dir)/$(PACKAGE).1

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(PACKAGE) $(DESTDIR)$(man1dir)/$(PACKAGE).1
	rm -rf $(DESTDIR)$(docdir)

clean:
	rm -f src/*.o src/xzile src/estr-test src/mkdotzile src/dotzile.sample

.PHONY: all check install uninstall clean
