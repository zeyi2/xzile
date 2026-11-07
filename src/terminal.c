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
   Mouse events go to a program that has asked for them; otherwise a
   click on a FILE:LINE[:COL] reference visits it (see follow_link in
   window.c), and a drag selects text on the screen, copied on release.
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
	char *remote_dir;	/* On another machine (bp->remote), the
				   directory there; bp->dir stays local. */
	char *foreign_host;	/* The host named by the last OSC 7, if it
				   is not the terminal's machine; or NULL. */
	bool line_mode;
	/* The cells selected with the mouse, from where the drag started
	   to where it is, both included. Rows count from the first line
	   ever shown (see vterm_scrolled), so that the selection moves up
	   with its text. */
	bool selecting, selected;
	size_t sel_row[2];
	int sel_col[2];
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
	free (sh->remote_dir);
	free (sh->foreign_host);
	free (sh);
	bp->shell = NULL;
}

/* The remote directory of a terminal on another machine, or NULL. */
const char *
shell_directory (const Buffer *bp)
{
	return bp->shell != NULL && bp->remote != NULL ? bp->shell->remote_dir : NULL;
}

/* Programs that log in to another machine. */
static const char *const login_programs[] = { "ssh", "slogin", "mosh-client", "telnet", "rlogin", "rsh" };

/* If the program of terminal `bp' may be on another machine than the
   terminal's, where it is, as "on HOST" or "running ssh"; else NULL.
   It is on the host the last OSC 7 named; on this machine, the program
   in the foreground may also log in elsewhere. */
char *
shell_elsewhere (const Buffer *bp)
{
	Shell *sh = bp->shell;
	if (sh == NULL)
		return NULL;
	if (sh->foreign_host != NULL)
		return xasprintf ("on %s", sh->foreign_host);
	if (bp->remote != NULL)
		return NULL;
	char *name = process_foreground (sh->proc), *where = NULL;
	for (size_t i = 0; name != NULL && where == NULL && i < sizeof login_programs / sizeof login_programs[0]; i++)
		if (strcmp (name, login_programs[i]) == 0)
			where = xasprintf ("running %s", name);
	free (name);
	return where;
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

/* Track the working directory from OSC 7, "file://HOST/PATH", only
   taken if HOST is the terminal's machine (a shell that ssh'ed
   elsewhere reports another). On a remote terminal, the path is on its
   machine. */
static void
vterm_osc (int code, const char *text, void *owner)
{
	Shell *sh = owner;
	if (code != 7 || !str_has_prefix (text, "file://"))
		return;
	const char *path = strchr (text + 7, '/');
	if (path == NULL)
		return;
	char *host = xstrndup (text + 7, (size_t) (path - (text + 7)));
	bool same = sh->bp->remote != NULL ? remote_same_host (sh->bp->remote, host) : remote_host_is_local (host);
	free (sh->foreign_host);
	sh->foreign_host = same ? NULL : xstrdup (host);
	if (!same) {
		debug_log ("remote", "%s: ignored OSC 7 for host %s", sh->bp->name, host);
		free (host);
		return;
	}
	free (host);

	StrBuf sb;
	sb_init (&sb);
	for (const char *s = path; *s != '\0'; s++)
		if (*s == '%' && hex_value (s[1]) >= 0 && hex_value (s[2]) >= 0) {
			sb_addc (&sb, (char) (hex_value (s[1]) * 16 + hex_value (s[2])));
			s += 2;
		} else
			sb_addc (&sb, *s);
	if (sh->bp->remote != NULL) {
		/* A remote name: it cannot be checked here. */
		free (sh->remote_dir);
		sh->remote_dir = sb_take (&sb);
		debug_log ("remote", "%s: directory %s", sh->bp->name, sh->remote_dir);
	} else if (file_is_dir (sb.s)) {
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
	else if (WIFEXITED (status) && WEXITSTATUS (status) == 255 && sh->bp->remote != NULL)
		msg = xasprintf ("\r\nProcess %s exited with code 255: ssh failed, or the connection was lost\r\n",
				 sh->program);
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

/* Row `y' of the terminal `bp' as shown, as UTF-8 in `sb'. The byte
   of the character shown at column `x' is `at[x]'; `at' has a column
   more than the terminal, for the end of the row. */
static void
row_text (Buffer *bp, int y, StrBuf *sb, size_t *at)
{
	VTerm *vt = bp->shell->vt;
	int cols = vterm_cols (vt);
	const VCell *cells = vterm_cell (vt, y, 0);
	for (int c = 0; c < cols; c++) {
		const VCell *cell = &cells[c];
		/* The right half of a wide character is the character. */
		at[c] = cell->width == 0 && c > 0 ? at[c - 1] : sb->len;
		if (cell->width == 0)
			continue;
		uint32_t cp = cell->cp == 0 || (cell->attr.flags & VATTR_INVISIBLE) ? ' ' : cell->cp;
		char buf[4];
		sb_addn (sb, buf, utf8_encode (cp, buf));
	}
	at[cols] = sb->len;
}

/* Set `link_col[x]' for each column `x' of row `y' of the terminal `bp'
   that shows a reference a click would follow. */
static void
row_links (Buffer *bp, int y, size_t *at, bool *link_col)
{
	StrBuf sb;
	sb_init (&sb);
	row_text (bp, y, &sb, at);
	LinkSpan *spans;
	size_t n = reference_links (bp, sb.s, sb.len, NOPOS, &spans), k = 0;
	for (int x = 0; x < vterm_cols (bp->shell->vt); x++) {
		while (k < n && spans[k].end <= at[x])
			k++;
		link_col[x] = k < n && at[x] >= spans[k].start;
	}
	free (spans);
	sb_free (&sb);
}

/* The selection, in reading order: from row `*r0', column `*c0' to
   row `*r1', column `*c1'. */
static void
selection_bounds (const Shell *sh, size_t *r0, int *c0, size_t *r1, int *c1)
{
	int first = sh->sel_row[0] < sh->sel_row[1] ||
		(sh->sel_row[0] == sh->sel_row[1] && sh->sel_col[0] <= sh->sel_col[1]) ? 0 : 1;
	*r0 = sh->sel_row[first];
	*c0 = sh->sel_col[first];
	*r1 = sh->sel_row[1 - first];
	*c1 = sh->sel_col[1 - first];
}

/* Whether column `x' of screen row `y' is selected. */
static bool
cell_selected (const Shell *sh, int y, int x)
{
	if (!sh->selected)
		return false;
	size_t r0, r1, r = vterm_scrolled (sh->vt) + (size_t) y;
	int c0, c1;
	selection_bounds (sh, &r0, &c0, &r1, &c1);
	return r >= r0 && r <= r1 && (r > r0 || x >= c0) && (r < r1 || x <= c1);
}

static void
clear_selection (Shell *sh)
{
	sh->selecting = sh->selected = false;
}

static bool tracks_mouse (const Window *wp);
static void
put_vcell (const VCell *c, bool link, bool selected)
{
	VAttr a = c->attr;
	term_set_vattr (&a);
	if (selected)
		term_apply_face (FACE_REGION);
	else if (link)
		term_mark_link (a.fg == VCOLOR_DEFAULT && !(a.flags & VATTR_REVERSE));
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
		clear_selection (sh);
	}

	int vrows = vterm_rows (sh->vt), vcols = vterm_cols (sh->vt);
	/* References are shown as links, unless the program reads the mouse. */
	bool links = !tracks_mouse (wp) && references_followable (wp->bp);
	bool *link_col = xzalloc ((size_t) vcols * sizeof *link_col);
	size_t *at = xmalloc (((size_t) vcols + 1) * sizeof *at);
	for (size_t y = 0; y < wp->eheight; y++) {
		term_move (topline + y, leftcol);
		size_t x = 0;
		if (links && (int) y < vrows)
			row_links (wp->bp, (int) y, at, link_col);
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
				bool selected = cell_selected (sh, (int) y, (int) x) ||
					(c->width == 2 && cell_selected (sh, (int) y, (int) x + 1));
				put_vcell (c, links && link_col[x], selected);
				if (c->width == 2)
					x++;
			}
		term_attrset (FONT_NORMAL);
		for (; x < wp->ewidth; x++)
			term_addch (' ');
	}
	free (at);
	free (link_col);
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
			if (wp == cur_wp) {
				recenter (wp);
				wp->lastpointn = buffer_offset_to_line (bp, buffer_pt (bp));
			} else if (wp->saved_pt != NULL)
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
	clear_selection (sh);
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
	clear_selection (sh);
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

/* The mouse. */

/* Whether the program in `wp' reads the mouse. */
static bool
tracks_mouse (const Window *wp)
{
	return shell_char_mode (wp->bp) && process_alive (wp->bp->shell->proc) &&
		vterm_mouse_tracking (wp->bp->shell->vt);
}

/* The window whose text area has screen cell `*y', `*x', which become
   relative to it; NULL if there is none. */
static Window *
text_window_at (size_t *y, size_t *x)
{
	WinNode *leaf = layout_leaf_at (*y, *x);
	if (leaf == NULL || *y - leaf->y >= leaf->wp->eheight || *x - leaf->x >= leaf->wp->ewidth)
		return NULL;
	*y -= leaf->y;
	*x -= leaf->x;
	return leaf->wp;
}

bool
shell_mouse_at (size_t y, size_t x)
{
	Window *wp = text_window_at (&y, &x);
	return wp != NULL && tracks_mouse (wp) && !term_minibuf_active ();
}

/* A press or the wheel goes to the window under the pointer (a press
   makes it current); a drag or release goes to the current window,
   where the press was. */
bool
shell_mouse (Keystroke key)
{
	size_t y, x;
	if (!term_mouse_position (&y, &x))
		return false;

	int button;
	enum VMouseAction action;
	switch (key) {
	case KBD_MOUSE_1: button = 0; action = VMOUSE_PRESS; break;
	case KBD_MOUSE_2: button = 1; action = VMOUSE_PRESS; break;
	case KBD_MOUSE_3: button = 2; action = VMOUSE_PRESS; break;
	case KBD_WHEEL_UP: button = 64; action = VMOUSE_PRESS; break;
	case KBD_WHEEL_DOWN: button = 65; action = VMOUSE_PRESS; break;
	case KBD_UP_MOUSE_1: button = 0; action = VMOUSE_RELEASE; break;
	case KBD_UP_MOUSE_2: button = 1; action = VMOUSE_RELEASE; break;
	case KBD_UP_MOUSE_3: button = 2; action = VMOUSE_RELEASE; break;
	case KBD_DRAG_MOUSE_1: button = 0; action = VMOUSE_DRAG; break;
	case KBD_DRAG_MOUSE_2: button = 1; action = VMOUSE_DRAG; break;
	case KBD_DRAG_MOUSE_3: button = 2; action = VMOUSE_DRAG; break;
	default: return false;
	}

	Window *wp;
	if (action == VMOUSE_PRESS)
		wp = text_window_at (&y, &x);
	else {
		wp = cur_wp;
		WinNode *leaf = find_leaf_for (wp);
		y = y > leaf->y ? y - leaf->y : 0;
		x = x > leaf->x ? x - leaf->x : 0;
	}
	if (wp == NULL || !tracks_mouse (wp))
		return false;
	if (wp != cur_wp && button < 64)
		window_set_current (wp);
	vterm_mouse (wp->bp->shell->vt, button, action, (int) MIN (y, 0xffff), (int) MIN (x, 0xffff));
	return true;
}

/* Follow the reference to a file, if any, shown at row `y', column `x'
   of the terminal screen in `wp'. */
bool
shell_follow_link (Window *wp, size_t y, size_t x)
{
	VTerm *vt = wp->bp->shell->vt;
	int cols = vterm_cols (vt);
	if ((int) y >= vterm_rows (vt) || (int) x >= cols || x >= wp->ewidth)
		return false;

	StrBuf sb;
	sb_init (&sb);
	size_t *at = xmalloc (((size_t) cols + 1) * sizeof *at);
	row_text (wp->bp, (int) y, &sb, at);
	bool ok = follow_reference (wp, sb.s != NULL ? sb.s : "", sb.len, at[x], NOPOS);
	free (at);
	sb_free (&sb);
	return ok;
}

bool
shell_link_at (Window *wp, size_t y, size_t x, size_t *x0, size_t *x1, char **target)
{
	if (!shell_char_mode (wp->bp) || tracks_mouse (wp))
		return false;
	VTerm *vt = wp->bp->shell->vt;
	int cols = vterm_cols (vt);
	if ((int) y >= vterm_rows (vt) || (int) x >= cols || x >= wp->ewidth ||
	    !references_followable (wp->bp))
		return false;

	StrBuf sb;
	sb_init (&sb);
	size_t *at = xmalloc (((size_t) cols + 1) * sizeof *at);
	row_text (wp->bp, (int) y, &sb, at);
	LinkSpan span;
	bool ok = reference_link_at (wp->bp, sb.s, sb.len, at[x], NOPOS, &span, target);
	if (ok) {
		size_t c = x;
		while (c > 0 && at[c - 1] >= span.start)
			c--;
		*x0 = c;
		for (c = x + 1; (int) c < cols && at[c] < span.end; c++)
			;
		*x1 = MIN (c, wp->ewidth);
	}
	free (at);
	sb_free (&sb);
	return ok;
}

/* Set end `end' of the selection in `wp' to row `y', column `x' of
   its screen, kept on the screen. */
static void
set_selection_end (Window *wp, int end, size_t y, size_t x)
{
	Shell *sh = wp->bp->shell;
	int rows = vterm_rows (sh->vt), cols = vterm_cols (sh->vt);
	int row = (int) MIN (y, (size_t) rows - 1), col = (int) MIN (x, (size_t) cols - 1);
	/* The right half of a wide character is the character. */
	if (col > 0 && vterm_cell (sh->vt, row, col)->width == 0)
		col--;
	sh->sel_row[end] = vterm_scrolled (sh->vt) + (size_t) row;
	sh->sel_col[end] = col;
}

void
shell_select_start (Window *wp, size_t y, size_t x)
{
	if (!shell_char_mode (wp->bp))
		return;
	Shell *sh = wp->bp->shell;
	clear_selection (sh);
	sh->selecting = true;
	set_selection_end (wp, 0, y, x);
	set_selection_end (wp, 1, y, x);
}

void
shell_select_extend (Window *wp, size_t y, size_t x)
{
	if (!shell_char_mode (wp->bp) || !wp->bp->shell->selecting)
		return;
	Shell *sh = wp->bp->shell;
	set_selection_end (wp, 1, y, x);
	sh->selected = true;
}

/* Put the selected text of the current terminal in the kill ring, and
   so on the clipboard; false if nothing is selected. Rows that have
   scrolled off the screen are left out. */
static bool
copy_selection (void)
{
	Shell *sh = cur_bp->shell;
	if (sh == NULL || sh->line_mode || !sh->selected)
		return false;
	size_t r0, r1, top = vterm_scrolled (sh->vt);
	int c0, c1, rows = vterm_rows (sh->vt), cols = vterm_cols (sh->vt);
	selection_bounds (sh, &r0, &c0, &r1, &c1);
	StrBuf sb;
	sb_init (&sb);
	for (size_t r = MAX (r0, top); r <= r1 && r < top + (size_t) rows; r++) {
		int from = r == r0 ? c0 : 0, to = r == r1 ? c1 : cols - 1;
		if (r > MAX (r0, top))
			sb_addc (&sb, '\n');
		const VCell *cells = vterm_cell (sh->vt, (int) (r - top), 0);
		/* A wide character at the end is taken whole. */
		if (to + 1 < cols && cells[to].width == 2)
			to++;
		add_cells (&sb, cells + from, (size_t) (to - from + 1));
	}
	estr_free (kill_ring_text);
	kill_ring_text = NULL;
	kill_ring_push (ev_of (sb.s != NULL ? sb.s : "", sb.len, eol_lf));
	sb_free (&sb);
	return true;
}

static bool
F_term_mouse_release (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (cur_bp->shell == NULL)
		return false;
	cur_bp->shell->selecting = false;
	copy_selection ();
	return true;
}

static bool
F_term_copy (long uniarg, Args *args)
{
	if (!check_shell ())
		return false;
	if (copy_selection ()) {
		clear_selection (cur_bp->shell);
		return true;
	}
	return F_term_send_raw (uniarg, args);
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
	/* Signalling ssh would end the session: the remote terminal
	   interrupts its program instead, as for a typed C-c. */
	if (cur_bp->remote != NULL)
		process_send (cur_bp->shell->proc, "\003", 1);
	else
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
	keymap_bind (char_map, "\\M-w", "term-copy");
	keymap_bind (char_map, "\\UPMOUSE1", "term-mouse-release");
	keymap_bind (char_map, "\\M-x", "execute-extended-command");

	line_map = keymap_new ();
	keymap_bind (line_map, "\\C-x\\C-k", "term-char-mode");
	keymap_bind (line_map, "q", "term-char-mode");
	keymap_bind (line_map, "\\C-c\\C-c", "term-interrupt");
}

/* Start `argv' in the terminal buffer `bp', whose program has exited,
   or if `bp' is NULL in a new buffer called `name' (made unique). On
   another machine, `r' is it and `rdir' the directory there. */
static bool
start_terminal (Buffer *bp, const char *name, const char *const *argv, const char *exec_path, const char *program,
		Remote *r, const char *rdir)
{
	init_maps ();

	int rows = (int) MAX (cur_wp->eheight, 1), cols = (int) MAX (cur_wp->ewidth, 1);
	Shell *sh = xzalloc (sizeof (Shell));
	sh->program = xstrdup (program);
	sh->remote_dir = xstrdup_or_null (rdir);
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
		free (sh->remote_dir);
		free (sh);
		return false;
	}

	/* Copy first: `bp' may be the current buffer. */
	char *dir = xstrdup (cur_bp->dir);
	if (bp == NULL) {
		bp = buffer_new (NULL);
		bp->name = buffer_unique_name (name);
		bp->nosave = true;
		bp->noundo = true;
	} else
		shell_free (bp);
	free (bp->dir);
	bp->dir = dir;
	bp->remote = r;
	bp->shell = sh;
	sh->bp = bp;
	enter_char_mode (sh);
	buffer_switch_to (bp, true);
	return true;
}

static char *
user_shell (void)
{
	const char *shell = getenv ("SHELL");
	return xstrdup (shell != NULL && shell[0] != '\0' ? shell : "/bin/sh");
}

/* The name of terminal buffers called `base' for `r': on another
   machine, its label is added, so that each machine has its own. */
static char *
terminal_name (const char *base, Remote *r)
{
	return r != NULL ? xasprintf ("*%s ssh:%s*", base, remote_label (r)) : xasprintf ("*%s*", base);
}

/* Check that a terminal can be started on `r' in the directory of the
   current buffer, there: its absolute name goes to `*dir', and the
   remote user's $SHELL to `*shell'. Nothing local is used instead. */
static bool
check_remote_terminal (Remote *r, char **dir, char **shell)
{
	char *start = remote_buffer_dir (cur_bp);
	bool ok = remote_terminal_check (r, start, dir, shell);
	free (start);
	if (!ok)
		minibuf_error ("%s", remote_last_error ());
	return ok;
}

/* Start `program' (NULL for the login shell `shell') in `dir' on `r',
   as start_terminal does. */
static bool
start_remote_terminal (Buffer *bp, const char *name, Remote *r, const char *dir, const char *program,
		       const char *shell)
{
	StrVec keep = { NULL, 0, 0 };
	const char **argv = remote_terminal_argv (r, dir, program, &keep);
	char *first = xstrndup (program != NULL ? program : shell, program != NULL ? strcspn (program, " \t")
				: strlen (shell));
	char *base = path_basename (first);
	char *shown = xasprintf ("%s on ssh:%s", base, remote_label (r));
	bool ok = start_terminal (bp, name, argv, NULL, shown, r, dir);
	free (shown);
	free (base);
	free (first);
	free (argv);
	sv_clear (&keep);
	free (keep.v);
	return ok;
}

static bool
F_shell (long uniarg, Args *args)
{
	/* From a remote buffer, the shell runs on its machine. */
	Remote *r = cur_bp->remote;
	char *base_name = terminal_name ("shell", r);
	/* A NULL `args' means a prefix argument was given. */
	Buffer *bp = NULL;
	char *name;
	if (args == NULL && (lastflag & FLAG_UNIARG_EMPTY))
		name = buffer_unique_name (base_name);
	else {
		long n = args == NULL ? uniarg : 1;
		if (n < 1) {
			minibuf_error ("Shell number must be positive");
			free (base_name);
			return false;
		}
		name = n == 1 ? xstrdup (base_name) : xasprintf ("%s<%ld>", base_name, n);
		bp = buffer_find (name);
		if (bp != NULL && (bp->shell == NULL || bp->remote != r)) {
			if (bp->shell == NULL)
				minibuf_error ("Buffer %s is not a terminal", name);
			else
				minibuf_error ("Buffer %s is a terminal on %s%s", name, bp->remote != NULL ? "ssh:" : "",
					       bp->remote != NULL ? remote_label (bp->remote) : "this machine");
			free (name);
			free (base_name);
			return false;
		}
		if (bp != NULL && shell_process_alive (bp)) {
			buffer_switch_to (bp, true);
			free (name);
			free (base_name);
			return true;
		}
	}
	free (base_name);

	bool ok;
	if (r != NULL) {
		char *dir, *shell;
		ok = check_remote_terminal (r, &dir, &shell);
		if (ok)
			ok = start_remote_terminal (bp, name, r, dir, NULL, shell);
		free (dir);
		free (shell);
	} else {
		/* Run a login shell, as terminal emulators do. */
		char *shell = user_shell ();
		char *base = path_basename (shell);
		char *argv0 = xstrcat3 ("-", base, "");
		const char *argv[] = { argv0, NULL };
		ok = start_terminal (bp, name, argv, shell, base, NULL, NULL);
		free (argv0);
		free (base);
		free (shell);
	}
	free (name);
	return ok;
}

static bool
F_term (long uniarg, Args *args)
{
	(void) uniarg;
	Remote *r = cur_bp->remote;
	char *dir = NULL, *shell;
	if (r != NULL) {
		if (!check_remote_terminal (r, &dir, &shell))
			return false;
	} else
		shell = user_shell ();
	char *program = xstrdup_or_null (args_poll (args));
	if (program == NULL) {
		program = r != NULL ? minibuf_read ("Run program on ssh:%s: ", shell, remote_label (r))
			: minibuf_read ("Run program: ", shell);
		if (program == NULL) {
			free (shell);
			free (dir);
			return funcall ("keyboard-quit");
		}
	}
	bool ok = false;
	if (program[0] != '\0' && r != NULL) {
		char *name = terminal_name ("terminal", r);
		char *unique = buffer_unique_name (name);
		ok = start_remote_terminal (NULL, unique, r, dir, program, shell);
		free (unique);
		free (name);
	} else if (program[0] != '\0') {
		char *cmd = xstrcat3 ("exec ", program, "");
		const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
		char *first = xstrndup (program, strcspn (program, " \t"));
		char *base = path_basename (first);
		ok = start_terminal (NULL, "*terminal*", argv, NULL, base, NULL, NULL);
		free (base);
		free (first);
		free (cmd);
	}
	free (program);
	free (shell);
	free (dir);
	return ok;
}

void
terminal_init (void)
{
	lisp_func_new ("shell", F_shell, true,
		       "Run your login shell ($SHELL) in a terminal buffer named *shell*.\n"
		       "If a shell is already running there, switch to it; if its shell has\n"
		       "exited, start a new one in the same buffer.\n"
		       "With a numeric prefix argument N, use the buffer *shell*<N> instead\n"
		       "(N = 1 is *shell*). With just C-u, start another shell, in a new\n"
		       "buffer *shell* or *shell*<N>.\n"
		       "\n"
		       "In a remote buffer, run the remote user's $SHELL on its machine, in\n"
		       "its directory there, on a remote terminal; the buffers are then\n"
		       "*shell ssh:HOST*, *shell ssh:HOST*<N>, apart from local shells.\n"
		       "\n"
		       "Keys go to the shell, except for C-x, which is a prefix for the usual\n"
		       "C-x commands, and M-x. C-x C-x sends C-x, C-x C-y pastes the last\n"
		       "killed text, and C-x C-j switches to line mode, where the scrollback\n"
		       "is ordinary text; C-x C-k or q returns to char mode.\n"
		       "\n"
		       "A program that reads the mouse gets the clicks, drags and wheel\n"
		       "in its window. Otherwise, dragging selects text on the screen and\n"
		       "copies it to the kill ring and the clipboard; M-w (Command-C in\n"
		       "xzile-gui) copies the selection again. A click on a FILE:LINE[:COL]\n"
		       "reference visits that place in the window on the left, splitting a\n"
		       "sole window side by side for it; a terminal that reports another\n"
		       "host by OSC 7, or runs ssh in the foreground, refuses this.");
	lisp_func_new ("term", F_term, true,
		       "Run PROGRAM in a terminal buffer named *terminal*.\n"
		       "In a remote buffer, run it on its machine, in its directory there,\n"
		       "in *terminal ssh:HOST*; the default is the remote user's $SHELL.\n"
		       "See `shell' for the keys.");
	lisp_func_new ("term-send-raw", F_term_send_raw, false,
		       "Send the last key typed to the terminal's program.");
	lisp_func_new ("term-send-C-x", F_term_send_C_x, true,
		       "Send C-x to the terminal's program.");
	lisp_func_new ("term-paste", F_term_paste, true,
		       "Send the last killed text to the terminal, as if pasted.");
	lisp_func_new ("term-copy", F_term_copy, true,
		       "Copy the text selected with the mouse in the terminal to the kill\n"
		       "ring and the clipboard, and drop the selection. With no selection,\n"
		       "send the key typed to the terminal's program.");
	lisp_func_new ("term-mouse-release", F_term_mouse_release, false,
		       "End a drag of the mouse in the terminal, copying the text selected\n"
		       "to the kill ring and the clipboard.");
	lisp_func_new ("term-line-mode", F_term_line_mode, true,
		       "Show the terminal's scrollback and screen as text, for moving around,\n"
		       "searching and copying. C-x C-k or q returns to char mode.");
	lisp_func_new ("term-char-mode", F_term_char_mode, true,
		       "Show the terminal and send keys to its program.");
	lisp_func_new ("term-interrupt", F_term_interrupt, true,
		       "Interrupt the terminal's program.");
}
