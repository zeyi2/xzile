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
#include "tui.h"

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

/* What a line shows besides its text, found once for all its rows. */
typedef struct {
	const char **syntax;	/* The face of each byte, or NULL. */
	LinkSpan *links;	/* References that a click would follow. */
	size_t nlinks;
	LinkSpan *matches;	/* The matches of a search running. */
	size_t nmatches;
	bool have_cur;
	size_t cur_start, cur_end;
} LineFaces;

static void
line_faces (Window *wp, size_t o, size_t line_len, bool links, LineFaces *lf)
{
	Buffer *bp = wp->bp;
	*lf = (LineFaces) { 0 };
	if (bp->highlighter != NULL && line_len > 0) {
		lf->syntax = xzalloc (line_len * sizeof (char *));
		size_t line_idx = buffer_offset_to_line (bp, o);
		int start_state = buffer_get_line_start_state (bp, line_idx);
		bp->highlighter->scan_line (bp->highlighter, bp, line_idx, o, line_len, start_state, lf->syntax);
	}
	lf->nlinks = links && line_len > 0 ? reference_line_links (bp, o, &lf->links) : 0;
	lf->nmatches = search_line_matches (wp, o, line_len, &lf->matches);
	lf->have_cur = search_current_match (wp, &lf->cur_start, &lf->cur_end);
}

static void
line_faces_free (LineFaces *lf)
{
	free (lf->syntax);
	free (lf->links);
	free (lf->matches);
}

/* Where the cursor goes in the current window, once found, and its
   column in the line at point when truncated. */
static bool cursor_found;
static size_t cursor_y, cursor_x;
static size_t col;

/*
 * Draw a row of the line at `o' onto terminal row `line': its bytes
 * from `row->start', at display column `row->col' of the line, to
 * `row->end', or to the end of the line if it is the last row. When
 * `truncate', the line shows from column `row->col' to the edge, with a
 * `$' if it goes further.
 */
static void
draw_row (size_t line, size_t leftcol, Window *wp, size_t o, const DisplayRow *row, bool last, bool truncate,
	  const Region *r, const LineFaces *lf)
{
	Buffer *bp = wp->bp;
	size_t gutter_width = wp->gutter_width;
	size_t cur_tab_width = buffer_tab_width (bp);
	term_move (line, leftcol + gutter_width);

	size_t line_len = buffer_line_len (bp, o);
	size_t ew = gutter_width < wp->ewidth ? wp->ewidth - gutter_width : 0;
	if (markdown_draw_line (wp, o, ew, row->col, last ? SIZE_MAX : row->end, r, truncate)) {
		term_apply_face (FACE_DEFAULT);
		return;
	}

	size_t startcol = truncate ? row->col : 0;
	size_t i = truncate ? 0 : row->start;	/* byte index within the line */
	size_t col = truncate ? 0 : row->col;	/* display column from the line start */
	size_t end = last ? line_len : row->end;
	size_t x = 0;	/* display columns written so far */
	size_t pt = wp == cur_wp && !truncate ? window_o (wp) : NOPOS;
	size_t k = 0, m = 0;

	while (i < end) {
		size_t char_len;
		uint32_t ch = buffer_get_utf8_char (bp, o + i, &char_len);
		char_len = char_len ? char_len : 1;
		size_t w = display_char_width (ch, col, cur_tab_width);

		if (col + w <= startcol) {
			col += w;
			i += char_len;
			continue;
		}
		if (o + i == pt) {
			cursor_found = true;
			cursor_y = line;
			cursor_x = MIN (x, ew > 0 ? ew - 1 : 0);
		}

		bool in_highlight = r != NULL && region_contains (*r, o + i);
		const char *face_name = FACE_DEFAULT;
		bool plain = true;
		if (lf->syntax != NULL && lf->syntax[i] != NULL) {
			face_name = lf->syntax[i];
			plain = false;
		}
		if (o + i == paren_at[0] || o + i == paren_at[1]) {
			face_name = paren_face;
			plain = false;
		}
		while (m < lf->nmatches && lf->matches[m].end <= i)
			m++;
		const char *shown = lf->have_cur && o + i >= lf->cur_start && o + i < lf->cur_end ? FACE_ISEARCH
			: m < lf->nmatches && i >= lf->matches[m].start ? FACE_LAZY_HIGHLIGHT
			: in_highlight ? FACE_REGION : face_name;
		term_apply_face (shown);
		while (k < lf->nlinks && lf->links[k].end <= i)
			k++;
		if (k < lf->nlinks && i >= lf->links[k].start)
			term_mark_link (plain && shown == face_name);

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
			/* Wider than what is left: a tab, or what was too wide
			   for the window at all. */
			add_spaces (ew - x);
			x = ew;
			col += w;
			i += char_len;
			break;
		}

		if (ch == '\t' || ch < 0x20 || ch == 0x7F
		    || (ch >= 0x80 && ch < 0xA0) || ch == UTF8_REPLACEMENT) {
			char s[16];
			make_char_printable (ch, col, cur_tab_width, s, sizeof s);
			term_addstr (s);
			x += strlen (s);
		} else {
			char buf[4];
			for (size_t b = 0; b < char_len && b < sizeof buf; b++)
				buf[b] = buffer_get_char (bp, o + i + b);
			term_addnstr (buf, char_len);
			x += w;
		}
		col += w;
		i += char_len;
	}
	/* Point at the end of the line, or after what the row could show. */
	if ((last && pt == o + line_len && i >= end) || (pt != NOPOS && pt >= o + i && pt < o + end)) {
		cursor_found = true;
		cursor_y = line;
		cursor_x = MIN (x, ew > 0 ? ew - 1 : 0);
	}

	term_apply_face (FACE_DEFAULT);
	if (truncate && x >= ew && i < line_len) {
		term_move (line, leftcol + gutter_width + ew - 1);
		term_addstr ("$");
	} else if (x < ew)
		add_spaces (ew - x);
	if (truncate && startcol > 0) {
		term_move (line, leftcol + gutter_width);
		term_addstr ("$");
	}
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

/*--------------------------------------------------------------------------
 * Window chrome: the mode line, scroll bars and dividers, which the
 * mouse can use.
 *--------------------------------------------------------------------------*/

enum {
	CHROME_KIND = 0x40,	/* Region keys: mode line parts. */
	SCROLL_KIND,		/* Scroll bars. */
	DIVIDER_KIND,		/* Dividers between windows. */
};

/* What a click on a part of the mode line does. */
enum ChromeAction {
	ACT_NONE, ACT_SAVE, ACT_SWITCH_BUFFER, ACT_READ_ONLY, ACT_GOTO_LINE, ACT_BINDINGS,
};

static const char *const action_commands[] = {
	NULL, "save-buffer", "switch-to-buffer", "toggle-read-only", "goto-line", "describe-bindings",
};

static Window *
window_by_id (unsigned long id)
{
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (wp->id == id)
			return wp;
	return NULL;
}

static void
keep_point_visible (WinNode *leaf, void *data)
{
	(void) data;
	Window *wp = leaf->wp;
	if (wp->topdelta >= wp->eheight)
		wp->topdelta = wp->eheight > 0 ? wp->eheight - 1 : 0;
}

/* A press on a divider, or on a mode line that is one, starts a drag
   that resizes the windows either side; a double click shares the space
   equally. A release where it was pressed is a click on the part of the
   mode line pressed: it selects the window, and runs the part's
   command. The press keeps the split, and the layout generation to
   know that the split still exists. */
static bool
chrome_event (const TuiEvent *ev, void *data)
{
	(void) data;
	long y = ev->rect.y + ev->y, x = ev->rect.x + ev->x;
	switch (ev->kind) {
	case TUI_PRESS: {
		WinNode *split = layout_divider_at ((size_t) y, (size_t) x);
		if (split == NULL && ev->b == ACT_NONE)
			return false;
		ev->state[0] = (long) (uintptr_t) split;
		ev->state[1] = (long) layout_generation ();
		if (split != NULL) {
			bool rows = split->kind == SPLIT_ROWS;
			ev->state[2] = rows ? y : x;
			ev->state[3] = (long) (rows ? split->left_or_top->height : split->left_or_top->width);
			if (ev->clicks == 2 && ev->b == ACT_NONE) {
				size_t total = rows ? split->height : split->width;
				if (layout_resize_split (split, total / 2))
					update_windows_geometry (0, 0, term_width (), get_main_window_height ());
			}
		}
		return true;
	}
	case TUI_DRAG: {
		WinNode *split = (WinNode *) (uintptr_t) ev->state[0];
		if (split == NULL || (unsigned long) ev->state[1] != layout_generation ())
			return true;
		long size = ev->state[3] - ev->state[2] + (split->kind == SPLIT_ROWS ? y : x);
		if (layout_resize_split (split, (size_t) MAX (size, 0))) {
			update_windows_geometry (0, 0, term_width (), get_main_window_height ());
			winnode_each_leaf (root_node, keep_point_visible, NULL);
		}
		return true;
	}
	case TUI_RELEASE:
		if (!ev->moved && ev->a != 0) {
			Window *wp = window_by_id ((unsigned long) ev->a);
			if (wp != NULL && wp != cur_wp)
				window_set_current (wp);
			if (wp != NULL && ev->b != ACT_NONE)
				run_command_after_click (action_commands[ev->b]);
		}
		return true;
	case TUI_WHEEL:
		return false;
	}
	return false;
}

static TuiKey
divider_key (const WinNode *split)
{
	return TUI_KEY (DIVIDER_KIND, (uintptr_t) split >> 4);
}

/* Regions for the dividers of `n' and the splits in it: the separator
   columns between windows side by side, and the mode line between
   windows one above the other. Recorded before the windows are drawn,
   so that the parts of mode lines are on top. */
static void
add_divider_regions (WinNode *n)
{
	if (n == NULL || n->is_leaf)
		return;
	WinNode *first = n->left_or_top;
	TuiRegionSpec spec = { divider_key (n), chrome_event, NULL, 0, 0, ACT_NONE,
			       n->kind == SPLIT_ROWS ? TUI_CURSOR_NS_RESIZE : TUI_CURSOR_EW_RESIZE, false };
	if (n->kind == SPLIT_COLS && first->width >= 2)
		tui_region ((TuiRect) { (int) (first->x + first->width - 2), (int) n->y, 2, (int) n->height }, &spec);
	else if (n->kind == SPLIT_ROWS && first->height >= 1)
		tui_region ((TuiRect) { (int) n->x, (int) (first->y + first->height - 1), (int) n->width, 1 }, &spec);
	add_divider_regions (first);
	add_divider_regions (n->right_or_bottom);
}

/* Whether the divider at row `y', column `x' is hovered or dragged. */
static bool
divider_hot (size_t y, size_t x)
{
	WinNode *split = layout_divider_at (y, x);
	return split != NULL && tui_hovered (divider_key (split));
}

/* The status line: a bar with the buffer on the left and the position
   on the right, as in modern editors. Its parts can be clicked. */
typedef struct {
	const char *text;
	unsigned flags;
	enum ChromeAction action;
} Part;

static void
draw_status_line (size_t line, size_t leftcol, Window *wp)
{
	const char *face = wp == cur_wp ? FACE_MODE_LINE : FACE_MODE_LINE_INACTIVE;
	TuiRect bar = { (int) leftcol, (int) line, (int) (wp->ewidth + wp->scroll_bar_width), 1 };
	tui_clear (bar, face);

	bool u = tui_unicode ();
	char *ssh = wp->bp->remote != NULL ? xasprintf ("ssh:%s", remote_label (wp->bp->remote)) : NULL;
	Part left[8];
	size_t nl = 0;
	left[nl++] = (Part) { wp->bp->modified ? (u ? "●" : "*") : " ", 0, wp->bp->modified ? ACT_SAVE : ACT_NONE };
	left[nl++] = (Part) { wp->bp->name, TUI_BOLD, ACT_SWITCH_BUFFER };
	if (ssh != NULL)
		left[nl++] = (Part) { ssh, 0, ACT_NONE };
	if (wp->bp->readonly)
		left[nl++] = (Part) { "[read-only]", 0, ACT_READ_ONLY };
	if (wp->bp->autofill)
		left[nl++] = (Part) { "Fill", 0, ACT_NONE };
	if (thisflag & FLAG_DEFINING_MACRO)
		left[nl++] = (Part) { "Def", 0, ACT_NONE };
	if (wp->bp->isearch)
		left[nl++] = (Part) { "Isearch", 0, ACT_NONE };

	const char *eol = buffer_eol (wp->bp);
	char *pos = make_screen_pos (wp);
	char *preview = preview_status (wp);
	char *linecol = preview != NULL ? preview
		: xasprintf ("Ln %zu, Col %zu", buffer_offset_to_line (wp->bp, window_o (wp)) + 1,
			     buffer_calculate_goalc (wp->bp, window_o (wp)) + 1);
	Part right[4];
	size_t nr = 0;
	right[nr++] = (Part) { linecol, 0, preview != NULL ? ACT_NONE : ACT_GOTO_LINE };
	if (preview == NULL) {
		right[nr++] = (Part) { pos, 0, ACT_NONE };
		right[nr++] = (Part) { strcmp (eol, eol_crlf) == 0 ? "CRLF" : strcmp (eol, eol_cr) == 0 ? "CR" : "LF",
				       0, ACT_NONE };
	}
	right[nr++] = (Part) { buffer_mode_name (wp->bp), 0, ACT_BINDINGS };

	/* The widths: in a narrow window, the parts on the right go, from
	   the right, to leave room for the left ones, then the position. */
	int lw = 1;
	for (size_t i = 0; i < nl; i++)
		lw += tui_width (left[i].text, strlen (left[i].text)) + 2;
	int rw[4], rtotal = 0;
	for (size_t i = 0; i < nr; i++) {
		rw[i] = tui_width (right[i].text, strlen (right[i].text)) + 3;
		rtotal += rw[i];
	}
	while (nr > 1 && lw + rtotal + 1 > bar.w)
		rtotal -= rw[--nr];
	if (lw + rtotal + 1 > bar.w) {
		nr = 0;
		rtotal = 0;
	}

	/* Draw the parts, each clickable if it does something. */
	int x = bar.x + 1;
	for (int pass = 0; pass < 2; pass++) {
		Part *parts = pass == 0 ? left : right;
		size_t n = pass == 0 ? nl : nr;
		if (pass == 1)
			x = bar.x + bar.w - rtotal;
		for (size_t i = 0; i < n; i++) {
			int w = tui_width (parts[i].text, strlen (parts[i].text)) + 2;
			int end = pass == 0 ? bar.x + bar.w - rtotal - 1 : bar.x + bar.w - 1;
			if (x >= end)
				break;
			TuiKey key = TUI_KEY (CHROME_KIND, (wp->id << 4) | (pass * 8 + i));
			bool hot = parts[i].action != ACT_NONE && tui_hovered (key);
			const char *pf = hot ? FACE_MODE_LINE_HIGHLIGHT : NULL;
			int drawn = tui_put (bar.y, x, end - x, " ", pf != NULL ? pf : face, 0);
			drawn += tui_put (bar.y, x + drawn, end - x - drawn, parts[i].text, pf != NULL ? pf : face,
					  parts[i].flags);
			drawn += tui_put (bar.y, x + drawn, end - x - drawn, " ", pf != NULL ? pf : face, 0);
			if (parts[i].action != ACT_NONE) {
				TuiRegionSpec spec = { key, chrome_event, NULL, 0, (long) wp->id, parts[i].action,
						       TUI_CURSOR_POINTER, false };
				tui_region ((TuiRect) { x, bar.y, drawn, 1 }, &spec);
			}
			x += MAX (drawn, w) + (pass == 1 ? 1 : 0);
		}
	}

	free (ssh);
	free (pos);
	free (linecol);
}

/* Scroll window `a' to show from row `offset'. */
static void
scroll_window_to (double offset, void *data, long a, long b)
{
	(void) data;
	(void) b;
	Window *wp = window_by_id ((unsigned long) a);
	if (wp == NULL)
		return;
	double total, top;
	window_scroll_position (wp, &total, &top);
	window_scroll (wp, (long) (offset + 0.5) - (long) top);
}

/* The scroll bar of `wp', in the column right of its text. */
static void
draw_scroll_bar (size_t topline, size_t leftcol, Window *wp)
{
	if (wp->scroll_bar_width == 0 || wp->eheight == 0)
		return;
	int x = (int) (leftcol + wp->ewidth);
	TuiScroll s = { 0, 0, 0 };
	if (!shell_char_mode (wp->bp) && !preview_showing (wp->bp)) {
		window_scroll_position (wp, &s.total, &s.offset);
		s.viewport = (double) wp->eheight;
		/* Scrolled past the end: the thumb stays at the bottom. */
		if (s.offset > s.total - s.viewport && s.total > s.viewport)
			s.total = s.offset + s.viewport;
	}
	tui_paint_scrollbar ((int) topline, x, (int) wp->eheight, &s, TUI_KEY (SCROLL_KIND, wp->id), scroll_window_to,
			     NULL, (long) wp->id, 0);
}

/* The gutter of row `line', for the line `line_idx' of the buffer, or
   blank for the rows that continue a line. */
static void
draw_gutter (size_t line, size_t leftcol, Window *wp, size_t line_idx, size_t current_line, bool first)
{
	size_t gutter_width = wp->gutter_width;
	if (gutter_width == 0)
		return;
	bool abs_mode, rel_mode;
	get_line_number_modes (wp->bp, &abs_mode, &rel_mode);
	char gutter_str[32] = "";
	long num_to_show = 0;
	if (rel_mode && line_idx != current_line) {
		long diff = (long) line_idx - (long) current_line;
		num_to_show = diff < 0 ? -diff : diff;
	} else if (rel_mode || abs_mode)
		num_to_show = (long) line_idx + 1;
	if (first)
		snprintf (gutter_str, sizeof gutter_str, "%*ld ", (int) (gutter_width - 1), num_to_show);
	else
		snprintf (gutter_str, sizeof gutter_str, "%*s", (int) gutter_width, "");
	term_move (line, leftcol);
	term_apply_face (line_idx == current_line ? FACE_LINE_NUMBER_CURRENT_LINE : FACE_LINE_NUMBER);
	term_addstr (gutter_str);
}

static void
draw_text (Window *wp, size_t topline, size_t leftcol, const Region *r)
{
	Buffer *bp = wp->bp;
	bool truncate = !window_wraps (wp);

	/* The row at the top: `topdelta' rows above point's. */
	size_t o = buffer_start_of_line (bp, window_o (wp));
	size_t current_line = buffer_offset_to_line (bp, window_o (wp)), line_idx = current_line;
	size_t need = wp->topdelta, top_row = window_point_row (wp, NULL);
	if (need <= top_row)
		top_row -= need;
	else {
		need -= top_row;
		top_row = 0;
		while (need > 0 && o > 0) {
			o = buffer_prev_line (bp, o);
			line_idx--;
			size_t n = window_line_nrows (wp, o);
			top_row = n - MIN (n, need);
			need -= MIN (n, need);
		}
	}

	wp->screen_rows = xrealloc (wp->screen_rows, MAX (wp->eheight, 1) * sizeof (ScreenRow));
	wp->nscreen_rows = wp->eheight;
	bool links = (bp->compilation != NULL || bp->shell != NULL) && references_followable (bp);
	size_t y = topline, bottom = topline + wp->eheight;
	size_t point_lo = wp == cur_wp ? buffer_start_of_line (bp, window_o (wp)) : NOPOS;
	if (wp == cur_wp)
		cursor_found = false;
	bool cut = false;
	while (y < bottom && o != NOPOS && !cut) {
		DisplayRow *rows;
		size_t nrows = window_line_rows (wp, o, &rows);
		size_t len = buffer_line_len (bp, o);
		LineFaces lf;
		line_faces (wp, o, len, links, &lf);
		size_t k;
		for (k = top_row; k < nrows && y < bottom; k++, y++) {
			draw_gutter (y, leftcol, wp, line_idx, current_line, k == 0);
			draw_row (y, leftcol, wp, o, &rows[k], k + 1 == nrows, truncate, r, &lf);
			if (truncate && o == point_lo) {
				cursor_found = true;
				cursor_y = y;
				cursor_x = col;
			}
			ScreenRow sr = { o, rows[k].start, rows[k].end, rows[k].col, rows[k].col, k + 1 == nrows };
			if (truncate)
				sr = (ScreenRow) { o, 0, len, 0, wp->start_column, true };
			wp->screen_rows[y - topline] = sr;
		}
		line_faces_free (&lf);
		free (rows);
		top_row = 0;
		cut = k < nrows;
		if (!cut) {
			o = buffer_next_line (bp, o);
			line_idx++;
		}
	}
	for (; y < bottom; y++) {
		term_move (y, leftcol);
		term_apply_face (FACE_DEFAULT);
		add_spaces (wp->ewidth);
		wp->screen_rows[y - topline] = (ScreenRow) { NOPOS, 0, 0, 0, 0, true };
	}

	wp->all_displayed = !cut && (o == NOPOS || o >= buffer_length (bp));
}

static void
draw_window (size_t topline, size_t leftcol, Window *wp)
{
	Region r;
	bool highlight = calculate_highlight_region (wp, &r);

	if (preview_draw (wp, topline, leftcol) || shell_draw (wp, topline, leftcol))
		wp->all_displayed = true;
	else {
		find_parens (wp);
		draw_text (wp, topline, leftcol, highlight ? &r : NULL);
		paren_at[0] = paren_at[1] = NOPOS;
	}

	draw_scroll_bar (topline, leftcol, wp);
	if (wp->fheight > wp->eheight)
		draw_status_line (topline + wp->eheight, leftcol, wp);

	size_t sep_col = leftcol + wp->ewidth + wp->scroll_bar_width;
	if (wp->fwidth > sep_col - leftcol) {
		bool right_has_linenum = right_neighbor_has_linenum (leftcol + wp->fwidth, topline, wp->fheight);
		const char *bar = tui_unicode () ? "\xe2\x94\x82" : "|";
		bool hot = divider_hot (topline, sep_col);
		for (size_t row = topline; row < topline + wp->fheight; ++row) {
			term_move (row, sep_col);
			term_apply_face (FACE_VERTICAL_BORDER);
			term_addstr (right_has_linenum ? " " : "");
			term_apply_face (hot ? FACE_TUI_DIVIDER_HOVER : FACE_VERTICAL_BORDER);
			term_addstr (bar);
			term_apply_face (FACE_VERTICAL_BORDER);
			term_addstr (right_has_linenum ? "" : " ");
		}
		term_apply_face (FACE_DEFAULT);
	}
}

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

	tui_frame_begin ();
	layout_scroll_bar_width = get_variable_bool ("scroll-bar-mode") ? 1 : 0;
	update_windows_geometry (0, 0, term_width (), get_main_window_height ());
	add_divider_regions (root_node);

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

	if (cursor_col >= ew && !window_wraps (cur_wp)) {
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
	preview_begin_redisplay ();
	winnode_each_leaf (root_node, draw_leaf, NULL);

	term_redraw_cursor ();
	tui_draw_overlays ();
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
	if (preview_showing (cur_wp->bp)) {
		term_set_cursor_visible (false);
		term_move (cur_topline, cur_leftcol);
		return;
	}
	term_set_cursor_visible (true);
	if (cursor_found)
		term_move (cursor_y, cur_leftcol + cur_wp->gutter_width + cursor_x);
	else
		term_move (cur_topline + MIN (cur_wp->topdelta, cur_wp->eheight - 1), cur_leftcol + cur_wp->gutter_width + col);
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
