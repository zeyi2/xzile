
# XZile

<p align="center">
  <img src="pic/peace.jpg" alt="XZile" />
</p>

<p align="center"><em>The soul is the witness.</em></p>

XZile is a fork of [GNU Zile](https://www.gnu.org/s/zile/) with additional features. It remains a small, fast, lightweight Emacs clone for the terminal.

The file **README** in this directory contains the original GNU Zile readme.

---

## Build and install

Same as GNU Zile. From a release tarball:

```bash
./configure
make
make install
```

From a git checkout:

```bash
./bootstrap
./configure
make
make install
```

You need **valac** 0.56 or later, **glib 2.0**, and **libgee**. The installed binary is **xzile**, and the user init file is **~/.xzile** (see `doc/man-extras` or `man xzile` for details).

---

## Reporting bugs

For **XZile-specific** bugs and feature requests, please open an issue or pull request at the project repository (or send an email to zeyi2@nekoarch.cc if you wish). If the issue is with upstream GNU Zile, use the [GNU Zile tracker](https://savannah.gnu.org/projects/zile/) or <bug-zile@gnu.org> as described in **README**.

---

## License and credits

Same license as GNU Zile. See **COPYING**.

Zile was written by Sandro Sigala, David A. Capello, and Reuben Thomas. The Lisp interpreter is based on code by Scott Lawrence. XZile was extended by Zeyi2 as a side project.
