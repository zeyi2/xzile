
# XZile

<p align="center">
  <img src="pic/peace.jpg" alt="XZile" />
</p>

<p align="center"><em>The soul is the witness.</em></p>

XZile is a fork of [GNU Zile](https://www.gnu.org/s/zile/) with additional features. It remains a small, fast, lightweight Emacs clone for the terminal.

The file **README** in this directory contains the original GNU Zile readme.

---

## Build and install

XZile is written in C99 against POSIX.1-2008, and its only dependency is the
C library: it needs no curses, GLib or other libraries, and no configure step.
It builds on macOS and Linux with any C compiler and POSIX `make`:

```bash
make
make check
make install
```

`make install` honours the usual `prefix` and `DESTDIR` variables, e.g.
`make prefix=$HOME/.local install`; `CC` and `CFLAGS` can be overridden
the same way.

The installed binary is **xzile**, and the user init file is **~/.xzile**
(see `man xzile` and the installed `dotzile.sample` for details).

---

## Graphical version

`xzile-gui` is the same editor in its own window, for Linux and macOS:
anti-aliased fonts, truecolor themes, a modern status bar, mouse
support and the system clipboard. It is also plain C, and needs SDL2
(2.0.18 or later) and SDL2_ttf:

```bash
# Debian/Ubuntu: sudo apt install libsdl2-dev libsdl2-ttf-dev
# Fedora:        sudo dnf install SDL2-devel SDL2_ttf-devel
# macOS:         brew install sdl2 sdl2_ttf pkg-config
make gui            # builds src/xzile-gui
make install-gui    # installs xzile-gui and, on Linux, a desktop entry
make app            # macOS: builds XZile.app, to drag to Applications
```

What the window adds to the terminal editor:

- Themes `xzile-dark` (the default) and `xzile-light`; switch with
  `M-x load-theme`. The `cursor` face sets the cursor colour.
- Click to move point and select a window, drag to select, and scroll
  the window under the pointer with the wheel.
- Drag a status bar, or the border between side-by-side windows, to
  resize them.
- Kills and yanks use the system clipboard; middle-click pastes.
- Command (macOS) or Super shortcuts: ⌘C copy, ⌘X cut, ⌘V paste,
  ⌘Z undo, ⌘S save, ⌘O open, ⌘F search, ⌘A select all, ⌘W close
  window, ⌘Q quit, and ⌘= ⌘- ⌘0 (or Ctrl and the wheel) to zoom.
- Option/Alt is Meta, as in a terminal.

The font is the best monospace font installed (JetBrains Mono, Fira
Code, SF Mono, Menlo, DejaVu Sans Mono, ...). Choose another in
`~/.xzile`:

```lisp
(setq gui-font "Fira Code")   ; a family, or a font file
(setq gui-font-size 15)
```

---

## Reporting bugs

For **XZile-specific** bugs and feature requests, please open an issue or pull request at the project repository (or send an email to zeyi2@nekoarch.cc if you wish). If the issue is with upstream GNU Zile, use the [GNU Zile tracker](https://savannah.gnu.org/projects/zile/) or <bug-zile@gnu.org> as described in **README**.

---

## License and credits

Same license as GNU Zile. See **COPYING**.

Zile was written by Sandro Sigala, David A. Capello, and Reuben Thomas. The Lisp interpreter is based on code by Scott Lawrence. XZile was extended by Zeyi2 as a side project.
