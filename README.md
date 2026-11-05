
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
make check-gui      # Linux: tests the GUI's keyboard, input method and mouse handling
```

What the window adds to the terminal editor:

- Themes `xzile-dark` (the default) and `xzile-light`; switch with
  `M-x load-theme`. The `cursor` face sets the cursor colour.
- Click to move point and select a window, drag to select, and scroll
  the window under the pointer with the wheel.
- Click a `FILE:LINE[:COL]` reference in a terminal or `*compilation*`
  to visit that place in the window on the left (a sole window is
  split side by side for it), or a name in Dired to visit that file
  there or list that directory in place. This works in `xzile` too.
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

### Remote files over SSH

`C-x C-f` and `C-x d` can open files and directories on other
machines. The file picker's prompt names the location
(`Find file: [Local] ~/src/` or `Find file: [ssh:host] ~/`), and
`M-l` (shown with the other keys, as space allows) chooses another:
Local, a machine used in this session or saved from an earlier one, a
`Host` alias from `~/.ssh/config` (or `ssh-config-file`), or *New SSH
connection...*, which asks for the host, user and port. You can also
type `[user@]host[:port]`. The path is then edited and completed as
usual on that machine, starting where you were last in this dialog,
or else in the directory you last browsed there. `M-l` or `C-g` in the
location list returns to the file name as it was. From a remote
buffer, `C-x C-f` and `C-x d` start on its machine; nothing else
changes machine, so local buffers stay local. The mode line and the
buffer list show remote buffers as `ssh:host`.

Connections that worked, and the directory last browsed on each
machine, are saved in `~/.xzile.d/locations` as soon as a connection
succeeds or a remote file or directory is opened; the local directory
is saved when a file or directory is opened through this picker. A
failed connection is not saved. The file holds only host, user, port
and directory (at most 32 connections), never passwords, keys or
sockets. `~/.xzile.d` is created with mode 700, and is not used if it
is a symbolic link, someone else's, or writable by others; the file
has mode 600. Each save holds an `fcntl` lock on
`~/.xzile.d/locations.lock`, reads the file again, changes the one
location and renames a new file over the old, so sessions sharing it
keep each other's changes and never see a partial file. A save that
fails (or waits more than 2 seconds for another session's lock)
leaves the file as it was, does not undo what you opened, and shows
`Warning: locations not saved: ...` once, until a save works again.
The first line is `xzile-locations 1`; then one record per line, most
recent first, with tab-separated fields:

    local   DIR
    ssh     HOST    USER    PORT    DIR

`USER`, `PORT` and `DIR` are empty when not known; `\\`, `\t`, `\n` and
`\r` stand for a backslash, tab, newline and carriage return. Records
that do not parse are ignored (and dropped on the next save); a file
of another version, or one that cannot be read, is left alone (saves
then fail, with the warning). With `-q` (`--no-init-file`) the file
is neither read nor written.

Remote buffers support visiting, creating, saving and reverting text
files, and Dired for browsing (`RET`, `o`, `v`, `^`, `g`, marks).
Saving writes only if the file is unchanged since it was read or
saved, otherwise it asks; the buffer stays modified unless the remote
side confirms what it wrote. Dired's `R` `C` `D` `x` `M` `+`, and
compile, `cd`, `write-file` and previews are refused in remote
buffers rather than run locally, and a remote failure never falls
back to a local file or program.

Magit works on a repository on a remote machine: `C-x g`
(`magit-status`) in a remote buffer (a file, a Dired listing or a
terminal there) finds the repository containing its directory on that
machine, and every git command for it, including staging hunks and
committing (the patch or message goes to git's standard input), runs
there, in the repository, through the shared connection. A repository
is known by its machine and its top directory, so the same path here
and on another machine (or on two machines) gives separate status,
log, diff and `$` process buffers, shown as `ssh:host:dir` in the
buffer list; a change in one refreshes only that repository's
buffers. `RET` visits files and directories there as remote buffers,
and commit messages are edited in a local buffer that commits to that
repository. Called from Lisp with a directory, `(magit-status DIR)`
looks for the repository on the current buffer's machine: a relative
DIR is relative to the buffer's directory there, and `~` is that
machine's home. A refresh runs its git commands there one after the
other, not in parallel, as each waits for ssh; `C-g` cancels a
command and `remote-timeout` bounds it, as for shell commands, and a
refresh that does not finish leaves the buffer as it was. Push, pull
and fetch run there in the background, without `remote-timeout`. The
remote machine needs `git` (and `env`) in the `PATH` of
non-interactive ssh sessions. Visited remote files are not reverted
when git changes them; revert them with `M-x revert-buffer`.

Shells and shell commands started from a remote buffer (a file, a
Dired listing or a terminal there) run on its machine, in its
directory there. `M-!` (`shell-command`) and `M-|`
(`shell-command-on-region`) run the command with `sh`, with the
region as standard input, and show or insert its output and errors as
for a local command. `C-g` cancels them and `remote-timeout` bounds
them: XZile stops waiting, and tries to stop the command on the remote
machine by sending SIGTERM to its process group there. This is best
effort. It needs `ps` on the remote machine to find the group, and a
working connection to send the signal; a command can also ignore
SIGTERM, or leave the group (as a daemon does), and go on running. A
command that cannot be run in the directory or does not finish
(cancelled, timed out, killed, or the connection lost) inserts nothing
and leaves the region as it was.

`M-x shell` runs the remote user's `$SHELL` as a login shell, and
`M-x term` a program (by default that shell), on a remote
pseudo-terminal (`ssh -tt`, through the shared connection): keys,
`C-c`, `C-d` and window size changes reach the remote program, and
`remote-timeout` does not limit the session. Remote shells have
buffers of their own, named after the machine (`*shell ssh:host*`,
`*shell ssh:host*<2>`, ..., `*terminal ssh:host*`): in a remote
buffer, `M-x shell`, `C-u M-x shell` and `C-u N M-x shell` find or
start shells on that machine only, and in a local buffer local ones.
A remote terminal buffer keeps its machine and directory (shown as
`ssh:host:dir` in the buffer list) without visiting a file, so
`C-x C-f`, `C-x d` and shell commands started there use that machine.
Its directory follows the shell's OSC 7 reports (`file://HOST/PATH`)
as a remote path, when HOST is that machine: empty, `localhost`, the
host name it was reached with, or its `uname -n`. Names are compared
ignoring case, and a short name also matches that name with a domain
(`host` and `host.example`, but not `host.example` and
`host.example.other`). Reports for other hosts, such as those of a
shell that ran `ssh` itself, are ignored.

A click on a `FILE:LINE[:COL]` reference in a remote terminal visits
the file on that machine, relative to the terminal's directory there.
A terminal whose last OSC 7 named another host (which never changes
its directory, local or remote), or a local one with
`ssh` (or `mosh`, `telnet`, ...) in the foreground, refuses such
clicks with a message rather than open a local file of the same name.

XZile runs the system `ssh` with `BatchMode=yes`: it never asks for a
password or passphrase, and host keys are checked as your ssh
configuration says (an unknown host must first be accepted by running
`ssh host` in a terminal). Use keys, `ssh-agent` or `~/.ssh/config`.
One connection per machine is shared through a private control socket.
Each operation can be cancelled with `C-g`, and is abandoned after
`remote-timeout` seconds (30). The remote machine needs a POSIX shell
with `cat`, `cksum`, `ls` and `mktemp`.

---

## Reporting bugs

For **XZile-specific** bugs and feature requests, please open an issue or pull request at the project repository (or send an email to zeyi2@nekoarch.cc if you wish). If the issue is with upstream GNU Zile, use the [GNU Zile tracker](https://savannah.gnu.org/projects/zile/) or <bug-zile@gnu.org> as described in **README**.

---

## License and credits

Same license as GNU Zile. See **COPYING**.

Zile was written by Sandro Sigala, David A. Capello, and Reuben Thomas. The Lisp interpreter is based on code by Scott Lawrence. XZile was extended by Zeyi2 as a side project.
