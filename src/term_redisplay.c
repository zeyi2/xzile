/* Redisplay engine

   Copyright (c) 1997-2020 Free Software Foundation, Inc.
   Copyright (c) 2025 Zeyi2 <zeyi2@nekoarch.cc>

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

#include "xzile.h"

#include <assert.h>

/*
 * Return a printable representation for a non-printable code point at
 * display column `x`.
 *
 * Tab   spaces up to the next tab stop.
 * C0    caret notation ("^@" … "^_").
 * DEL   "^?".
 * C1    octal escape ("\200" … "\237").
 */
static void
make_char_printable (uint32_t ch, size_t x, size_t cur_tab_width, char *buf, size_t size)
{
	if (ch == '\t')
		snprintf (buf, size, "%*s", (int) (cur_tab_width - x % cur_tab_width), "");
	else if (ch < 0x20)
		snprintf (buf, size, "^%c", (int) ('@' + ch));
	else if (ch == 0x7F)
		snprintf (buf, size, "^?");
	else if (ch >= 0x80 && ch < 0xA0)
		snprintf (buf, size, "\\%o", (unsigned) ch);
	else
		snprintf (buf, size, "?");
}

static void
add_spaces (size_t n)
{
	for (size_t i = 0; i < n; i++)
		term_addch (' ');
}

/* Add `s', truncated to at most `width' display columns. */
static void
add_clipped (const char *s, size_t width)
{
	size_t len = strlen (s), i = 0, col = 0;
	while (i < len) {
		size_t char_len;
		uint32_t ch = utf8_decode (s, len, i, &char_len);
		size_t w = (size_t) utf8_char_display_width (ch);
		if (col + w > width)
			break;
		col += w;
		i += char_len;
	}
	term_addnstr (s, i);
}

/*
 * Draw one buffer line onto terminal row `line`.
 *
 * `startcol`     – first display column of buffer content to show.
 * `gutter_str`   – line number (or spaces) for the gutter; empty if gutter_width 0.
 * `gutter_width` – width of the gutter; text area width is wp->ewidth - gutter_width.
 */
/* The brackets to show as matching (or not) in the window being drawn. */
static size_t paren_at[2] = { NOPOS, NOPOS };
static const char *paren_face;

static void
find_parens (Window *wp)
{
	paren_at[0] = paren_at[1] = NOPOS;
	bool mismatch;
	if (wp == cur_wp && get_variable_bool ("show-paren-mode") &&
	    paren_match (wp->bp, window_o (wp), &paren_at[0], &paren_at[1], &mismatch))
		paren_face = mismatch ? FACE_SHOW_PAREN_MISMATCH : FACE_SHOW_PAREN_MATCH;
	else
		paren_at[0] = paren_at[1] = NOPOS;
}

static void
draw_line (size_t line, size_t leftcol, size_t startcol, Window *wp,
	   size_t o, const Region *r, bool highlight, size_t cur_tab_width,
	   const char *gutter_str, size_t gutter_width, const char *gutter_face_name)
{
	Buffer *bp = wp->bp;
	term_move (line, leftcol);

	if (gutter_width > 0) {
		term_apply_face (gutter_face_name);
		term_addstr (gutter_str);
		term_move (line, leftcol + gutter_width);
	}

	size_t line_len = buffer_line_len (bp, o);
	size_t ew = gutter_width < wp->ewidth ? wp->ewidth - gutter_width : 0;
	const char **syntax_faces = NULL;

	if (bp->highlighter != NULL && line_len > 0) {
		syntax_faces = xzalloc (line_len * sizeof (char *));
		size_t line_idx = buffer_offset_to_line (bp, o);
		int start_state = buffer_get_line_start_state (bp, line_idx);
		bp->highlighter->scan_line (bp->highlighter, bp, line_idx, o, line_len, start_state, syntax_faces);
	}

	size_t i = 0;	/* byte index within the line */
	size_t col = 0;	/* accumulated display column (from line start) */
	size_t x = 0;	/* display columns written to terminal so far */

	while (i < line_len) {
		size_t char_len;
		uint32_t ch = buffer_get_utf8_char (bp, o + i, &char_len);

		size_t w;
		if (ch == '\t')
			w = cur_tab_width - col % cur_tab_width;
		else
			w = (size_t) utf8_char_display_width (ch);

		if (col + w <= startcol) {
			col += w;
			i += char_len;
			continue;
		}

		bool in_highlight = highlight && r != NULL && region_contains (*r, o + i);
		const char *face_name = FACE_DEFAULT;
		if (syntax_faces != NULL && syntax_faces[i] != NULL)
			face_name = syntax_faces[i];
		if (o + i == paren_at[0] || o + i == paren_at[1])
			face_name = paren_face;
		term_apply_face (in_highlight ? FACE_REGION : face_name);

		if (col < startcol) {
			/* A character straddling the start column. */
			size_t visible = col + w - startcol;
			col += w;
			i += char_len;
			while (visible-- > 0 && x < ew) {
				term_addch (' ');
				x++;
			}
			continue;
		}

		if (x >= ew)
			break;

		if (x + w > ew) {
			add_spaces (ew - x);
			x = ew;
			break;
		}

		/* Emit the character. */
		if (ch == '\t' || ch < 0x20 || ch == 0x7F
		    || (ch >= 0x80 && ch < 0xA0) || ch == UTF8_REPLACEMENT) {
			char s[16];
			make_char_printable (ch, col, cur_tab_width, s, sizeof s);
			term_addstr (s);
			x += strlen (s);
		} else {
			char buf[4];
			for (size_t k = 0; k < char_len && k < sizeof buf; k++)
				buf[k] = buffer_get_char (bp, o + i + k);
			term_addnstr (buf, char_len);
			x += w;
		}

		col += w;
		i += char_len;
	}
	free (syntax_faces);

	/* Draw end-of-line indicator or padding. */
	if (x >= ew && i < line_len) {
		term_move (line, leftcol + gutter_width + ew - 1);
		term_apply_face (FACE_DEFAULT);
		term_addstr ("$");
	} else if (x < ew) {
		term_apply_face (FACE_DEFAULT);
		add_spaces (ew - x);
	}
	term_apply_face (FACE_DEFAULT);
}

static bool
calculate_highlight_region (Window *wp, Region *rp)
{
	if ((wp != cur_wp && !get_variable_bool ("highlight-nonselected-windows"))
	    || wp->bp->mark == NULL
	    || !wp->bp->mark_active)
		return false;

	*rp = region_new (window_o (wp), wp->bp->mark->o);
	return true;
}

static const char *
make_mode_line_flags (Window *wp)
{
	if (wp->bp->modified && wp->bp->readonly)
		return "%*";
	else if (wp->bp->modified)
		return "**";
	else if (wp->bp->readonly)
		return "%%";
	return "--";
}

static char *
make_screen_pos (Window *wp)
{
	bool tv = window_top_visible (wp);
	bool bv = window_bottom_visible (wp);

	if (tv && bv)
		return xstrdup ("All");
	else if (tv)
		return xstrdup ("Top");
	else if (bv)
		return xstrdup ("Bot");
	size_t len = buffer_length (wp->bp);
	return xasprintf ("%2d%%", len > 0 ? (int) (100.0 * (double) window_o (wp) / (double) len) : 0);
}

/*
 * Read the line-number modes for a buffer: whether absolute and/or
 * relative line numbers are enabled.
 */
static void
get_line_number_modes (Buffer *bp, bool *abs_mode, bool *rel_mode)
{
	const char *v = buffer_get_variable (bp, "line-number-mode");
	*abs_mode = v != NULL && strcmp (v, "nil") != 0;
	v = buffer_get_variable (bp, "relative-line-number-mode");
	*rel_mode = v != NULL && strcmp (v, "nil") != 0;
}

static bool
buffer_has_linenum (Buffer *bp)
{
	bool abs_mode, rel_mode;
	get_line_number_modes (bp, &abs_mode, &rel_mode);
	return abs_mode || rel_mode;
}

typedef struct {
	size_t right_x, top_y, height;
	bool has;
} NeighborData;

static void
check_right_neighbor (WinNode *leaf, void *data)
{
	NeighborData *d = data;
	if (leaf->x == d->right_x &&
	    leaf->y < d->top_y + d->height && leaf->y + leaf->height > d->top_y &&
	    buffer_has_linenum (leaf->wp->bp))
		d->has = true;
}

static bool
right_neighbor_has_linenum (size_t right_x, size_t top_y, size_t height)
{
	NeighborData d = { right_x, top_y, height, false };
	winnode_each_leaf (root_node, check_right_neighbor, &d);
	return d.has;
}

/*
 * Compute gutter width for a window from its buffer's line-number settings.
 * Gutter width = digit count for max line number + 2 (space + margin);
 * relative mode can show negative numbers so the same formula covers both.
 */
static void
update_gutter_width (WinNode *leaf, void *data)
{
	(void) data;
	Window *wp = leaf->wp;
	if (!buffer_has_linenum (wp->bp) || shell_char_mode (wp->bp)) {
		wp->gutter_width = 0;
		return;
	}

	size_t n = buffer_offset_to_line (wp->bp, buffer_length (wp->bp)) + 1;
	size_t digits = 0;
	do {
		n /= 10;
		digits++;
	} while (n > 0);

	wp->gutter_width = digits + 2;
}

static void
draw_status_line (size_t line, size_t leftcol, Window *wp)
{
	term_apply_face (wp == cur_wp ? FACE_MODE_LINE : FACE_MODE_LINE_INACTIVE);

	term_move (line, leftcol);
	for (size_t i = 0; i < wp->ewidth; ++i)
		term_addstr ("-");

	const char *eol = buffer_eol (wp->bp);
	const char *eol_type;
	if (strcmp (eol, eol_cr) == 0)
		eol_type = "(Mac)";
	else if (strcmp (eol, eol_crlf) == 0)
		eol_type = "(DOS)";
	else
		eol_type = ":";

	term_move (line, leftcol);
	size_t n = buffer_offset_to_line (wp->bp, window_o (wp));
	char *pos = make_screen_pos (wp);
	char *linecol = xasprintf ("(%zu,%zu)", n + 1, buffer_calculate_goalc (wp->bp, window_o (wp)));
	StrBuf a;
	sb_init (&a);
	sb_addf (&a, "--%s%2s  %-15s   %s %-9s (%s", eol_type, make_mode_line_flags (wp),
		 wp->bp->name, pos, linecol, buffer_mode_name (wp->bp));
	free (pos);
	free (linecol);

	if (wp->bp->autofill)
		sb_add (&a, " Fill");
	if (thisflag & FLAG_DEFINING_MACRO)
		sb_add (&a, " Def");
	if (wp->bp->isearch)
		sb_add (&a, " Isearch");

	sb_add (&a, ")");
	add_clipped (a.s, wp->ewidth);
	sb_free (&a);

	term_attrset (FONT_NORMAL);
}

static void
draw_text (Window *wp, size_t topline, size_t leftcol, const Region *r)
{
	Buffer *bp = wp->bp;
	size_t i, o;

	/* Find the first line to display on the first screen line. */
	for (o = buffer_start_of_line (bp, window_o (wp)), i = wp->topdelta; i > 0 && o > 0; --i) {
		o = buffer_prev_line (bp, o);
		assert (o != NOPOS);
	}

	size_t gutter_width = wp->gutter_width;
	bool abs_mode, rel_mode;
	get_line_number_modes (bp, &abs_mode, &rel_mode);

	size_t current_line = buffer_offset_to_line (bp, window_o (wp));
	size_t line_idx = wp->topdelta <= current_line ? current_line - wp->topdelta : 0;
	size_t text_w = gutter_width < wp->ewidth ? wp->ewidth - gutter_width : 0;

	size_t cur_tab_width = buffer_tab_width (bp);
	for (i = topline; i < wp->eheight + topline; ++i) {
		term_move (i, leftcol);

		if (o == NOPOS) {
			term_apply_face (FACE_DEFAULT);
			add_spaces (gutter_width + text_w);
			continue;
		}

		char gutter_str[32] = "";
		const char *gutter_face_name = FACE_LINE_NUMBER;
		if (gutter_width > 0) {
			long num_to_show = 0;
			if (line_idx == current_line)
				gutter_face_name = FACE_LINE_NUMBER_CURRENT_LINE;
			if (rel_mode) {
				if (line_idx == current_line)
					num_to_show = (long) line_idx + 1;
				else {
					long diff = (long) line_idx - (long) current_line;
					num_to_show = diff < 0 ? -diff : diff;
				}
			} else if (abs_mode)
				num_to_show = (long) line_idx + 1;
			snprintf (gutter_str, sizeof gutter_str, "%*ld ", (int) (gutter_width - 1), num_to_show);
		}

		draw_line (i, leftcol, wp->start_column, wp, o, r, r != NULL,
			   cur_tab_width, gutter_str, gutter_width, gutter_face_name);

		if (wp->start_column > 0) {
			term_move (i, leftcol + gutter_width);
			term_addstr ("$");
		}

		o = buffer_next_line (bp, o);
		line_idx++;
	}

	wp->all_displayed = o == NOPOS || o >= buffer_length (bp);

}

static void
draw_window (size_t topline, size_t leftcol, Window *wp)
{
	Region r;
	bool highlight = calculate_highlight_region (wp, &r);

	if (shell_draw (wp, topline, leftcol))
		wp->all_displayed = true;
	else {
		find_parens (wp);
		draw_text (wp, topline, leftcol, highlight ? &r : NULL);
		paren_at[0] = paren_at[1] = NOPOS;
	}

	if (wp->fheight > wp->eheight)
		draw_status_line (topline + wp->eheight, leftcol, wp);

	if (wp->fwidth > wp->ewidth) {
		size_t sep_col = leftcol + wp->ewidth;
		bool right_has_linenum = right_neighbor_has_linenum (leftcol + wp->fwidth, topline, wp->fheight);
		const char *sep_str = right_has_linenum ? " |" : "| ";
		for (size_t row = topline; row < topline + wp->fheight; ++row) {
			term_move (row, sep_col);
			term_apply_face (FACE_VERTICAL_BORDER);
			term_addstr (sep_str);
		}
		term_apply_face (FACE_DEFAULT);
	}
}

static size_t col;
static size_t cur_topline = 0;
static size_t cur_leftcol = 0;

static void
draw_leaf (WinNode *leaf, void *data)
{
	(void) data;
	Window *wp = leaf->wp;
	draw_window (leaf->y, leaf->x, wp);
	if (wp == cur_wp) {
		cur_topline = leaf->y;
		cur_leftcol = leaf->x;
	}
}

void
term_redisplay (void)
{
	if (cur_wp == NULL)
		return;

	update_windows_geometry (0, 0, term_width (), get_main_window_height ());

	/* Update gutter width for each window from its buffer's line-number settings. */
	winnode_each_leaf (root_node, update_gutter_width, NULL);

	/* Calculate the start column if the line at point has to be truncated.
	 * Effective text width is ewidth minus the gutter. */
	Buffer *bp = cur_wp->bp;
	size_t t = buffer_tab_width (bp);
	size_t line_start = buffer_line_o (bp);
	size_t pt_o = window_o (cur_wp);

	col = 0;
	cur_wp->start_column = 0;

	size_t ew = cur_wp->gutter_width < cur_wp->ewidth ? cur_wp->ewidth - cur_wp->gutter_width : 0;

	/* cursor_col: display columns from the start of the line to the point. */
	size_t cursor_col = buffer_calculate_goalc (bp, pt_o);

	if (cursor_col >= ew) {
		/* The cursor is off the right edge.  Choose a start column so the
		 * cursor lands in the right third of the window. */
		size_t target_from_right = ew * 2 / 3;
		size_t pos = line_start;
		size_t running_col = 0;

		while (pos < pt_o) {
			size_t char_len;
			uint32_t ch = buffer_get_utf8_char (bp, pos, &char_len);
			size_t w = ch == '\t' ? t - running_col % t : (size_t) utf8_char_display_width (ch);

			if (cursor_col - running_col <= target_from_right)
				break;

			running_col += w;
			pos += char_len;
		}

		cur_wp->start_column = running_col;
		col = cursor_col - running_col;
	} else
		col = cursor_col;

	/* Draw the windows. */
	cur_topline = 0;
	winnode_each_leaf (root_node, draw_leaf, NULL);

	term_redraw_cursor ();
}

void
term_redraw_cursor (void)
{
	if (cur_wp == NULL)
		return;
	size_t y, x;
	if (shell_char_mode (cur_wp->bp)) {
		bool visible = shell_cursor (cur_wp, &y, &x);
		term_set_cursor_visible (visible);
		term_move (cur_topline + MIN (y, cur_wp->eheight - 1), cur_leftcol + MIN (x, cur_wp->ewidth - 1));
		return;
	}
	term_set_cursor_visible (true);
	term_move (cur_topline + cur_wp->topdelta, cur_leftcol + cur_wp->gutter_width + col);
}

/*
 * Redisplay after a subprocess produced output, leaving the cursor in
 * the minibuffer if it is being read.
 */
void
redisplay_after_output (void)
{
	if (!term_initialized () || cur_wp == NULL)
		return;
	size_t y, x;
	term_get_cursor (&y, &x);
	window_resync (cur_wp);
	term_redisplay ();
	if (term_minibuf_active ()) {
		term_set_cursor_visible (true);
		term_move (y, x);
	}
	term_refresh ();
}

/*
 * Tidy and close the terminal ready to leave Zile.
 */
void
term_finish (void)
{
	term_move (term_height () - 1, 0);
	term_clrtoeol ();
	term_attrset (FONT_NORMAL);
	term_refresh ();
	term_close ();
}
