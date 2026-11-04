
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

## Reporting bugs

For **XZile-specific** bugs and feature requests, please open an issue or pull request at the project repository (or send an email to zeyi2@nekoarch.cc if you wish). If the issue is with upstream GNU Zile, use the [GNU Zile tracker](https://savannah.gnu.org/projects/zile/) or <bug-zile@gnu.org> as described in **README**.

---

## License and credits

Same license as GNU Zile. See **COPYING**.

Zile was written by Sandro Sigala, David A. Capello, and Reuben Thomas. The Lisp interpreter is based on code by Scott Lawrence. XZile was extended by Zeyi2 as a side project.
