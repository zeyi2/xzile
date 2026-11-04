/* Terminal (shell) buffers

   Copyright (c) 2026 Zeyi2 <zeyi2@nekoarch.cc>

   This file is part of XZile.

   XZile is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   XZile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.  */

/* A terminal buffer runs a program on a pseudo-terminal and shows its
   screen through the emulator in vterm.c.

   In char mode, which is the default, the window shows the terminal
   and keys are sent to the program, except for C-x, which is a prefix
   for the usual C-x commands (C-x C-x sends C-x itself), and M-x.
   In line mode (C-x C-j), the buffer holds the scrollback and screen
   as ordinary read-only text; C-x C-k or q returns to char mode. */

#include "xzile.h"

#include <signal.h>
#include <sys/wait.h>

struct Shell {
	Buffer *bp;
	Process *proc;
	VTerm *vt;
	char *program;		/* For messages. */
	bool line_mode;
};

static Binding *char_map, *line_map;

bool
shell_char_mode (const Buffer *bp)
{
	return bp->shell != NULL && !bp->shell->line_mode;
}

bool
shell_process_alive (const Buffer *bp)
{
	return bp->shell != NULL && process_alive (bp->shell->proc);
}

static void
update_mode_label (Shell *sh)
{
	free (sh->bp->mode_label);
	sh->bp->mode_label = xasprintf ("Term: %s %s", sh->line_mode ? "line" : "char",
					process_alive (sh->proc) ? "run" : "exit");
}

void
shell_free (Buffer *bp)
{
	Shell *sh = bp->shell;
	if (sh == NULL)
		return;
	process_free (sh->proc);
	vterm_free (sh->vt);
	free (sh->program);
	free (sh);
	bp->shell = NULL;
}


/* Process I/O. */

static void
on_output (Process *p, const char *data, size_t len, void *owner)
{
	(void) p;
	Shell *sh = owner;
	vterm_write (sh->vt, data, len);
}

static void
vterm_reply (const char *s, size_t len, void *owner)
{
	Shell *sh = owner;
	process_send (sh->proc, s, len);
}

static int
hex_value (char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Track the working directory from OSC 7, "file://HOST/PATH". */
static void
vterm_osc (int code, const char *text, void *owner)
{
	Shell *sh = owner;
	if (code != 7 || !str_has_prefix (text, "file://"))
		return;
	const char *path = strchr (text + 7, '/');
	if (path == NULL)
		return;

	StrBuf sb;
	sb_init (&sb);
	for (const char *s = path; *s != '\0'; s++)
		if (*s == '%' && hex_value (s[1]) >= 0 && hex_value (s[2]) >= 0) {
			sb_addc (&sb, (char) (hex_value (s[1]) * 16 + hex_value (s[2])));
			s += 2;
		} else
			sb_addc (&sb, *s);
	if (file_is_dir (sb.s)) {
		free (sh->bp->dir);
		sh->bp->dir = sb_take (&sb);
	} else
		sb_free (&sb);
}

static void enter_line_mode (Shell *sh);

static void
on_process_exit (Process *p, int status, void *owner)
{
	(void) p;
	Shell *sh = owner;
	char *msg;
	if (WIFEXITED (status) && WEXITSTATUS (status) == 0)
		msg = xasprintf ("\r\nProcess %s finished\r\n", sh->program);
	else if (WIFEXITED (status))
		msg = xasprintf ("\r\nProcess %s exited abnormally with code %d\r\n", sh->program, WEXITSTATUS (status));
	else
		msg = xasprintf ("\r\nProcess %s killed by signal %d\r\n", sh->program,
				 WIFSIGNALED (status) ? WTERMSIG (status) : 0);
	vterm_write (sh->vt, msg, strlen (msg));
	free (msg);
	enter_line_mode (sh);
}


/* Display. */

/* The window whose size the terminal follows. */
static bool
is_primary_window (Window *wp)
{
	if (wp == cur_wp)
		return true;
	if (cur_wp->bp == wp->bp)
		return false;
	for (Window *w = head_wp; w != NULL; w = w->next)
		if (w->bp == wp->bp)
			return w == wp;
	return false;
}

static void
put_vcell (const VCell *c)
{
	VAttr a = c->attr;
	term_set_vattr (&a);
	if (c->cp == ' ' || c->cp == 0 || (a.flags & VATTR_INVISIBLE))
		term_addch (' ');
	else {
		char buf[4];
		size_t n = utf8_encode (c->cp, buf);
		term_addnstr (buf, n);
	}
}

/* Draw the terminal into the text area of `wp'; returns false if the
   window does not show a terminal in char mode. */
bool
shell_draw (Window *wp, size_t topline, size_t leftcol)
{
	if (!shell_char_mode (wp->bp))
		return false;
	Shell *sh = wp->bp->shell;

	int rows = (int) MAX (wp->eheight, 1), cols = (int) MAX (wp->ewidth, 1);
	if (is_primary_window (wp) && (rows != vterm_rows (sh->vt) || cols != vterm_cols (sh->vt))) {
		vterm_resize (sh->vt, rows, cols);
		process_resize (sh->proc, rows, cols);
	}

	int vrows = vterm_rows (sh->vt), vcols = vterm_cols (sh->vt);
	for (size_t y = 0; y < wp->eheight; y++) {
		term_move (topline + y, leftcol);
		size_t x = 0;
		if ((int) y < vrows)
			for (; x < wp->ewidth && (int) x < vcols; x++) {
				const VCell *c = vterm_cell (sh->vt, (int) y, (int) x);
				if (c->width == 0)
					continue;
				if (c->width == 2 && x + 1 >= wp->ewidth) {
					term_attrset (FONT_NORMAL);
					term_addch (' ');
					continue;
				}
				put_vcell (c);
				if (c->width == 2)
					x++;
			}
		term_attrset (FONT_NORMAL);
		for (; x < wp->ewidth; x++)
			term_addch (' ');
	}
	term_attrset (FONT_NORMAL);
	return true;
}

/* The terminal cursor, relative to the window; false if not shown. */
bool
shell_cursor (Window *wp, size_t *y, size_t *x)
{
	if (!shell_char_mode (wp->bp))
		return false;
	int cy, cx;
	vterm_cursor (wp->bp->shell->vt, &cy, &cx);
	*y = (size_t) cy;
	*x = (size_t) cx;
	return vterm_cursor_visible (wp->bp->shell->vt) && process_alive (wp->bp->shell->proc);
}


/* Modes. */

static void
add_cells (StrBuf *sb, const VCell *cells, size_t n)
{
	/* Drop trailing blanks. */
	while (n > 0 && (cells[n - 1].cp == ' ' || cells[n - 1].cp == 0))
		n--;
	for (size_t i = 0; i < n; i++) {
		if (cells[i].width == 0)
			continue;
		char buf[4];
		size_t len = utf8_encode (cells[i].cp == 0 ? ' ' : cells[i].cp, buf);
		sb_addn (sb, buf, len);
	}
}

static void
enter_line_mode (Shell *sh)
{
	Buffer *bp = sh->bp;
	VTerm *vt = sh->vt;
	StrBuf sb;
	sb_init (&sb);

	size_t nsb = vterm_scrollback_len (vt);
	for (size_t i = 0; i < nsb; i++) {
		size_t len;
		const VCell *cells = vterm_scrollback_line (vt, i, &len);
		add_cells (&sb, cells, len);
		sb_addc (&sb, '\n');
	}

	/* The screen, without blank lines below the cursor. */
	int cy, cx, rows = vterm_rows (vt), cols = vterm_cols (vt);
	vterm_cursor (vt, &cy, &cx);
	int last = cy;
	for (int y = rows - 1; y > cy; y--) {
		StrBuf line;
		sb_init (&line);
		add_cells (&line, vterm_cell (vt, y, 0), (size_t) cols);
		bool blank = line.len == 0;
		sb_free (&line);
		if (!blank) {
			last = y;
			break;
		}
	}
	size_t cursor_line_o = 0;
	for (int y = 0; y <= last; y++) {
		if (y == cy)
			cursor_line_o = sb.len;
		add_cells (&sb, vterm_cell (vt, y, 0), (size_t) cols);
		if (y < last)
			sb_addc (&sb, '\n');
	}

	bp->readonly = false;
	buffer_goto_offset (bp, 0);
	buffer_replace_estr (bp, buffer_length (bp), ev_of (sb.s, sb.len, eol_lf));
	bp->readonly = true;
	bp->modified = false;

	/* Put point where the terminal cursor is. */
	buffer_goto_offset (bp, cursor_line_o);
	buffer_goto_offset (bp, MIN (cursor_line_o + (size_t) cx, buffer_end_of_line (bp, cursor_line_o)));
	sb_free (&sb);

	sh->line_mode = true;
	bp->keymap = line_map;
	update_mode_label (sh);
	thisflag |= FLAG_NEED_RESYNC;
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (wp->bp == bp) {
			if (wp == cur_wp)
				recenter (wp);
			else if (wp->saved_pt != NULL)
				wp->saved_pt->o = buffer_pt (bp);
		}
}

static void
enter_char_mode (Shell *sh)
{
	Buffer *bp = sh->bp;
	bp->readonly = false;
	buffer_goto_offset (bp, 0);
	buffer_replace_estr (bp, buffer_length (bp), ev_empty ());
	bp->readonly = true;
	bp->modified = false;
	sh->line_mode = false;
	bp->keymap = char_map;
	update_mode_label (sh);
}

static bool
check_shell (void)
{
	if (cur_bp->shell == NULL) {
		minibuf_error ("Buffer is not a terminal");
		return false;
	}
	return true;
}

static bool
F_term_line_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_shell ())
		return false;
	if (!cur_bp->shell->line_mode)
		enter_line_mode (cur_bp->shell);
	return true;
}

static bool
F_term_char_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_shell ())
		return false;
	if (!process_alive (cur_bp->shell->proc)) {
		minibuf_error ("Process %s is not running", cur_bp->shell->program);
		return false;
	}
	if (cur_bp->shell->line_mode)
		enter_char_mode (cur_bp->shell);
	return true;
}


/* Sending input. */

static void
send (const char *s, size_t len)
{
	Shell *sh = cur_bp->shell;
	if (sh == NULL || !process_alive (sh->proc)) {
		ding ();
		return;
	}
	process_send (sh->proc, s, len);
}

/* The bytes an xterm sends for `key'. */
static size_t
key_to_bytes (Keystroke key, bool app_cursor, char *out)
{
	size_t n = 0;
	bool meta = (key & KBD_META) != 0, ctrl = (key & KBD_CTRL) != 0;
	Keystroke base = key & ~(KBD_META | KBD_CTRL);
	int mods = 1 + (meta ? 2 : 0) + (ctrl ? 4 : 0);

	char final = 0;
	int tilde = 0;
	switch (base) {
	case KBD_UP: final = 'A'; break;
	case KBD_DOWN: final = 'B'; break;
	case KBD_RIGHT: final = 'C'; break;
	case KBD_LEFT: final = 'D'; break;
	case KBD_HOME: final = 'H'; break;
	case KBD_END: final = 'F'; break;
	case KBD_F1: final = 'P'; break;
	case KBD_F2: final = 'Q'; break;
	case KBD_F3: final = 'R'; break;
	case KBD_F4: final = 'S'; break;
	case KBD_INS: tilde = 2; break;
	case KBD_DEL: tilde = 3; break;
	case KBD_PGUP: tilde = 5; break;
	case KBD_PGDN: tilde = 6; break;
	case KBD_F5: tilde = 15; break;
	case KBD_F6: tilde = 17; break;
	case KBD_F7: tilde = 18; break;
	case KBD_F8: tilde = 19; break;
	case KBD_F9: tilde = 20; break;
	case KBD_F10: tilde = 21; break;
	case KBD_F11: tilde = 23; break;
	case KBD_F12: tilde = 24; break;
	default: break;
	}

	if (final != 0) {
		bool ss3 = base >= KBD_F1 && base <= KBD_F4;
		if (mods > 1)
			return (size_t) snprintf (out, 32, "\033[1;%d%c", mods, final);
		if (ss3 || app_cursor)
			return (size_t) snprintf (out, 32, "\033O%c", final);
		return (size_t) snprintf (out, 32, "\033[%c", final);
	}
	if (tilde != 0)
		return mods > 1 ? (size_t) snprintf (out, 32, "\033[%d;%d~", tilde, mods)
			: (size_t) snprintf (out, 32, "\033[%d~", tilde);

	if (meta)
		out[n++] = 033;
	switch (base) {
	case KBD_TAB:
		out[n++] = '\t';
		break;
	case KBD_RET:
		out[n++] = '\r';
		break;
	case KBD_BS:
		out[n++] = ctrl ? '\b' : 0177;
		break;
	case KBD_ESCAPE:
		out[n++] = 033;
		break;
	default:
		if (base > 0xff)
			return 0;
		if (ctrl) {
			if (base >= 'a' && base <= 'z')
				out[n++] = (char) (base - 'a' + 1);
			else if (base == '@' || base == ' ')
				out[n++] = 0;
			else if (base >= '[' && base <= '_')
				out[n++] = (char) (base - '@');
			else if (base == '?')
				out[n++] = 0177;
			else
				out[n++] = (char) base;
		} else
			out[n++] = (char) base;
		break;
	}
	return n;
}

static bool
F_term_send_raw (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_shell ())
		return false;
	char buf[32];
	size_t n = key_to_bytes (lastkey (), vterm_app_cursor (cur_bp->shell->vt), buf);
	if (n > 0)
		send (buf, n);
	return true;
}

static bool
F_term_send_C_x (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_shell ())
		return false;
	send ("\030", 1);
	return true;
}

/* Send text as if pasted into the terminal. */
void
shell_send_paste (const char *text)
{
	Shell *sh = cur_bp->shell;
	if (sh == NULL)
		return;
	bool bracketed = vterm_bracketed_paste (sh->vt);
	StrBuf sb;
	sb_init (&sb);
	if (bracketed)
		sb_add (&sb, "\033[200~");
	for (const char *s = text; *s != '\0'; s++)
		if (*s == '\r' && s[1] == '\n')
			continue;
		else
			sb_addc (&sb, *s == '\n' ? '\r' : *s);
	if (bracketed)
		sb_add (&sb, "\033[201~");
	send (sb.s, sb.len);
	sb_free (&sb);
}

static bool
F_term_paste (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_shell ())
		return false;
	kill_ring_take_clipboard ();
	if (kill_ring_text == NULL) {
		minibuf_error ("Kill ring is empty");
		return false;
	}
	shell_send_paste (kill_ring_text->text);
	return true;
}

static bool
F_term_interrupt (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_shell () || !process_alive (cur_bp->shell->proc))
		return false;
	process_signal (cur_bp->shell->proc, SIGINT);
	return true;
}


/* Starting terminals. */

static void
init_maps (void)
{
	if (char_map != NULL)
		return;
	char_map = keymap_new ();
	keymap_set_default (char_map, "term-send-raw");
	keymap_bind (char_map, "\\C-x\\C-x", "term-send-C-x");
	keymap_bind (char_map, "\\C-x\\C-j", "term-line-mode");
	keymap_bind (char_map, "\\C-x\\C-k", "term-char-mode");
	keymap_bind (char_map, "\\C-x\\C-y", "term-paste");
	keymap_bind (char_map, "\\M-x", "execute-extended-command");

	line_map = keymap_new ();
	keymap_bind (line_map, "\\C-x\\C-k", "term-char-mode");
	keymap_bind (line_map, "q", "term-char-mode");
	keymap_bind (line_map, "\\C-c\\C-c", "term-interrupt");
}

/* Start `argv' in a new terminal buffer called `name'. */
static bool
start_terminal (const char *name, const char *const *argv, const char *exec_path, const char *program)
{
	init_maps ();

	int rows = (int) MAX (cur_wp->eheight, 1), cols = (int) MAX (cur_wp->ewidth, 1);
	Shell *sh = xzalloc (sizeof (Shell));
	sh->program = xstrdup (program);
	sh->vt = vterm_new (rows, cols, vterm_reply, vterm_osc, sh);

	static const char *const env[] = {
		"TERM=xterm-256color",
		"COLORTERM=truecolor",
		"INSIDE_XZILE=1",
		NULL
	};
	sh->proc = process_start (argv, exec_path, cur_bp->dir, true, rows, cols, env,
				  on_output, on_process_exit, sh);
	if (sh->proc == NULL) {
		minibuf_error ("Cannot start %s", program);
		vterm_free (sh->vt);
		free (sh->program);
		free (sh);
		return false;
	}

	char *dir = xstrdup (cur_bp->dir);
	Buffer *bp = buffer_new (NULL);
	bp->name = buffer_unique_name (name);
	free (bp->dir);
	bp->dir = dir;
	bp->nosave = true;
	bp->noundo = true;
	bp->readonly = true;
	bp->shell = sh;
	bp->keymap = char_map;
	sh->bp = bp;
	update_mode_label (sh);
	buffer_switch_to (bp, true);
	return true;
}

static char *
user_shell (void)
{
	const char *shell = getenv ("SHELL");
	return xstrdup (shell != NULL && shell[0] != '\0' ? shell : "/bin/sh");
}

static bool
F_shell (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	/* Reuse a running shell. */
	Buffer *bp = buffer_find ("*shell*");
	if (bp != NULL && shell_process_alive (bp)) {
		buffer_switch_to (bp, true);
		return true;
	}

	/* Run a login shell, as terminal emulators do. */
	char *shell = user_shell ();
	char *base = path_basename (shell);
	char *argv0 = xstrcat3 ("-", base, "");
	const char *argv[] = { argv0, NULL };
	bool ok = start_terminal ("*shell*", argv, shell, base);
	free (argv0);
	free (base);
	free (shell);
	return ok;
}

static bool
F_term (long uniarg, Args *args)
{
	(void) uniarg;
	char *program = xstrdup_or_null (args_poll (args));
	if (program == NULL) {
		char *shell = user_shell ();
		program = minibuf_read ("Run program: ", shell);
		free (shell);
		if (program == NULL)
			return funcall ("keyboard-quit");
	}
	if (program[0] == '\0') {
		free (program);
		return false;
	}

	char *cmd = xstrcat3 ("exec ", program, "");
	const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
	char *first = xstrndup (program, strcspn (program, " \t"));
	char *base = path_basename (first);
	bool ok = start_terminal ("*terminal*", argv, NULL, base);
	free (base);
	free (first);
	free (cmd);
	free (program);
	return ok;
}

void
terminal_init (void)
{
	lisp_func_new ("shell", F_shell, true,
		       "Run your login shell ($SHELL) in a terminal buffer named *shell*.\n"
		       "If a shell is already running there, switch to it.\n"
		       "\n"
		       "Keys go to the shell, except for C-x, which is a prefix for the usual\n"
		       "C-x commands, and M-x. C-x C-x sends C-x, C-x C-y pastes the last\n"
		       "killed text, and C-x C-j switches to line mode, where the scrollback\n"
		       "is ordinary text; C-x C-k or q returns to char mode.");
	lisp_func_new ("term", F_term, true,
		       "Run PROGRAM in a terminal buffer named *terminal*.\n"
		       "See `shell' for the keys.");
	lisp_func_new ("term-send-raw", F_term_send_raw, false,
		       "Send the last key typed to the terminal's program.");
	lisp_func_new ("term-send-C-x", F_term_send_C_x, true,
		       "Send C-x to the terminal's program.");
	lisp_func_new ("term-paste", F_term_paste, true,
		       "Send the last killed text to the terminal, as if pasted.");
	lisp_func_new ("term-line-mode", F_term_line_mode, true,
		       "Show the terminal's scrollback and screen as text, for moving around,\n"
		       "searching and copying. C-x C-k or q returns to char mode.");
	lisp_func_new ("term-char-mode", F_term_char_mode, true,
		       "Show the terminal and send keys to its program.");
	lisp_func_new ("term-interrupt", F_term_interrupt, true,
		       "Interrupt the terminal's program.");
}
