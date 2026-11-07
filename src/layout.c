/* Window layout management.

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

WinNode *root_node = NULL;

/* Counts splits and deletions, which may free nodes. */
static unsigned long generation;

unsigned long
layout_generation (void)
{
	return generation;
}

WinNode *
leaf_node_new (Window *wp)
{
	WinNode *n = xzalloc (sizeof (WinNode));
	n->is_leaf = true;
	n->wp = wp;
	return n;
}

static WinNode *
split_node_new (enum SplitKind kind, WinNode *left_or_top, WinNode *right_or_bottom)
{
	WinNode *n = xzalloc (sizeof (WinNode));
	n->kind = kind;
	n->ratio = 0.5;
	n->left_or_top = left_or_top;
	n->right_or_bottom = right_or_bottom;
	left_or_top->parent = n;
	right_or_bottom->parent = n;
	return n;
}

/* The size of the first child of split `n', which is `total' cells. */
static size_t
first_size (const WinNode *n, size_t total)
{
	size_t d1 = (size_t) ((double) total * n->ratio);
	if (d1 == 0 && total > 0)
		d1 = 1;
	if (d1 >= total && total > 1)
		d1 = total - 1;
	return d1;
}

static void
winnode_layout (WinNode *n, size_t x, size_t y, size_t w, size_t h)
{
	n->x = x;
	n->y = y;
	n->width = w;
	n->height = h;
	if (n->is_leaf)
		return;

	if (n->kind == SPLIT_ROWS) {
		size_t d1 = first_size (n, h);
		winnode_layout (n->left_or_top, x, y, w, d1);
		winnode_layout (n->right_or_bottom, x, y + d1, w, h - d1);
	} else {
		size_t d1 = first_size (n, w);
		winnode_layout (n->left_or_top, x, y, d1, h);
		winnode_layout (n->right_or_bottom, x + d1, y, w - d1, h);
	}
}

static bool
too_small (size_t w, size_t h)
{
	return h < 2 || w < 4;
}

/* Whether every window in `n' would be big enough at size `w' x `h'. */
static bool
winnode_fits (const WinNode *n, size_t w, size_t h)
{
	if (n->is_leaf)
		return !too_small (w, h);
	if (n->kind == SPLIT_ROWS) {
		size_t d1 = first_size (n, h);
		return winnode_fits (n->left_or_top, w, d1) && winnode_fits (n->right_or_bottom, w, h - d1);
	}
	size_t d1 = first_size (n, w);
	return winnode_fits (n->left_or_top, d1, h) && winnode_fits (n->right_or_bottom, w - d1, h);
}

void
winnode_each_leaf (WinNode *n, LeafFn fn, void *data)
{
	if (n == NULL)
		return;
	if (n->is_leaf)
		fn (n, data);
	else {
		winnode_each_leaf (n->left_or_top, fn, data);
		winnode_each_leaf (n->right_or_bottom, fn, data);
	}
}

static WinNode *
winnode_find_leaf (WinNode *n, Window *wp)
{
	if (n->is_leaf)
		return n->wp == wp ? n : NULL;
	WinNode *found = winnode_find_leaf (n->left_or_top, wp);
	return found != NULL ? found : winnode_find_leaf (n->right_or_bottom, wp);
}

WinNode *
find_leaf_for (Window *wp)
{
	return root_node != NULL ? winnode_find_leaf (root_node, wp) : NULL;
}

static void
replace_child_local (WinNode *parent, WinNode *old_child, WinNode *new_child)
{
	if (parent->left_or_top == old_child)
		parent->left_or_top = new_child;
	else if (parent->right_or_bottom == old_child)
		parent->right_or_bottom = new_child;
	new_child->parent = parent;
}

void
replace_leaf_with_split (WinNode *leaf, enum SplitKind kind, Window *w1, Window *w2)
{
	generation++;
	WinNode *split = split_node_new (kind, leaf_node_new (w1), leaf_node_new (w2));

	if (root_node == leaf)
		root_node = split;
	else
		replace_child_local (leaf->parent, leaf, split);
	free (leaf);
}

void
remove_leaf_and_promote_sibling (WinNode *leaf)
{
	if (root_node == leaf)
		return;

	generation++;
	WinNode *parent = leaf->parent;
	WinNode *sibling = parent->left_or_top == leaf ? parent->right_or_bottom : parent->left_or_top;

	if (root_node == parent) {
		root_node = sibling;
		sibling->parent = NULL;
	} else
		replace_child_local (parent->parent, parent, sibling);
	free (leaf);
	free (parent);
}

typedef struct {
	size_t w;
	Window *too_small;
} GeometryData;

static void
update_leaf_geometry (WinNode *leaf, void *data)
{
	GeometryData *gd = data;
	Window *wp = leaf->wp;
	wp->xpos = leaf->x;
	wp->fwidth = leaf->width;
	wp->fheight = leaf->height;

	if (leaf->x + leaf->width < gd->w)
		wp->ewidth = wp->fwidth > 2 ? wp->fwidth - 2 : 1;
	else
		wp->ewidth = wp->fwidth;
	/* The scroll bar is right of the text. */
	wp->scroll_bar_width = wp->ewidth > layout_scroll_bar_width + 1 ? layout_scroll_bar_width : 0;
	wp->ewidth -= wp->scroll_bar_width;

	wp->eheight = wp->fheight > 0 ? wp->fheight - 1 : 0;

	if (gd->too_small == NULL && too_small (wp->fwidth, wp->fheight))
		gd->too_small = wp;
}

size_t layout_scroll_bar_width = 0;

Window *
update_windows_geometry (size_t x, size_t y, size_t w, size_t h)
{
	if (root_node == NULL)
		return NULL;

	GeometryData gd = { w, NULL };
	winnode_layout (root_node, x, y, w, h);
	winnode_each_leaf (root_node, update_leaf_geometry, &gd);
	return gd.too_small;
}

bool
resize_window_layout (Window *wp, int delta, bool vertical)
{
	WinNode *leaf = find_leaf_for (wp);
	if (leaf == NULL)
		return false;

	WinNode *split = leaf->parent;
	WinNode *child = leaf;
	enum SplitKind needed_kind = vertical ? SPLIT_ROWS : SPLIT_COLS;

	while (split != NULL) {
		if (split->kind == needed_kind) {
			bool is_left_top = split->left_or_top == child;
			size_t total_size = vertical ? split->height : split->width;
			size_t current_part_size = (size_t) ((double) total_size * split->ratio);

			if (!is_left_top)
				current_part_size = total_size - current_part_size;

			int new_size = (int) current_part_size + delta;
			if (new_size > (int) total_size - 2)
				new_size = (int) total_size - 2;
			if (new_size < 2)
				new_size = 2;
			if (new_size == (int) current_part_size)
				return false;
			double new_ratio = is_left_top
				? (double) new_size / (double) total_size
				: (double) ((int) total_size - new_size) / (double) total_size;

			if (new_ratio < 0.05)
				new_ratio = 0.05;
			if (new_ratio > 0.95)
				new_ratio = 0.95;
			split->ratio = new_ratio;
			return true;
		}
		child = split;
		split = split->parent;
	}
	return false;
}

static WinNode *
winnode_divider_at (WinNode *n, size_t y, size_t x)
{
	if (n->is_leaf || y < n->y || y >= n->y + n->height || x < n->x || x >= n->x + n->width)
		return NULL;
	/* The divider is the status line, or the two separator columns,
	   at the bottom or right edge of the first child. */
	WinNode *first = n->left_or_top;
	if (n->kind == SPLIT_ROWS
	    ? y + 1 == first->y + first->height
	    : x < first->x + first->width && x + 2 >= first->x + first->width)
		return n;
	WinNode *found = winnode_divider_at (first, y, x);
	return found != NULL ? found : winnode_divider_at (n->right_or_bottom, y, x);
}

/* The split whose divider (a status line or separator between two
   windows) is at row `y', column `x' of the screen, or NULL. */
WinNode *
layout_divider_at (size_t y, size_t x)
{
	return root_node != NULL ? winnode_divider_at (root_node, y, x) : NULL;
}

typedef struct {
	size_t y, x;
	WinNode *leaf;
} LeafAt;

static void
find_leaf_at (WinNode *leaf, void *data)
{
	LeafAt *d = data;
	if (d->y >= leaf->y && d->y < leaf->y + leaf->height &&
	    d->x >= leaf->x && d->x < leaf->x + leaf->width)
		d->leaf = leaf;
}

/* The window at row `y', column `x' of the screen, or NULL. */
WinNode *
layout_leaf_at (size_t y, size_t x)
{
	LeafAt d = { y, x, NULL };
	if (root_node != NULL)
		winnode_each_leaf (root_node, find_leaf_at, &d);
	return d.leaf;
}

static bool
split_fits (const WinNode *split, size_t size, size_t total)
{
	if (split->kind == SPLIT_ROWS)
		return winnode_fits (split->left_or_top, split->width, size) &&
			winnode_fits (split->right_or_bottom, split->width, total - size);
	return winnode_fits (split->left_or_top, size, split->height) &&
		winnode_fits (split->right_or_bottom, total - size, split->height);
}

/* Make the first child of `split' `size' cells high or wide, or as
   near to that as no window becoming too small allows. Returns true
   if the size changed; update_windows_geometry applies it. */
bool
layout_resize_split (WinNode *split, size_t size)
{
	size_t total = split->kind == SPLIT_ROWS ? split->height : split->width;
	if (total < 2)
		return false;
	size_t cur = first_size (split, total);
	size = MIN (MAX (size, 1), total - 1);
	/* Window sizes only grow with their split's size, so the sizes
	   that fit are a range, and the current size should be in it. */
	while (size != cur && !split_fits (split, size, total))
		size = size < cur ? size + 1 : size - 1;
	if (size == cur)
		return false;
	/* Half a cell over, so that first_size rounds down to `size'. */
	split->ratio = ((double) size + 0.5) / (double) total;
	return true;
}
