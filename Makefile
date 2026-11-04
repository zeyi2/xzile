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

# The terminal build needs only the C library. This Makefile works with any
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

CORE_OBJS = \
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
	src/screen.o \
	src/search.o \
	src/selectors.o \
	src/shell.o \
	src/syntax.o \
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
	src/winhist.o \
	$(MODE_OBJS)

# The image, PDF and HTML modes, in modes/. These need only the C
# library; what draws images and PDFs is in xzile-gui alone.
MODE_OBJS = \
	modes/html.o \
	modes/image.o \
	modes/image_probe.o \
	modes/pdf.o \
	modes/preview.o \
	modes/preview_view.o

OBJS = $(CORE_OBJS) src/term.o

# The graphical frontend, xzile-gui, replaces the terminal (term.o) with
# a window drawn using SDL2 and SDL2_ttf, with image and PDF previews.
GUI_MODE_OBJS = modes/gui_preview.o modes/image_decode.o modes/pdf_render.o
GUI_OBJS = $(CORE_OBJS) src/gui.o $(GUI_MODE_OBJS)
PKG_CONFIG = pkg-config
GUI_PKGS = sdl2 SDL2_ttf SDL2_image poppler-glib cairo
GUI_CFLAGS = `$(PKG_CONFIG) --cflags $(GUI_PKGS)`
GUI_LIBS = `$(PKG_CONFIG) --libs $(GUI_PKGS)` -lm

# Linux uses a WebKitGTK helper; macOS uses the system browser.
HTML_PREVIEW_PKG = webkit2gtk-4.1

ESTR_TEST_OBJS = src/estr-test.o src/estr.o src/util.o
LAYOUT_TEST_OBJS = src/layout-test.o src/layout.o src/estr.o src/util.o
PREVIEW_TEST_OBJS = modes/preview-test.o modes/preview_view.o modes/image_probe.o src/util.o

DOCS = AUTHORS FAQ NEWS README.md src/dotzile.sample

all: src/xzile src/dotzile.sample

src/xzile: $(OBJS)
	$(CC) $(PTHREAD_FLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

GUI_RENDER_OBJS = modes/image_decode.o modes/pdf_render.o

$(OBJS) src/gui.o $(GUI_MODE_OBJS) $(GUI_RENDER_OBJS) src/estr-test.o src/layout-test.o modes/preview-test.o: src/xzile.h
src/screen.o src/term.o src/gui.o modes/gui_preview.o: src/screen.h
modes/image.o modes/image_probe.o modes/pdf.o modes/preview.o modes/preview_view.o: modes/preview.h
$(GUI_MODE_OBJS) $(GUI_RENDER_OBJS) modes/preview-test.o: modes/preview.h
src/gui.o modes/gui_preview.o: modes/gui_preview.h
modes/gui_preview.o $(GUI_RENDER_OBJS): modes/gui_render.h

gui: src/xzile-gui
	@if [ "`uname -s`" = Linux ]; then $(MAKE) src/xzile-html-preview; fi

# Rebuild old GUI objects when the feature flags or libraries change.
src/gui.o $(GUI_MODE_OBJS): Makefile

src/xzile-gui: $(GUI_OBJS)
	$(CC) $(PTHREAD_FLAGS) $(LDFLAGS) -o $@ $(GUI_OBJS) $(GUI_LIBS) $(LIBS)

src/gui.o: src/gui.c
	$(CC) $(ALL_CFLAGS) $(GUI_CFLAGS) -c -o $@ src/gui.c

modes/gui_preview.o: modes/gui_preview.c
	$(CC) $(ALL_CFLAGS) $(GUI_CFLAGS) -c -o $@ modes/gui_preview.c

modes/image_decode.o: modes/image_decode.c
	$(CC) $(ALL_CFLAGS) $(GUI_CFLAGS) -DHAVE_SDL_IMAGE -c -o $@ modes/image_decode.c

modes/pdf_render.o: modes/pdf_render.c
	$(CC) $(ALL_CFLAGS) $(GUI_CFLAGS) -DHAVE_POPPLER -c -o $@ modes/pdf_render.c

src/xzile-html-preview: modes/html_preview.c Makefile
	$(CC) $(ZILE_CPPFLAGS) $(CPPFLAGS) $(WARN_CFLAGS) $(CFLAGS) `$(PKG_CONFIG) --cflags $(HTML_PREVIEW_PKG)` \
		$(LDFLAGS) -o $@ modes/html_preview.c `$(PKG_CONFIG) --libs $(HTML_PREVIEW_PKG)` $(LIBS)

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

src/layout-test: $(LAYOUT_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(LAYOUT_TEST_OBJS) $(LIBS)

modes/preview-test: $(PREVIEW_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(PREVIEW_TEST_OBJS) $(LIBS)

check: src/xzile src/estr-test src/layout-test modes/preview-test
	src/estr-test
	src/layout-test
	modes/preview-test
	XZILE=src/xzile sh tests/run-tests.sh

install: all
	$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(docdir) $(DESTDIR)$(man1dir)
	$(INSTALL) -m 755 src/xzile $(DESTDIR)$(bindir)/$(PACKAGE)
	$(INSTALL) -m 644 $(DOCS) $(DESTDIR)$(docdir)
	$(INSTALL) -m 644 doc/xzile.1 $(DESTDIR)$(man1dir)/$(PACKAGE).1

install-gui: gui
	$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(datarootdir)/applications
	$(INSTALL) -m 755 src/xzile-gui $(DESTDIR)$(bindir)/$(PACKAGE)-gui
	$(INSTALL) -m 644 xzile.desktop $(DESTDIR)$(datarootdir)/applications/$(PACKAGE).desktop
	@if [ "`uname -s`" = Linux ]; then \
		$(INSTALL) -m 755 src/xzile-html-preview $(DESTDIR)$(bindir)/$(PACKAGE)-html-preview; \
	fi

# A macOS application bundle for xzile-gui, using the SDL2 libraries
# it was built against.
app: gui
	rm -rf XZile.app
	mkdir -p XZile.app/Contents/MacOS XZile.app/Contents/Resources
	cp src/xzile-gui XZile.app/Contents/MacOS/$(PACKAGE)-gui
	cp pic/XZile.icns XZile.app/Contents/Resources/XZile.icns
	sed 's/@VERSION@/$(VERSION)/g' Info.plist.in > XZile.app/Contents/Info.plist

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(PACKAGE) $(DESTDIR)$(man1dir)/$(PACKAGE).1
	rm -f $(DESTDIR)$(bindir)/$(PACKAGE)-gui $(DESTDIR)$(datarootdir)/applications/$(PACKAGE).desktop
	rm -f $(DESTDIR)$(bindir)/$(PACKAGE)-html-preview
	rm -rf $(DESTDIR)$(docdir)

clean:
	rm -f src/*.o src/xzile src/xzile-gui src/xzile-html-preview src/estr-test src/layout-test
	rm -f modes/*.o modes/preview-test
	rm -f src/mkdotzile src/dotzile.sample
	rm -rf XZile.app

.PHONY: all gui check install install-gui app uninstall clean
