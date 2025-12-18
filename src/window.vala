/* Window handling functions

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

/* The current window. */
public Window cur_wp = null;
/* The first window in list. */
public Window head_wp = null;

/*
 * Structure
 */
public class Window {
	public Window next;		/* The next window in window list. */
	public Buffer bp;		/* The buffer displayed in window. */
	public size_t topdelta;	/* The top line delta from point. */
	public size_t start_column;	/* The start column of the window (>0 if scrolled
								   sideways). */
	public Marker? saved_pt;	/* The point line pointer, line number and offset
							   (used to hold the point in non-current windows). */
	public size_t fwidth;	/* The formal width and height of the window. */
	public size_t fheight;
	public size_t ewidth;	/* The effective width and height of the window. */
	public size_t eheight;
	public size_t xpos;
	public bool all_displayed; /* The bottom of the buffer is visible */
	internal size_t lastpointn;		/* The last point line number. */

	public static Window? find (string name) {
		for (Window wp = head_wp; wp != null; wp = wp.next)
			if (wp.bp.name == name)
				return wp;

		return null;
	}

	public size_t o () {
		/* The current window uses the current buffer point; all other
		   windows have a saved point, except that if a window has just been
		   killed, it needs to use its new buffer's current point. */
		if (this == cur_wp) {
			assert (bp == cur_bp);
			assert (saved_pt == null);
			return cur_bp.pt;
		} else {
			if (saved_pt != null)
				return saved_pt.o;
			else
				return bp.pt;
		}
	}

	public bool top_visible () {
		return bp.offset_to_line (o ()) == topdelta;
	}

	public bool bottom_visible () {
		return all_displayed;
	}

	public void resync () {
		size_t n = bp.offset_to_line (bp.pt);
		long delta = (long) (n - lastpointn);

		if (delta != 0) {
			if ((delta > 0 && topdelta + delta < eheight) ||
				(delta < 0 && topdelta >= (size_t) (-delta)))
				topdelta += delta;
			else if (n > eheight / 2)
				topdelta = eheight / 2;
			else
				topdelta = n;
		}
		lastpointn = n;
	}

	/*
	 * Set the current window and its buffer as the current buffer.
	 */
	public void set_current () {
		/* Save buffer's point in a new marker.  */
		if (cur_wp.saved_pt != null)
			cur_wp.saved_pt.unchain ();

		cur_wp.saved_pt = Marker.point ();

		cur_wp = this;
		cur_bp = bp;

		/* Update the buffer point with the window's saved point
		   marker.  */
		if (cur_wp.saved_pt != null) {
			cur_bp.goto_offset (cur_wp.saved_pt.o);
			cur_wp.saved_pt.unchain ();
			cur_wp.saved_pt = null;
		}
	}

	public void delete () {
		Window wp;

		if (this == head_wp)
			wp = head_wp = head_wp.next;
		else
			for (wp = head_wp; wp != null; wp = wp.next)
				if (wp.next == this) {
					wp.next = wp.next.next;
					break;
				}

		if (wp != null) {
			wp.set_current ();
		}

		if (this.saved_pt != null)
			this.saved_pt.unchain ();
	}
}

void wm_split (Window wp, SplitKind kind) {
	/* Copy cur_wp. */
	Window newwp = new Window ();
	newwp.next = wp.next;
	newwp.bp = wp.bp;
	newwp.topdelta = wp.topdelta;
	newwp.start_column = wp.start_column;
	newwp.saved_pt = wp.saved_pt;
	newwp.fwidth = wp.fwidth;
	newwp.fheight = wp.fheight;
	newwp.ewidth = wp.ewidth;
	newwp.eheight = wp.eheight;
	newwp.all_displayed = wp.all_displayed;
	newwp.lastpointn = wp.lastpointn;
	newwp.saved_pt = Marker.point ();

	/* Adjust cur_wp. */
	wp.next = newwp;

	LeafNode leaf = find_leaf_for (wp);
	if (leaf != null)
		replace_leaf_with_split (leaf, kind, wp, newwp);
}

void wm_split_rows (Window wp) {
	wm_split (wp, SplitKind.Rows);
	update_windows_geometry (0, 0, term_width (), get_main_window_height ());
	if (wp.topdelta >= wp.eheight)
		recenter (wp);
}

void wm_split_cols (Window wp) {
	wm_split (wp, SplitKind.Cols);
	update_windows_geometry (0, 0, term_width (), get_main_window_height ());
}

void wm_delete (Window wp) {
	LeafNode leaf = find_leaf_for (wp);
	if (leaf != null)
		remove_leaf_and_promote_sibling (leaf);
	wp.delete ();
}

/*
 * This function creates the scratch buffer and window when there are
 * no other windows (and possibly no other buffers).
 */
public void create_scratch_window () {
	Buffer bp = create_scratch_buffer ();
	Window wp = new Window ();
	cur_wp = head_wp = wp;
	wp.fwidth = wp.ewidth = term_width ();
	/* Save space for minibuffer. */
	wp.fheight = term_height () - 1;
	/* Save space for status line. */
	wp.eheight = wp.fheight - 1;
	wp.bp = cur_bp = bp;
	root_node = new LeafNode (wp);
}

Window popup_window () {
	if (head_wp != null && head_wp.next == null) {
		/* There is only one window on the screen, so split it. */
		funcall ("split-window");
		return cur_wp.next;
	}

	/* Use the window after the current one, or first window if none. */
	return cur_wp.next ?? head_wp;
}


public void window_init () {
	new LispFunc (
		"split-window",
		(uniarg, args) => {
			/* Windows smaller than 4 lines cannot be split. */
			if (cur_wp.fheight < 4) {
				Minibuf.error ("Window height %zu too small (after splitting)",
							   cur_wp.fheight);
				return false;
			}

			wm_split_rows (cur_wp);

			return true;
		},
		true,
		"""Split current window into two windows, one above the other.
		Both windows display the same buffer now current."""
		);

	new LispFunc (
		"split-window-right",
		(uniarg, args) => {
			/* Windows smaller than 4 columns cannot be split. */
			if (cur_wp.fwidth < 4) {
				Minibuf.error ("Window width %zu too small (after splitting)",
							   cur_wp.fwidth);
				return false;
			}

			wm_split_cols (cur_wp);

			return true;
		},
		true,
		"""Split current window into two windows, side by side.
		Both windows display the same buffer now current."""
		);

	new LispFunc (
		"delete-window",
		(uniarg, args) => {
			if (cur_wp == head_wp && cur_wp.next == null) {
				Minibuf.error ("Attempt to delete sole ordinary window");
				return false;
			}

			wm_delete (cur_wp);
			return true;
		},
		true,
		"""Remove the current window from the screen."""
		);

	new LispFunc (
		"enlarge-window",
		(uniarg, args) => {
			if (!resize_window_layout (cur_wp, (int) uniarg, true))
				return false;
			return true;
		},
		true,
		"""Make current window one line bigger."""
		);

	new LispFunc (
		"shrink-window",
		(uniarg, args) => {
			if (!resize_window_layout (cur_wp, (int) (-uniarg), true))
				return false;
			return true;
		},
		true,
		"""Make current window one line smaller."""
		);

	new LispFunc (
		"enlarge-window-horizontally",
		(uniarg, args) => {
			if (!resize_window_layout (cur_wp, (int) uniarg, false))
				return false;
			return true;
		},
		true,
		"""Make current window one column wider."""
		);

	new LispFunc (
		"shrink-window-horizontally",
		(uniarg, args) => {
			if (!resize_window_layout (cur_wp, (int) (-uniarg), false))
				return false;
			return true;
		},
		true,
		"""Make current window one column narrower."""
		);

	new LispFunc (
		"delete-other-windows",
		(uniarg, args) => {
			for (Window wp = head_wp, nextwp = null; wp != null; wp = nextwp) {
				nextwp = wp.next;
				if (wp != cur_wp)
					wm_delete (wp);
			}
			return true;
		},
		true,
		"""Make the selected window fill the screen."""
		);

	new LispFunc (
		"other-window",
		(uniarg, args) => {
			if (cur_wp.next != null)
				cur_wp.next.set_current ();
			else
				head_wp.set_current ();
			return true;
		},
		true,
		"""Select the first different window on the screen.
All windows are arranged in a cyclic order.
This command selects the window one step away in that order."""
		);
}
