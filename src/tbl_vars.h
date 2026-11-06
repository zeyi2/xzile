/* Zile variables

   Copyright (c) 1997-2020 Free Software Foundation, Inc.

   This file is part of GNU Zile.

   GNU Zile is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GNU Zile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.  */

/*
 * name, default_value, local_when_set, docstring
 */

X ("inhibit-splash-screen", "nil", false, "Non-nil inhibits the startup screen.\nIt also inhibits display of the initial message in the `*scratch*' buffer.")
X ("standard-indent", "4", false, "Default number of columns for margin-changing functions to indent.")
X ("tab-width", "8", true, "Distance between tab stops (for display of tab characters), in columns.")
X ("tab-always-indent", "t", false, "Controls the operation of the TAB key.\nIf t, hitting TAB always just indents the current line.\nIf nil, hitting TAB indents the current line if point is at the\nleft margin or in the line's indentation, otherwise it inserts a\n\"real\" TAB character.")
X ("indent-tabs-mode", "t", true, "If non-nil, insert-tab inserts \"real\" tabs; otherwise, it always inserts\nspaces.")
X ("fill-column", "70", true, "Column beyond which automatic line-wrapping should happen.\nAutomatically becomes buffer-local when set in any fashion.")
X ("auto-fill-mode", "nil", false, "If non-nil, Auto Fill Mode is automatically enabled.")
X ("kill-whole-line", "nil", false, "If non-nil, `kill-line' with no arg at beg of line kills the whole line.")
X ("case-fold-search", "t", true, "Non-nil means searches ignore case.")
X ("case-replace", "t", false, "Non-nil means `query-replace' should preserve case in replacements.")
X ("ring-bell", "t", false, "Non-nil means ring the terminal bell on any error.")
X ("highlight-nonselected-windows", "nil", false, "If non-nil, highlight region even in nonselected windows.")
X ("line-number-mode", "nil", true, "If non-nil, display line numbers in the left margin.")
X ("relative-line-number-mode", "nil", true, "If non-nil, display relative line numbers (distance from current line).")
X ("show-paren-mode", "t", false, "Non-nil means highlight the bracket matching the one at point.\nThis is the bracket after point if it opens, or the one before point if\nit closes; an unmatched or mismatched bracket is shown in a warning face.")
X ("select-enable-clipboard", "t", false, "Non-nil means kills and copies also go to the system clipboard, and\nyank takes text copied there by other programs.  Locally this uses\npbcopy and pbpaste (macOS), wl-copy and wl-paste (Wayland), or xclip\nor xsel (X11); otherwise, and always over SSH, copies are sent to the\nterminal with the OSC 52 escape sequence.")
X ("global-auto-revert-mode", "t", false, "Non-nil means buffers follow their files when these change on disk.\nA buffer without unsaved changes is reverted; a buffer with unsaved\nchanges is left alone, with a warning.  Saving a buffer whose file has\nchanged asks for confirmation regardless.")
X ("make-backup-files", "t", false, "Non-nil means make a backup of a file the first time it is saved.\nThis is done by appending `~' to the file name.")
X ("backup-directory", "nil", false, "The directory for backup files, which must exist.\nIf this variable is nil, the backup is made in the original file's\ndirectory.\nThis value is used only when `make-backup-files' is t.")
X ("remote-timeout", "30", false, "The number of seconds a remote file operation may take before it is\nabandoned.  The operation can also be cancelled with C-g while it runs.")
X ("ssh-config-file", "nil", false, "The OpenSSH client configuration file used for remote files, as with\nssh -F; its Host aliases are offered by M-l in the file picker.  If nil,\nssh's usual configuration is used, and aliases come from ~/.ssh/config.")
X ("gui-font", "nil", false, "The font of the graphical frontend, xzile-gui: a font file, or on\nLinux a fontconfig font name such as \"Fira Code\".  If nil, a good\nmonospace font is chosen from those installed.")
X ("gui-font-size", "14", false, "The font size of the graphical frontend, xzile-gui, in points.")
X ("preview-binary-files", "t", false, "Non-nil means xzile-gui shows an image or PDF file as a preview when\nit is visited. Previews are included in `make gui'.\nThe preview buffer has no text, and cannot be saved.  If nil, such\nfiles are visited as text.")
X ("html-preview-program", "xzile-html-preview", false, "The program `html-preview' runs to show an HTML file in a window of\nits own, found next to xzile or on PATH; it is built with `make gui',\nusing WebKitGTK on Linux and WKWebView on macOS.  If it cannot be\nfound, or this is nil, Linux opens the file with xdg-open; macOS\nreports an error, and never opens the browser.")
X ("html-preview-gpu", "t", false, "Non-nil means the HTML preview window may composite on the GPU.\nIf nil, it is drawn by the CPU, which can help with broken graphics\ndrivers.  Linux only: on macOS, WebKit decides how to draw.")
