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

public enum SplitKind { Rows, Cols }
public delegate void DelegateLeaf (LeafNode leaf);

public abstract class WinNode {
    public weak SplitNode? parent;

	public size_t x;
	public size_t y;
	public size_t width;
	public size_t height;
	public abstract void layout (size_t x, size_t y, size_t w, size_t h);
	public abstract void each_leaf (DelegateLeaf func);
    public abstract LeafNode? find_leaf (Window wp);
}

public class SplitNode : WinNode {
	public SplitKind kind;
	public WinNode left_or_top;
	public WinNode right_or_bottom;
	public double ratio = 0.5;

    public SplitNode (SplitKind kind, WinNode left_or_top, WinNode right_or_bottom) {
		this.kind = kind;
		this.left_or_top = left_or_top;
		this.right_or_bottom = right_or_bottom;
        this.left_or_top.parent = this;
        this.right_or_bottom.parent = this;
	}

    public override void layout (size_t x, size_t y, size_t w, size_t h) {
        this.x = x;
        this.y = y;
        this.width = w;
        this.height = h;

        size_t d1, d2;
        if (kind == SplitKind.Rows) {
            d1 = (size_t) (h * ratio);
            if (d1 == 0 && h > 0) d1 = 1;
            if (d1 >= h && h > 1) d1 = h - 1;

            d2 = h - d1;
            left_or_top.layout (x, y, w, d1);
            right_or_bottom.layout (x, y + d1, w, d2);
        } else {
            d1 = (size_t) (w * ratio);
            if (d1 == 0 && w > 0) d1 = 1;
            if (d1 >= w && w > 1) d1 = w - 1;

            d2 = w - d1;
            left_or_top.layout (x, y, d1, h);
            right_or_bottom.layout (x + d1, y, d2, h);
        }
    }

    public override void each_leaf (DelegateLeaf func) {
		left_or_top.each_leaf (func);
		right_or_bottom.each_leaf (func);
	}

    public override LeafNode? find_leaf (Window wp) {
        LeafNode? found = left_or_top.find_leaf (wp);
        if (found != null) return found;
        return right_or_bottom.find_leaf (wp);
    }

    public void replace_child_local (WinNode old_child, WinNode new_child) {
        if (left_or_top == old_child) {
            left_or_top = new_child;
        } else if (right_or_bottom == old_child) {
            right_or_bottom = new_child;
        }
        new_child.parent = this;
    }
}

public class LeafNode : WinNode {
    public Window wp;

	public LeafNode (Window wp) {
		this.wp = wp;
	}

	public override void layout (size_t x, size_t y, size_t w, size_t h) {
		this.x = x;
		this.y = y;
		this.width = w;
		this.height = h;
	}

	public override void each_leaf (DelegateLeaf func) {
		func (this);
	}

    public override LeafNode? find_leaf (Window wp) {
        return (this.wp == wp) ? this : null;
    }
}

public WinNode root_node = null;

public LeafNode? find_leaf_for (Window wp) {
	if (root_node == null) return null;
    return root_node.find_leaf (wp);
}

public void replace_leaf_with_split (LeafNode leaf, SplitKind kind, Window w1, Window w2) {
	LeafNode l1 = new LeafNode (w1);
	LeafNode l2 = new LeafNode (w2);
	SplitNode split = new SplitNode (kind, l1, l2);

	if (root_node == leaf)
		root_node = split;
	else
        leaf.parent.replace_child_local (leaf, split);
}

public void remove_leaf_and_promote_sibling (LeafNode leaf) {
	if (root_node == leaf)
		return;

	SplitNode parent = leaf.parent;
	WinNode sibling = (parent.left_or_top == leaf) ? parent.right_or_bottom : parent.left_or_top;

	if (root_node == parent) {
		root_node = sibling;
		sibling.parent = null;
	} else {
        parent.parent.replace_child_local (parent, sibling);
    }
}

public Window? update_windows_geometry (size_t x, size_t y, size_t w, size_t h) {
	if (root_node == null) return null;

	Window? too_small = null;

	root_node.layout (x, y, w, h);
	root_node.each_leaf ((leaf) => {
		Window wp = leaf.wp;
		wp.xpos = leaf.x;
		wp.fwidth = leaf.width;
		wp.fheight = leaf.height;

		if (leaf.x + leaf.width < w)
			wp.ewidth = (wp.fwidth > 2) ? wp.fwidth - 2 : 1;
		else
			wp.ewidth = wp.fwidth;

		wp.eheight = (wp.fheight > 0) ? wp.fheight - 1 : 0;

		if (too_small == null && (wp.fheight < 2 || wp.fwidth < 4))
		    too_small = wp;
	});

	return too_small;
}

public bool resize_window_layout (Window wp, int delta, bool vertical) {
    LeafNode? leaf = find_leaf_for (wp);
    if (leaf == null) return false;

    SplitNode? split = leaf.parent;
    WinNode? child = leaf;

    SplitKind needed_kind = vertical ? SplitKind.Rows : SplitKind.Cols;

    while (split != null) {
        if (split.kind == needed_kind) {
            bool is_left_top = (split.left_or_top == child);
            size_t total_size = vertical ? split.height : split.width;
            size_t current_part_size = (size_t) (total_size * split.ratio);

            if (!is_left_top) current_part_size = total_size - current_part_size;

            int new_size = (int) current_part_size + delta;
            if (new_size < 2) new_size = 2;
            if (new_size > (int)total_size - 2) new_size = (int)total_size - 2;
            if (new_size == (int)current_part_size) return false;
            double new_ratio = (is_left_top) ? (double) new_size / (double) total_size
                                             : (double) (total_size - new_size) / (double) total_size;

            if (new_ratio < 0.05) new_ratio = 0.05;
            if (new_ratio > 0.95) new_ratio = 0.95;
            split.ratio = new_ratio;
            return true;
        }
        child = split;
        split = split.parent;
    }
    return false;
}
