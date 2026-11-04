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
		size_t d1 = (size_t) ((double) h * n->ratio);
		if (d1 == 0 && h > 0)
			d1 = 1;
		if (d1 >= h && h > 1)
			d1 = h - 1;
		winnode_layout (n->left_or_top, x, y, w, d1);
		winnode_layout (n->right_or_bottom, x, y + d1, w, h - d1);
	} else {
		size_t d1 = (size_t) ((double) w * n->ratio);
		if (d1 == 0 && w > 0)
			d1 = 1;
		if (d1 >= w && w > 1)
			d1 = w - 1;
		winnode_layout (n->left_or_top, x, y, d1, h);
		winnode_layout (n->right_or_bottom, x + d1, y, w - d1, h);
	}
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

	wp->eheight = wp->fheight > 0 ? wp->fheight - 1 : 0;

	if (gd->too_small == NULL && (wp->fheight < 2 || wp->fwidth < 4))
		gd->too_small = wp;
}

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
