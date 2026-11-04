
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
(2.0.18 or later), SDL2_ttf, SDL2_image, poppler-glib and cairo.
Linux also needs WebKitGTK for HTML previews:

```bash
# Debian/Ubuntu: sudo apt install pkg-config libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libpoppler-glib-dev libcairo2-dev libwebkit2gtk-4.1-dev
# Fedora:        sudo dnf install pkgconf-pkg-config SDL2-devel SDL2_ttf-devel SDL2_image-devel poppler-glib-devel cairo-devel webkit2gtk4.1-devel
# macOS:         brew install sdl2 sdl2_ttf sdl2_image poppler cairo pkg-config
make gui            # builds the GUI with all previews (including the Linux HTML helper)
make install-gui    # installs the GUI and, on Linux, its helper and desktop entry
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

### Image, PDF and HTML previews

`make gui` includes image, PDF and HTML previews. The terminal `xzile`
still needs only the C library. The modes live in
`modes/`: `image.c`, `pdf.c` and `html.c`, with what images and PDFs
share in `preview.c`, and decoding in `image_decode.c` and
`pdf_render.c`.

Visiting a PNG, JPEG, GIF, WebP, TIFF, BMP or PDF file shows it as a
preview, in a buffer that has no text and cannot be saved, so that the
file cannot be overwritten by mistake. Set `preview-binary-files` to
nil to visit such files as text. `M-x image-mode` and `M-x pdf-mode`
show the visited file as a preview; `C-c C-c` switches a text file,
such as an SVG, between its preview and its text. In a preview:

- `+` `-` zoom, `0` resets, `1` shows actual size, `W` `H` `P` fit
  the width, height or page; Ctrl and the wheel zoom at the pointer.
- The arrows (or `C-f` `C-b` `C-n` `C-p`), the wheel and dragging move
  around. Shift and the wheel move sideways.
- In a PDF, `n` `p` (PageDown, PageUp) turn pages, `SPC` and `DEL`
  scroll a screen at a time onto the next or previous page, `M-<` `M->`
  go to the first and last page and `M-g g` to any.
- `g` reloads, `i` describes the preview, `q` quits. A preview also
  reloads when its file changes on disk.

Images and PDF pages are decoded on the CPU, in two background
threads, so that the editor keeps responding while a large file
loads. The results become textures, which the renderer scales and
moves when you zoom and pan, without decoding again. The renderer is
SDL's GPU-accelerated one when the system provides it, otherwise its
software renderer; `i` says which. PDF pages are rendered only when
shown or about to be (one page either side), at the size shown.
The texture cache has a soft limit of 256 MiB: beyond it, textures not
on screen are freed, least recently shown first. Textures on screen are
kept, so with several large images or pages visible at once the total
can go past the limit, by up to about 128 MiB per image and 48 MiB per
PDF page shown. Very large images are shown at reduced resolution or,
beyond 128 megapixels, not at all. A PDF page that fails to render
shows why in its window and is not tried again until you reload. The size is
read from the file's header before anything is decoded; an image whose
header does not give it (a TIFF without its dimensions, an SVG sized
in percent) is not shown, nor is a format other than those above.

HTML files are edited as text in `html-mode`. `C-c C-v`
(`html-preview`) shows the file rendered by a browser engine, in a
window of its own: on Linux, the WebKitGTK helper `xzile-html-preview`,
which runs pages with JavaScript, composites on the GPU when it can,
and reloads when the file is saved. `C-c C-v` again reloads it and
brings it forward, `C-c C-k` closes it, and it closes with its buffer
or the editor. On Linux, `make gui` builds the helper and
`make install-gui` installs it alongside the editor.

Without the helper, `html-preview` opens the file with `xdg-open`. On
macOS there is no helper: `html-preview` opens the file in the default
browser, which you reload yourself. Set `html-preview-gpu` to nil if
the helper draws badly with your graphics driver; it then draws on the
CPU.

---

## Reporting bugs

For **XZile-specific** bugs and feature requests, please open an issue or pull request at the project repository (or send an email to zeyi2@nekoarch.cc if you wish). If the issue is with upstream GNU Zile, use the [GNU Zile tracker](https://savannah.gnu.org/projects/zile/) or <bug-zile@gnu.org> as described in **README**.

---

## License and credits

Same license as GNU Zile. See **COPYING**.

Zile was written by Sandro Sigala, David A. Capello, and Reuben Thomas. The Lisp interpreter is based on code by Scott Lawrence. XZile was extended by Zeyi2 as a side project.
