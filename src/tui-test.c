/* Tests of the widget toolkit

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

/* The toolkit draws into the screen grid, which these tests read back.
   What the toolkit needs from the rest of the editor is stubbed: no
   faces but the scroll bar's, a terminal with colours, and a mouse. */

#include "xzile.h"
#include "screen.h"
#include "tui.h"

#include <locale.h>

static int failures;

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			fprintf (stderr, "%s:%d: ", __FILE__, __LINE__); \
			fprintf (stderr, __VA_ARGS__);			\
			fputc ('\n', stderr);				\
			failures++;					\
		}							\
	} while (0)

/*--------------------------------------------------------------------------
 * Stubs.
 *--------------------------------------------------------------------------*/

static TerminalCapabilities caps = { true, true, true, true, 256, true, false };
static size_t mouse_y, mouse_x;
static Keystroke pushed;

const TerminalCapabilities *
term_get_capabilities (void)
{
	return &caps;
}

/* Only the scroll bar has colours, so that its blocks are drawn. */
bool
resolve_terminal_style (const char *face_name, TerminalStyle *style)
{
	memset (style, 0, sizeof *style);
	if (strcmp (face_name, FACE_TUI_SCROLLBAR_TRACK) == 0) {
		style->has_background = true;
		style->background = 236;
		return true;
	}
	if (strcmp (face_name, FACE_TUI_SCROLLBAR_THUMB) == 0) {
		style->has_foreground = true;
		style->foreground = 245;
		return true;
	}
	return false;
}

bool
term_mouse_position (size_t *y, size_t *x)
{
	*y = mouse_y;
	*x = mouse_x;
	return true;
}

void
pushkey (Keystroke key)
{
	pushed = key;
}

/*--------------------------------------------------------------------------
 * Helpers.
 *--------------------------------------------------------------------------*/

/* Row `y' of the screen, as text. */
static const char *
row_text (size_t y)
{
	static char buf[1024];
	size_t n = 0;
	for (size_t x = 0; x < scr_cols; x++) {
		const char *ch = scr_cells[y * scr_cols + x].ch;
		size_t len = strlen (ch);
		if (n + len < sizeof buf) {
			memcpy (buf + n, ch, len);
			n += len;
		}
	}
	buf[n] = '\0';
	return buf;
}

static void
frame (size_t rows, size_t cols)
{
	screen_resize (rows, cols);
	tui_frame_begin ();
}

static void
pause_ms (long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep (&ts, NULL);
}

static bool
click (size_t y, size_t x, Keystroke key)
{
	mouse_y = y;
	mouse_x = x;
	return tui_dispatch_mouse (key);
}

/*--------------------------------------------------------------------------
 * Layout.
 *--------------------------------------------------------------------------*/

/* A custom node records where it was laid out. */
static void
record (TuiRect r, void *data)
{
	*(TuiRect *) data = r;
}

static void
test_flex (void)
{
	/* 10 columns: 1 fixed, then 9 shared 1:2, the remainder going
	   so that the shares add up (3 and 6, not 3 and 3). */
	TuiRect a = { 0 }, b = { 0 }, c = { 0 };
	frame (3, 10);
	tui_render (tui_row (tui_custom (1, 1, record, &a),
			     tui_expanded (1, tui_custom (-1, 1, record, &b)),
			     tui_expanded (2, tui_custom (-1, 1, record, &c)), NULL),
		    (TuiRect) { 0, 0, 10, 3 });
	CHECK (a.x == 0 && a.w == 1, "fixed child at %d+%d", a.x, a.w);
	CHECK (b.x == 1 && b.w == 3, "first share at %d+%d, wanted 1+3", b.x, b.w);
	CHECK (c.x == 4 && c.w == 6, "second share at %d+%d, wanted 4+6", c.x, c.w);
	CHECK (b.h == 3, "a row given a tight height stretches its children: %d", b.h);

	/* 7 shared 1:1:1 is 2, 2, 3: every column used. */
	frame (1, 7);
	tui_render (tui_row (tui_expanded (1, tui_custom (-1, 1, record, &a)),
			     tui_expanded (1, tui_custom (-1, 1, record, &b)),
			     tui_expanded (1, tui_custom (-1, 1, record, &c)), NULL),
		    (TuiRect) { 0, 0, 7, 1 });
	CHECK (a.w + b.w + c.w == 7 && c.x + c.w == 7, "shares %d %d %d leave columns", a.w, b.w, c.w);

	/* A column: fixed rows first, the flexible one gets the rest, and
	   gaps are left between. */
	frame (10, 4);
	tui_render (tui_gap (tui_column (tui_custom (-1, 2, record, &a),
					 tui_expanded (1, tui_custom (-1, -1, record, &b)),
					 tui_custom (-1, 1, record, &c), NULL), 1),
		    (TuiRect) { 0, 0, 4, 10 });
	CHECK (a.y == 0 && a.h == 2, "top at %d+%d", a.y, a.h);
	CHECK (b.y == 3 && b.h == 5, "middle at %d+%d, wanted 3+5", b.y, b.h);
	CHECK (c.y == 9 && c.h == 1, "bottom at %d+%d, wanted 9+1", c.y, c.h);
	CHECK (a.w == 4 && b.w == 4, "a column stretches its children across");

	/* Too little room: the fixed children are cut, the flexible one
	   gets nothing, and nothing is laid out off the area. */
	frame (3, 4);
	a = b = c = (TuiRect) { -1, -1, -1, -1 };
	tui_render (tui_column (tui_custom (-1, 2, record, &a), tui_custom (-1, 2, record, &b),
				tui_expanded (1, tui_custom (-1, -1, record, &c)), NULL),
		    (TuiRect) { 0, 0, 4, 3 });
	CHECK (a.h == 2 && b.y == 2 && b.h == 1, "cut to %d, %d+%d", a.h, b.y, b.h);
	CHECK (c.h == -1, "a flexible child with no room was drawn %d high", c.h);
}

static void
test_text (void)
{
	frame (2, 8);
	tui_render (tui_text ("abcdefghij", NULL), (TuiRect) { 0, 0, 8, 1 });
	CHECK (strcmp (row_text (0), "abcdefg…") == 0, "cut text is \"%s\"", row_text (0));

	/* A wide character that would straddle the ellipsis leaves a space
	   before it rather than half a character. */
	frame (1, 6);
	tui_render (tui_text ("abcd中文", NULL), (TuiRect) { 0, 0, 6, 1 });
	CHECK (strcmp (row_text (0), "abcd …") == 0, "cut wide text is \"%s\"", row_text (0));

	/* What fits exactly is not cut. */
	frame (1, 6);
	tui_render (tui_text ("abc中", NULL), (TuiRect) { 0, 0, 6, 1 });
	CHECK (strncmp (row_text (0), "abc中", strlen ("abc中")) == 0, "exact text is \"%s\"", row_text (0));

	/* Right aligned in the width given. */
	frame (1, 6);
	tui_render (tui_text_fill (tui_text ("ab", NULL), 2), (TuiRect) { 0, 0, 6, 1 });
	CHECK (strcmp (row_text (0), "    ab") == 0, "aligned text is \"%s\"", row_text (0));
}

static void
test_panel (void)
{
	frame (4, 20);
	TuiPanel p = { TUI_BORDER_ROUNDED, NULL, NULL, "Title", "3 / 9", NULL, 0, 0, 0 };
	TuiRect inner = { 0 };
	tui_render (tui_panel (&p, tui_custom (-1, 2, record, &inner)), (TuiRect) { 0, 0, 20, 4 });
	CHECK (strcmp (row_text (0), "╭─ Title ── 3 / 9 ─╮") == 0, "top border is \"%s\"", row_text (0));
	CHECK (strncmp (row_text (3), "╰", strlen ("╰")) == 0, "bottom border is \"%s\"", row_text (3));
	CHECK (inner.x == 1 && inner.y == 1 && inner.w == 18 && inner.h == 2, "inside at %d,%d %dx%d",
	       inner.x, inner.y, inner.w, inner.h);
}

/*--------------------------------------------------------------------------
 * Scroll bars.
 *--------------------------------------------------------------------------*/

static void
test_scroll_thumb (void)
{
	double start, size;
	TuiScroll s = { 10, 10, 0 };
	CHECK (!tui_scroll_thumb (&s, 10, &start, &size), "no thumb when all is shown");
	s = (TuiScroll) { 11, 10, 0 };
	CHECK (tui_scroll_thumb (&s, 10, &start, &size) && start == 0, "a thumb at the top for one more line");
	CHECK (size > 9 && size < 10, "thumb of %g for 10 of 11", size);
	s.offset = 1;
	CHECK (tui_scroll_thumb (&s, 10, &start, &size) && start + size == 10, "thumb ends at %g at the bottom",
	       start + size);
	/* Long content: the thumb is at least a cell. */
	s = (TuiScroll) { 100000, 10, 50000 };
	CHECK (tui_scroll_thumb (&s, 10, &start, &size) && size == 1, "thumb of %g for long content", size);
	CHECK (start > 4.4 && start < 4.6, "thumb at %g half way", start);

	/* Dragging: the grabbed point follows the pointer. */
	s = (TuiScroll) { 40, 10, 0 };
	tui_scroll_thumb (&s, 10, &start, &size);	/* size 2.5, 7.5 to travel */
	double off = tui_scroll_drag_offset (&s, 10, 1.0 + 7.5 / 2, 1.0);
	CHECK (off > 14.9 && off < 15.1, "drag half way gives %g, wanted 15", off);
	CHECK (tui_scroll_drag_offset (&s, 10, -5, 1.0) == 0, "dragged above the track");
	CHECK (tui_scroll_drag_offset (&s, 10, 50, 1.0) == 30, "dragged below the track");
}

static double scrolled_to = -1;

static void
scroll_to (double offset, void *data, long a, long b)
{
	(void) data;
	(void) a;
	(void) b;
	scrolled_to = offset;
}

static void
test_scrollbar_paint_and_mouse (void)
{
	/* 8 rows showing 8 of 32 from 12: the thumb is 2 cells, starting
	   at 3: whole cells 3 and 4, no partial ones. */
	frame (8, 1);
	TuiScroll s = { 32, 8, 12 };
	tui_paint_scrollbar (0, 0, 8, &s, 99, scroll_to, NULL, 0, 0);
	for (int y = 0; y < 8; y++) {
		const Cell *c = &scr_cells[y];
		bool thumb = y == 3 || y == 4;
		CHECK (strcmp (c->ch, " ") == 0, "row %d shows \"%s\" for a whole cell", y, c->ch);
		CHECK (c->attr.bg == (thumb ? 245 : 236), "row %d has background %d", y, c->attr.bg);
	}

	/* A thumb starting half way through a cell: a lower half block
	   there, and a reversed one where it ends. */
	frame (8, 1);
	s = (TuiScroll) { 32, 8, 2 };	/* thumb 2 cells from 0.5 */
	tui_paint_scrollbar (0, 0, 8, &s, 99, scroll_to, NULL, 0, 0);
	CHECK (strcmp (scr_cells[0].ch, "▄") == 0 && !(scr_cells[0].attr.flags & ATTR_REVERSE),
	       "start cell is \"%s\"", scr_cells[0].ch);
	CHECK (strcmp (scr_cells[2].ch, "▄") == 0 && (scr_cells[2].attr.flags & ATTR_REVERSE),
	       "end cell is \"%s\" reversed %d", scr_cells[2].ch, !!(scr_cells[2].attr.flags & ATTR_REVERSE));

	/* Clicking the track below the thumb pages down by a screen less a
	   line; dragging the thumb scrolls. */
	scrolled_to = -1;
	CHECK (click (6, 0, KBD_MOUSE_1), "track click taken");
	CHECK (scrolled_to == 2 + 7, "paged to %g", scrolled_to);
	click (6, 0, KBD_UP_MOUSE_1);
	frame (8, 1);
	s = (TuiScroll) { 32, 8, 0 };	/* thumb 0-2 */
	tui_paint_scrollbar (0, 0, 8, &s, 99, scroll_to, NULL, 0, 0);
	scrolled_to = -1;
	CHECK (click (1, 0, KBD_MOUSE_1) && scrolled_to < 0, "pressing the thumb does not jump");
	click (4, 0, KBD_DRAG_MOUSE_1);
	/* Grabbed at 1.5 of a thumb of 2 on a track of 8: at 4.5 its start
	   is at 3 of 6, half way. */
	CHECK (scrolled_to > 11.9 && scrolled_to < 12.1, "dragged to %g, wanted 12", scrolled_to);
	CHECK (tui_captured (), "the drag keeps the press");
	click (4, 0, KBD_UP_MOUSE_1);
	CHECK (!tui_captured (), "the release ends it");
}

/*--------------------------------------------------------------------------
 * Regions.
 *--------------------------------------------------------------------------*/

typedef struct {
	int presses, clicks, drags, releases, wheel;
	bool moved;
} Counts;

static bool
count_event (const TuiEvent *ev, void *data)
{
	Counts *c = data;
	switch (ev->kind) {
	case TUI_PRESS:
		c->presses++;
		c->clicks = ev->clicks;
		break;
	case TUI_DRAG:
		c->drags++;
		break;
	case TUI_RELEASE:
		c->releases++;
		c->moved = ev->moved;
		break;
	case TUI_WHEEL:
		c->wheel += ev->wheel;
		break;
	}
	return true;
}

static bool
decline (const TuiEvent *ev, void *data)
{
	(void) ev;
	(*(int *) data)++;
	return false;
}

static void
test_regions (void)
{
	Counts below = { 0 }, above = { 0 };
	frame (5, 10);
	TuiRegionSpec sb = { 1, count_event, &below, 0, 0, 0, TUI_CURSOR_DEFAULT, false };
	TuiRegionSpec sa = { 2, count_event, &above, 0, 0, 0, TUI_CURSOR_POINTER, false };
	tui_region ((TuiRect) { 0, 0, 10, 5 }, &sb);
	tui_region ((TuiRect) { 2, 1, 3, 2 }, &sa);

	/* The region drawn last is on top. */
	CHECK (click (1, 3, KBD_MOUSE_1) && above.presses == 1 && below.presses == 0, "press went below");
	/* Drags and the release go to the region pressed, even outside it. */
	click (4, 9, KBD_DRAG_MOUSE_1);
	click (4, 9, KBD_UP_MOUSE_1);
	CHECK (above.drags == 1 && above.releases == 1 && above.moved && below.drags == 0, "capture lost");

	/* Hover. */
	CHECK (tui_pointer_moved (true, 2, 4) || tui_hovered (2), "hovering the top region");
	CHECK (tui_hovered (2) && !tui_hovered (1) && tui_cursor () == TUI_CURSOR_POINTER, "wrong hover");
	CHECK (tui_pointer_moved (true, 4, 0) && tui_hovered (1), "hover moved to the region below");
	CHECK (!tui_pointer_moved (true, 4, 1), "moving within a region changes nothing");
	CHECK (tui_pointer_moved (false, 0, 0) && !tui_hovered (1), "leaving the screen");

	/* Double clicks: the same region, soon enough. */
	click (1, 3, KBD_MOUSE_1);
	click (1, 3, KBD_UP_MOUSE_1);
	pause_ms (500);
	click (1, 3, KBD_MOUSE_1);
	click (1, 3, KBD_UP_MOUSE_1);
	CHECK (above.clicks == 1, "slow clicks counted %d", above.clicks);
		click (1, 3, KBD_MOUSE_1);
	click (1, 3, KBD_UP_MOUSE_1);
	CHECK (above.clicks == 2, "a double click counted %d", above.clicks);
		click (4, 9, KBD_MOUSE_1);	/* Another region. */
	click (4, 9, KBD_UP_MOUSE_1);
		click (1, 3, KBD_MOUSE_1);
	click (1, 3, KBD_UP_MOUSE_1);
	CHECK (above.clicks == 1, "clicks in two regions counted %d", above.clicks);

	/* A press nothing takes goes through, unless the region is opaque;
	   a wheel event likewise. */
	int declined = 0;
	frame (5, 10);
	TuiRegionSpec sd = { 3, decline, &declined, 0, 0, 0, TUI_CURSOR_DEFAULT, false };
	tui_region ((TuiRect) { 0, 0, 10, 5 }, &sb);
	tui_region ((TuiRect) { 0, 0, 10, 5 }, &sd);
	int before = below.presses;
	CHECK (click (0, 0, KBD_MOUSE_1) && declined == 1 && below.presses == before + 1, "press not passed on");
	click (0, 0, KBD_UP_MOUSE_1);
	CHECK (click (0, 0, KBD_WHEEL_DOWN) && below.wheel == 1, "wheel not passed on");
	frame (5, 10);
	sd.opaque = true;
	tui_region ((TuiRect) { 0, 0, 10, 5 }, &sb);
	tui_region ((TuiRect) { 0, 0, 10, 5 }, &sd);
	before = below.presses;
	CHECK (click (0, 0, KBD_MOUSE_1) && below.presses == before, "an opaque region let a press through");
	CHECK (!tui_captured (), "a declined press is not captured");

	/* Outside every region, nothing is taken. */
	frame (5, 10);
	CHECK (!click (0, 0, KBD_MOUSE_1) && !click (0, 0, KBD_DRAG_MOUSE_1), "a press on nothing was taken");

	/* A copied region outlives the frame it was drawn in. */
	frame (5, 10);
	Counts *temp = xzalloc (sizeof (Counts));
	TuiRegionSpec sc = { 4, count_event, temp, sizeof (Counts), 0, 0, TUI_CURSOR_DEFAULT, false };
	tui_region ((TuiRect) { 0, 0, 10, 5 }, &sc);
	click (0, 0, KBD_MOUSE_1);
	memset (temp, 0xff, sizeof (Counts));
	free (temp);
	frame (5, 10);
	CHECK (click (1, 1, KBD_DRAG_MOUSE_1), "drag after a new frame");
	click (1, 1, KBD_UP_MOUSE_1);

	/* A hint in a panel's bottom edge pushes its key. */
	frame (3, 30);
	TuiHint hint = { "RET", "open", KBD_RET };
	TuiPanel p = { TUI_BORDER_ROUNDED, NULL, NULL, NULL, NULL, &hint, 1, 0, 0 };
	tui_render (tui_panel (&p, NULL), (TuiRect) { 0, 0, 30, 3 });
	CHECK (strstr (row_text (2), "RET open") != NULL, "hint row is \"%s\"", row_text (2));
	pushed = 0;
	click (2, 4, KBD_MOUSE_1);
	click (2, 4, KBD_UP_MOUSE_1);
	CHECK (pushed == KBD_RET, "clicking the hint pushed %u", pushed);
}

int
main (void)
{
	setlocale (LC_ALL, "C.UTF-8");
	test_flex ();
	test_text ();
	test_panel ();
	test_scroll_thumb ();
	test_scrollbar_paint_and_mouse ();
	test_regions ();
	if (failures > 0) {
		fprintf (stderr, "%d toolkit test(s) failed\n", failures);
		return 1;
	}
	printf ("toolkit tests passed\n");
	return 0;
}
