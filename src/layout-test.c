/* Tests for resizing windows by their dividers (layout.c).

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

#include "xzile.h"

/* An 80x24 screen less the minibuffer. */
#define W 80
#define H 23

#define CHECK(cond) check ((cond), #cond, __LINE__)

static void
check (bool ok, const char *what, int line)
{
	if (!ok) {
		printf ("layout test failed at line %d: %s\n", line, what);
		exit (EXIT_FAILURE);
	}
}

static Window *
first_window (void)
{
	Window *wp = xzalloc (sizeof (Window));
	root_node = leaf_node_new (wp);
	return wp;
}

/* Split `wp', putting a new window below or right of it. */
static Window *
split (Window *wp, enum SplitKind kind, double ratio)
{
	Window *nw = xzalloc (sizeof (Window));
	replace_leaf_with_split (find_leaf_for (wp), kind, wp, nw);
	find_leaf_for (wp)->parent->ratio = ratio;
	return nw;
}

static WinNode *
parent_of (Window *wp)
{
	return find_leaf_for (wp)->parent;
}

/* Lay out the screen, checking that no window is too small. */
static void
layout (void)
{
	CHECK (update_windows_geometry (0, 0, W, H) == NULL);
}

static void
no_dividers (void)
{
	first_window ();
	layout ();
	for (size_t y = 0; y <= H; y++)
		for (size_t x = 0; x < W; x++)
			CHECK (layout_divider_at (y, x) == NULL);
}

static void
rows (void)
{
	Window *a = first_window (), *b = split (a, SPLIT_ROWS, 0.5);
	WinNode *s = parent_of (a);
	layout ();
	CHECK (a->fheight == 11);
	/* A's status line is the divider; B's, at the bottom, is not. */
	CHECK (layout_divider_at (10, 0) == s);
	CHECK (layout_divider_at (10, W - 1) == s);
	CHECK (layout_divider_at (9, 0) == NULL);
	CHECK (layout_divider_at (11, 0) == NULL);
	CHECK (layout_divider_at (H - 1, 0) == NULL);
	CHECK (layout_divider_at (H, 0) == NULL);

	unsigned long gen = layout_generation ();
	CHECK (layout_resize_split (s, 5));
	layout ();
	CHECK (a->fheight == 5 && b->fheight == H - 5);
	CHECK (layout_divider_at (4, 0) == s);
	for (size_t n = 2; n <= H - 2; n++) {
		layout_resize_split (s, n);
		layout ();
		CHECK (a->fheight == n);
	}
	CHECK (layout_resize_split (s, 0));
	layout ();
	CHECK (a->fheight == 2);
	CHECK (layout_resize_split (s, 100));
	layout ();
	CHECK (b->fheight == 2);
	CHECK (!layout_resize_split (s, 100));
	CHECK (layout_generation () == gen);
}

static void
cols (void)
{
	Window *a = first_window (), *b = split (a, SPLIT_COLS, 0.5);
	WinNode *s = parent_of (a);
	layout ();
	/* The separator is the two columns at the right of A. */
	CHECK (layout_divider_at (0, 38) == s);
	CHECK (layout_divider_at (0, 39) == s);
	CHECK (layout_divider_at (H - 1, 39) == s);
	CHECK (layout_divider_at (0, 37) == NULL);
	CHECK (layout_divider_at (0, 40) == NULL);
	CHECK (layout_divider_at (0, W - 1) == NULL);
	CHECK (layout_divider_at (H, 39) == NULL);

	CHECK (layout_resize_split (s, 0));
	layout ();
	CHECK (a->fwidth == 4);
	CHECK (layout_resize_split (s, W));
	layout ();
	CHECK (b->fwidth == 4);
}

static void
nested (void)
{
	/* Rows within rows: B, at 20% of the top, limits how far the
	   outer divider can move up. */
	Window *a = first_window (), *c = split (a, SPLIT_ROWS, 0.5);
	Window *b = split (a, SPLIT_ROWS, 0.8);
	WinNode *outer = parent_of (c), *inner = parent_of (a);
	layout ();
	CHECK (a->fheight == 8 && b->fheight == 3);
	CHECK (layout_divider_at (7, 0) == inner);
	CHECK (layout_divider_at (10, 0) == outer);
	CHECK (layout_resize_split (outer, 0));
	layout ();
	CHECK (inner->height == 6 && b->fheight == 2);
	CHECK (layout_resize_split (outer, H));
	layout ();
	CHECK (c->fheight == 2);

	/* Columns within columns, likewise. */
	a = first_window ();
	Window *r = split (a, SPLIT_COLS, 0.5);
	b = split (a, SPLIT_COLS, 0.9);
	outer = parent_of (r);
	inner = parent_of (a);
	layout ();
	CHECK (layout_resize_split (outer, 0));
	layout ();
	CHECK (inner->width == 31 && b->fwidth == 4);

	/* Columns within the top of rows: the row divider is the status
	   lines of both, and wins where it crosses the separator. */
	a = first_window ();
	c = split (a, SPLIT_ROWS, 0.5);
	b = split (a, SPLIT_COLS, 0.5);
	outer = parent_of (c);
	inner = parent_of (a);
	layout ();
	CHECK (layout_divider_at (10, 0) == outer);
	CHECK (layout_divider_at (10, W - 1) == outer);
	CHECK (layout_divider_at (10, 39) == outer);
	CHECK (layout_divider_at (5, 39) == inner);
	CHECK (layout_divider_at (15, 39) == NULL);
	CHECK (layout_resize_split (inner, W));
	layout ();
	CHECK (b->fwidth == 4 && c->fwidth == W);

	/* Columns within the bottom of rows: the separator reaches the
	   bottom status line, which is not a divider itself. */
	a = first_window ();
	b = split (a, SPLIT_ROWS, 0.5);
	c = split (b, SPLIT_COLS, 0.5);
	inner = parent_of (c);
	layout ();
	CHECK (layout_divider_at (H - 1, 39) == inner);
	CHECK (layout_divider_at (H - 1, 10) == NULL);
	CHECK (layout_divider_at (H - 1, W - 1) == NULL);

	/* Rows within columns limit nothing across. */
	a = first_window ();
	r = split (a, SPLIT_COLS, 0.5);
	b = split (a, SPLIT_ROWS, 0.5);
	outer = parent_of (r);
	layout ();
	CHECK (layout_resize_split (outer, 0));
	layout ();
	CHECK (a->fwidth == 4 && b->fwidth == 4);
}

static void
generation (void)
{
	Window *a = first_window ();
	unsigned long gen = layout_generation ();
	Window *b = split (a, SPLIT_ROWS, 0.5);
	CHECK (layout_generation () != gen);
	gen = layout_generation ();
	remove_leaf_and_promote_sibling (find_leaf_for (b));
	CHECK (layout_generation () != gen);
	layout ();
	CHECK (layout_divider_at (10, 0) == NULL);
}

int
main (void)
{
	no_dividers ();
	rows ();
	cols ();
	nested ();
	generation ();

	printf ("layout test successful.\n");

	return EXIT_SUCCESS;
}
