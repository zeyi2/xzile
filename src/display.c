/* Lines of text on the screen: wrapping, and moving by screen lines

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

/* Unless `truncate-lines' is set, a line of text wider than its window
   continues on the screen lines below: it is shown as screen rows, each
   a run of its bytes starting at some display column of the line.
   Columns are counted from the start of the line, as everywhere else,
   so tabs are as wide wherever the line breaks.

   A window's `topdelta' counts screen rows: those from the top of the
   window to the row showing point. Without wrapping a row is a line,
   and this is what it always was. */

#include "xzile.h"

/*--------------------------------------------------------------------------
 * Breaking a line.
 *--------------------------------------------------------------------------*/

static bool
is_wide (uint32_t ch)
{
	return utf8_char_display_width (ch) > 1;
}

size_t
display_char_width (uint32_t ch, size_t col, size_t tab_width)
{
	if (ch == '\t')
		return tab_width - col % tab_width;
	if (ch < 0x20 || ch == 0x7f)
		return 2;
	if (ch >= 0x80 && ch < 0xa0)
		return 4;
	return (size_t) utf8_char_display_width (ch);
}

size_t
display_wrap (const char *s, size_t len, size_t width, size_t tab_width, bool word_wrap,
	      DisplayRow *rows, size_t max)
{
	size_t n = 0;
	size_t start = 0, start_col = 0;	/* The row being filled. */
	size_t brk = NOPOS, brk_col = 0;	/* Where it may break, by a word. */
	size_t col = 0;
	width = MAX (width, 1);
	tab_width = MAX (tab_width, 1);

#define EMIT(e, c)							\
	do {								\
		if (n < max) {						\
			rows[n].start = start;				\
			rows[n].end = (e);				\
			rows[n].col = start_col;			\
		}						\
		n++;							\
		start = (e);						\
		start_col = (c);					\
		brk = NOPOS;						\
	} while (0)

	for (size_t i = 0; i < len;) {
		size_t cl;
		uint32_t ch = utf8_decode (s, len, i, &cl);
		cl = cl ? cl : 1;
		size_t w = display_char_width (ch, col, tab_width);
		bool space = ch == ' ' || ch == '\t';

		if (col + w > start_col + width && i > start) {
			if (space) {
				/* A space that does not fit ends the row, unseen. */
				EMIT (i + cl, col + w);
				col += w;
				i += cl;
				continue;
			}
			if (word_wrap && brk != NOPOS && brk > start)
				EMIT (brk, brk_col);
			else
				EMIT (i, col);
			/* What followed the break may still not fit. */
			continue;
		}
		col += w;
		i += cl;
		/* Words break after spaces, and anywhere around wide
		   characters. */
		if (space || is_wide (ch)) {
			brk = i;
			brk_col = col;
		} else if (i < len) {
			size_t nl;
			uint32_t next = utf8_decode (s, len, i, &nl);
			if (is_wide (next)) {
				brk = i;
				brk_col = col;
			}
		}
	}
	/* The last row. When the line fills it exactly, the cursor at its
	   end shows on the last column rather than on a row of its own. */
	EMIT (len, col);
#undef EMIT
	return n;
}

/*--------------------------------------------------------------------------
 * The rows of a line in a window.
 *--------------------------------------------------------------------------*/

static bool
variable_true (Buffer *bp, const char *name)
{
	const char *v = buffer_get_variable (bp, name);
	return v != NULL && strcmp (v, "nil") != 0;
}

bool
window_wraps (Window *wp)
{
	if (shell_char_mode (wp->bp) || preview_showing (wp->bp))
		return false;
	const char *v = buffer_get_variable (wp->bp, "truncate-lines");
	return v == NULL || strcmp (v, "nil") == 0;
}

size_t
window_text_width (const Window *wp)
{
	return wp->gutter_width < wp->ewidth ? wp->ewidth - wp->gutter_width : 1;
}

/* The line at `lo', copied. */
static char *
line_text (Buffer *bp, size_t lo, size_t *len)
{
	*len = buffer_line_len (bp, lo);
	char *s = xmalloc (*len + 1);
	for (size_t i = 0; i < *len; i++)
		s[i] = buffer_get_char (bp, lo + i);
	s[*len] = '\0';
	return s;
}

size_t
window_line_rows (Window *wp, size_t lo, DisplayRow **rows)
{
	Buffer *bp = wp->bp;
	size_t len = buffer_line_len (bp, lo);
	if (!window_wraps (wp)) {
		*rows = xmalloc (sizeof (DisplayRow));
		(*rows)[0] = (DisplayRow) { 0, len, wp->start_column };
		return 1;
	}
	size_t width = window_text_width (wp);
	size_t n = markdown_line_rows (wp, lo, width, rows);
	if (n > 0)
		return n;
	char *s = line_text (bp, lo, &len);
	bool word_wrap = variable_true (bp, "word-wrap");
	size_t tab = buffer_tab_width (bp);
	n = display_wrap (s, len, width, tab, word_wrap, NULL, 0);
	*rows = xmalloc (n * sizeof (DisplayRow));
	display_wrap (s, len, width, tab, word_wrap, *rows, n);
	free (s);
	return n;
}

size_t
window_line_nrows (Window *wp, size_t lo)
{
	DisplayRow *rows;
	size_t n = window_line_rows (wp, lo, &rows);
	free (rows);
	return n;
}

size_t
display_row_of (const DisplayRow *rows, size_t n, size_t i)
{
	size_t k = 0;
	while (k + 1 < n && rows[k + 1].start <= i && rows[k + 1].start > rows[k].start)
		k++;
	return k;
}

size_t
window_point_row (Window *wp, size_t *nrows)
{
	Buffer *bp = wp->bp;
	size_t o = window_o (wp), lo = buffer_start_of_line (bp, o);
	DisplayRow *rows;
	size_t n = window_line_rows (wp, lo, &rows);
	size_t k = display_row_of (rows, n, o - lo);
	free (rows);
	if (nrows != NULL)
		*nrows = n;
	return k;
}

size_t
window_rows_above (Window *wp, size_t lo, size_t k, size_t limit)
{
	size_t rows = k;
	while (rows < limit && lo > 0) {
		lo = buffer_prev_line (wp->bp, lo);
		if (lo == NOPOS)
			break;
		rows += window_line_nrows (wp, lo);
	}
	return MIN (rows, limit);
}

size_t
window_point_rows_above (Window *wp, size_t limit)
{
	size_t lo = buffer_start_of_line (wp->bp, window_o (wp));
	return window_rows_above (wp, lo, window_point_row (wp, NULL), limit);
}

/*--------------------------------------------------------------------------
 * Moving point by screen rows.
 *--------------------------------------------------------------------------*/

/* Put point in row `row' of the line at `lo', at column `x' of the row
   (or as near as it can be), not after the row's end unless the row is
   the line's last. */
static void
goto_row_x (Buffer *bp, size_t lo, const DisplayRow *row, bool last, size_t x)
{
	size_t t = buffer_tab_width (bp);
	size_t len = buffer_line_len (bp, lo);
	size_t end = last ? len : row->end;
	size_t i = row->start, col = row->col, goal = row->col + x, prev = row->start;
	while (i < end) {
		size_t cl;
		uint32_t ch = buffer_get_utf8_char (bp, lo + i, &cl);
		size_t w = display_char_width (ch, col, t);
		if (col >= goal || col + w > goal)
			break;
		col += w;
		prev = i;
		i += cl ? cl : 1;
	}
	/* Not past the row: its end is the start of the next. */
	if (!last && i >= row->end && row->end > row->start)
		i = prev;
	buffer_goto_offset (bp, lo + i);
}

/* The column on the screen that point keeps as it moves by rows. */
static Buffer *goal_bp;
static size_t goal_pt = NOPOS, goal_x;

bool
window_move_rows (Window *wp, long n)
{
	Buffer *bp = wp->bp;
	size_t o = buffer_pt (bp), lo = buffer_start_of_line (bp, o);
	DisplayRow *rows;
	size_t nr = window_line_rows (wp, lo, &rows);
	size_t k = display_row_of (rows, nr, o - lo);
	if (goal_bp != bp || goal_pt != o)
		goal_x = buffer_calculate_goalc (bp, o) - MIN (rows[k].col, buffer_calculate_goalc (bp, o));

	bool ok = true;
	for (; n != 0; n += n > 0 ? -1 : 1) {
		if (n > 0 && k + 1 < nr)
			k++;
		else if (n < 0 && k > 0)
			k--;
		else {
			size_t next = n > 0 ? buffer_next_line (bp, lo) : buffer_prev_line (bp, lo);
			if (next == NOPOS) {
				ok = false;
				break;
			}
			lo = next;
			free (rows);
			nr = window_line_rows (wp, lo, &rows);
			k = n > 0 ? 0 : nr - 1;
		}
	}
	size_t goal = goal_x;
	goto_row_x (bp, lo, &rows[k], k + 1 == nr, goal_x);
	free (rows);
	goal_bp = bp;
	goal_pt = buffer_pt (bp);
	goal_x = goal;
	thisflag |= FLAG_NEED_RESYNC;
	return ok;
}

bool
window_moves_visually (Window *wp)
{
	return window_wraps (wp) && get_variable_bool ("line-move-visual");
}

/*--------------------------------------------------------------------------
 * Keeping point in view.
 *--------------------------------------------------------------------------*/

/* Rows from the row showing point at line `lo', row `k', down to row
   `k2' of the line at `lo2' after it (negative if before), but no more
   than `limit' either way. */
static long
rows_between (Window *wp, size_t lo, size_t k, size_t lo2, size_t k2, size_t limit)
{
	if (lo == lo2)
		return (long) k2 - (long) k;
	bool forward = lo2 > lo;
	size_t a = forward ? lo : lo2, ka = forward ? k : k2;
	size_t b = forward ? lo2 : lo, kb = forward ? k2 : k;
	long rows = -(long) ka;
	for (size_t l = a; l != NOPOS && l < b && (size_t) labs (rows) <= limit; l = buffer_next_line (wp->bp, l))
		rows += (long) window_line_nrows (wp, l);
	rows += (long) kb;
	return forward ? rows : -rows;
}

void
window_resync (Window *wp)
{
	Buffer *bp = wp->bp;
	size_t o = window_o (wp), n = buffer_offset_to_line (bp, o);
	size_t nrows, k = window_point_row (wp, &nrows);
	if (n == wp->lastpointn && k == wp->lastpointrow)
		return;

	/* How far point moved on the screen, when it stayed near. */
	long delta;
	long lines = (long) n - (long) wp->lastpointn;
	if (labs (lines) > (long) wp->eheight * 2 + 2 || wp->lastpointn > buffer_offset_to_line (bp, buffer_length (bp)))
		delta = LONG_MAX;
	else {
		size_t lo = buffer_start_of_line (bp, o), old = lo;
		for (long i = lines; i > 0 && old != NOPOS; i--)
			old = buffer_prev_line (bp, old);
		for (long i = lines; i < 0 && old != NOPOS; i++)
			old = buffer_next_line (bp, old);
		if (old == NOPOS)
			delta = LONG_MAX;
		else {
			size_t old_k = MIN (wp->lastpointrow, window_line_nrows (wp, old) - 1);
			delta = rows_between (wp, old, old_k, lo, k, wp->eheight * 2);
		}
	}

	if (delta != LONG_MAX && delta != 0 &&
	    ((delta > 0 && wp->topdelta + (size_t) delta < wp->eheight) ||
	     (delta < 0 && wp->topdelta >= (size_t) (-delta))))
		wp->topdelta += (size_t) delta;
	else if (delta != 0)
		recenter (wp);
	wp->lastpointn = n;
	wp->lastpointrow = k;
}

void
recenter (Window *wp)
{
	wp->topdelta = window_point_rows_above (wp, wp->eheight / 2);
	wp->lastpointn = buffer_offset_to_line (wp->bp, window_o (wp));
	wp->lastpointrow = window_point_row (wp, NULL);
}

void
window_set_top (Window *wp, size_t topdelta)
{
	wp->topdelta = MIN (window_point_rows_above (wp, topdelta), wp->eheight > 0 ? wp->eheight - 1 : 0);
	wp->lastpointn = buffer_offset_to_line (wp->bp, window_o (wp));
	wp->lastpointrow = window_point_row (wp, NULL);
}

bool
window_top_visible (Window *wp)
{
	return window_point_rows_above (wp, wp->topdelta + 1) == wp->topdelta;
}

/* Scroll `wp' by `n' rows, forward if positive, moving point only to
   keep it on the screen. */
void
window_scroll (Window *wp, long n)
{
	Window *old = cur_wp;
	if (wp != cur_wp)
		window_set_current (wp);

	if (!shell_char_mode (cur_bp) && n != 0) {
		size_t o = buffer_pt (cur_bp), lo = buffer_start_of_line (cur_bp, o);
		size_t nrows, k = window_point_row (wp, &nrows);
		if (n > 0) {
			/* No further than the last row at the top. */
			size_t ahead = nrows - 1 - k;
			for (size_t l = buffer_next_line (cur_bp, lo); l != NOPOS && ahead < (size_t) n + wp->topdelta;
			     l = buffer_next_line (cur_bp, l))
				ahead += window_line_nrows (wp, l);
			size_t can = MIN ((size_t) n, wp->topdelta + ahead);
			if (wp->topdelta >= can)
				wp->topdelta -= can;
			else {
				size_t move = can - wp->topdelta;
				if (window_wraps (wp))
					window_move_rows (wp, (long) move);
				else
					buffer_move_line (cur_bp, (long) move);
				wp->topdelta = 0;
			}
		} else {
			size_t above = window_rows_above (wp, lo, k, wp->topdelta + (size_t) -n) - wp->topdelta;
			wp->topdelta += MIN ((size_t) -n, above);
			if (wp->eheight > 0 && wp->topdelta >= wp->eheight) {
				size_t move = wp->topdelta - (wp->eheight - 1);
				if (window_wraps (wp))
					window_move_rows (wp, -(long) move);
				else
					buffer_move_line (cur_bp, -(long) move);
				wp->topdelta = wp->eheight - 1;
			}
		}
		wp->lastpointn = buffer_offset_to_line (cur_bp, buffer_pt (cur_bp));
		wp->lastpointrow = window_point_row (wp, NULL);
	}

	if (old != cur_wp)
		window_set_current (old);
}

/*--------------------------------------------------------------------------
 * The rows of the whole buffer, for the scroll bar.
 *--------------------------------------------------------------------------*/

/* Counting rows means breaking every line, so it is done when the
   buffer or the width changes, and not at all for long buffers. */
#define MAX_COUNTED_LINES 50000

typedef struct {
	unsigned long win;
	const Buffer *bp;
	size_t changes, width, tab;
	bool word_wrap;
	size_t nlines;
	size_t *before;		/* Rows before each line, and the total. */
} RowCount;

static RowCount counts[8];
static size_t next_count;

static RowCount *
row_count (Window *wp, size_t nlines)
{
	Buffer *bp = wp->bp;
	size_t width = window_text_width (wp), tab = buffer_tab_width (bp);
	bool word_wrap = variable_true (bp, "word-wrap");
	for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
		RowCount *c = &counts[i];
		if (c->win == wp->id && c->bp == bp && c->changes == bp->changes && c->width == width &&
		    c->tab == tab && c->word_wrap == word_wrap && c->nlines == nlines)
			return c;
	}
	RowCount *c = &counts[next_count++ % (sizeof counts / sizeof counts[0])];
	free (c->before);
	*c = (RowCount) { wp->id, bp, bp->changes, width, tab, word_wrap, nlines, NULL };
	c->before = xmalloc ((nlines + 1) * sizeof (size_t));
	size_t total = 0, l = 0;
	for (size_t lo = 0; lo != NOPOS && l < nlines; lo = buffer_next_line (bp, lo), l++) {
		c->before[l] = total;
		total += window_line_nrows (wp, lo);
	}
	for (; l <= nlines; l++)
		c->before[l] = total;
	return c;
}

bool
window_scroll_position (Window *wp, double *total, double *top)
{
	Buffer *bp = wp->bp;
	size_t o = window_o (wp), line = buffer_offset_to_line (bp, o);
	size_t nlines = buffer_offset_to_line (bp, buffer_length (bp)) + 1;
	size_t above = window_point_rows_above (wp, wp->topdelta);
	if (!window_wraps (wp) || nlines > MAX_COUNTED_LINES || bp->markdown != NULL) {
		/* Lines stand for rows. */
		*total = (double) nlines;
		*top = (double) (line - MIN (line, wp->topdelta));
		return !window_wraps (wp);
	}
	RowCount *c = row_count (wp, nlines);
	size_t point_row = c->before[line] + window_point_row (wp, NULL);
	*total = (double) c->before[nlines];
	*top = (double) (point_row - MIN (point_row, above));
	return true;
}

/*--------------------------------------------------------------------------
 * The screen as last shown, for the mouse.
 *--------------------------------------------------------------------------*/

const ScreenRow *
window_screen_row (Window *wp, size_t y)
{
	if (y >= wp->nscreen_rows || wp->screen_rows[y].lo == NOPOS ||
	    wp->screen_rows[y].lo > buffer_length (wp->bp))
		return NULL;
	return &wp->screen_rows[y];
}

size_t
window_cell_offset (Window *wp, size_t y, size_t x, bool *on_text)
{
	Buffer *bp = wp->bp;
	*on_text = false;
	const ScreenRow *row = window_screen_row (wp, y);
	if (row == NULL)
		return NOPOS;
	size_t o;
	/* A rendered Markdown row: its start and end are columns. */
	size_t mcol = row->left + x, mlast = row->end > row->start ? row->end - 1 : row->start;
	if (markdown_cell_offset (wp, row->lo, row->last ? mcol : MIN (mcol, mlast), &o, on_text)) {
		*on_text = *on_text && (row->last || mcol <= mlast);
		return o;
	}

	size_t t = buffer_tab_width (bp), len = buffer_line_len (bp, row->lo);
	size_t end = row->last ? len : MIN (row->end, len), goal = row->left + x;
	size_t i = row->start, col = row->col, prev = i;
	while (i < end) {
		size_t cl;
		uint32_t ch = buffer_get_utf8_char (bp, row->lo + i, &cl);
		size_t w = display_char_width (ch, col, t);
		if (col + w > goal) {
			*on_text = true;
			return row->lo + i;
		}
		col += w;
		prev = i;
		i += cl ? cl : 1;
	}
	/* After the text of a row that continues: its last character. */
	if (!row->last && i > row->start)
		i = prev;
	return row->lo + i;
}

size_t
window_row_column (Window *wp, size_t y, size_t i)
{
	const ScreenRow *row = window_screen_row (wp, y);
	if (row == NULL)
		return 0;
	Buffer *bp = wp->bp;
	size_t t = buffer_tab_width (bp), col = row->col;
	i = MAX (i, row->start);
	for (size_t j = row->start; j < i && j < row->end;) {
		size_t cl;
		uint32_t ch = buffer_get_utf8_char (bp, row->lo + j, &cl);
		col += display_char_width (ch, col, t);
		j += cl ? cl : 1;
	}
	col = col > row->left ? col - row->left : 0;
	return MIN (col, window_text_width (wp));
}
