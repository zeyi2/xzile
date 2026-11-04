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

#include "xzile.h"

#include <assert.h>

/* The current window. */
Window *cur_wp = NULL;
/* The first window in list. */
Window *head_wp = NULL;

Window *
window_find (const char *name)
{
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (strcmp (wp->bp->name, name) == 0)
			return wp;
	return NULL;
}

size_t
window_o (const Window *wp)
{
	/* The current window uses the current buffer point; all other
	   windows have a saved point, except that if a window has just been
	   killed, it needs to use its new buffer's current point. */
	if (wp == cur_wp) {
		assert (wp->bp == cur_bp);
		assert (wp->saved_pt == NULL);
		return buffer_pt (cur_bp);
	}
	if (wp->saved_pt != NULL)
		return wp->saved_pt->o;
	return buffer_pt (wp->bp);
}

bool
window_top_visible (Window *wp)
{
	return buffer_offset_to_line (wp->bp, window_o (wp)) == wp->topdelta;
}

bool
window_bottom_visible (const Window *wp)
{
	return wp->all_displayed;
}

void
window_resync (Window *wp)
{
	size_t n = buffer_offset_to_line (wp->bp, buffer_pt (wp->bp));
	long delta = (long) (n - wp->lastpointn);

	if (delta != 0) {
		if ((delta > 0 && wp->topdelta + (size_t) delta < wp->eheight) ||
		    (delta < 0 && wp->topdelta >= (size_t) (-delta)))
			wp->topdelta += (size_t) delta;
		else if (n > wp->eheight / 2)
			wp->topdelta = wp->eheight / 2;
		else
			wp->topdelta = n;
	}
	wp->lastpointn = n;
}

/*
 * Set the current window and its buffer as the current buffer.
 */
void
window_set_current (Window *wp)
{
	/* Save buffer's point in a new marker.  */
	marker_free (cur_wp->saved_pt);
	cur_wp->saved_pt = marker_point ();

	cur_wp = wp;
	cur_bp = wp->bp;

	/* Update the buffer point with the window's saved point
	   marker.  */
	if (cur_wp->saved_pt != NULL) {
		buffer_goto_offset (cur_bp, cur_wp->saved_pt->o);
		marker_free (cur_wp->saved_pt);
		cur_wp->saved_pt = NULL;
	}
}

static void
window_delete (Window *wp)
{
	Window *prev;

	if (wp == head_wp)
		prev = head_wp = head_wp->next;
	else
		for (prev = head_wp; prev != NULL; prev = prev->next)
			if (prev->next == wp) {
				prev->next = wp->next;
				break;
			}

	if (prev != NULL && wp == cur_wp)
		window_set_current (prev);

	marker_free (wp->saved_pt);
	free (wp);
}

static void
wm_split (Window *wp, enum SplitKind kind)
{
	/* Copy wp. */
	Window *newwp = xzalloc (sizeof (Window));
	newwp->next = wp->next;
	newwp->bp = wp->bp;
	newwp->topdelta = wp->topdelta;
	newwp->start_column = wp->start_column;
	newwp->fwidth = wp->fwidth;
	newwp->fheight = wp->fheight;
	newwp->ewidth = wp->ewidth;
	newwp->eheight = wp->eheight;
	newwp->all_displayed = wp->all_displayed;
	newwp->lastpointn = wp->lastpointn;
	newwp->saved_pt = marker_point ();

	/* Adjust wp. */
	wp->next = newwp;

	WinNode *leaf = find_leaf_for (wp);
	if (leaf != NULL)
		replace_leaf_with_split (leaf, kind, wp, newwp);
}

static void
wm_split_rows (Window *wp)
{
	wm_split (wp, SPLIT_ROWS);
	update_windows_geometry (0, 0, term_width (), get_main_window_height ());
	if (wp->topdelta >= wp->eheight)
		recenter (wp);
}

static void
wm_split_cols (Window *wp)
{
	wm_split (wp, SPLIT_COLS);
	update_windows_geometry (0, 0, term_width (), get_main_window_height ());
}

void
wm_delete (Window *wp)
{
	WinNode *leaf = find_leaf_for (wp);
	if (leaf != NULL)
		remove_leaf_and_promote_sibling (leaf);
	window_delete (wp);
}

/*
 * This function creates the scratch buffer and window when there are
 * no other windows (and possibly no other buffers).
 */
void
create_scratch_window (void)
{
	Buffer *bp = create_scratch_buffer ();
	Window *wp = xzalloc (sizeof (Window));
	cur_wp = head_wp = wp;
	wp->fwidth = wp->ewidth = term_width ();
	/* Save space for minibuffer. */
	wp->fheight = term_height () - 1;
	/* Save space for status line. */
	wp->eheight = wp->fheight - 1;
	wp->bp = cur_bp = bp;
	root_node = leaf_node_new (wp);
}

Window *
popup_window (void)
{
	if (head_wp != NULL && head_wp->next == NULL) {
		/* There is only one window on the screen, so split it. */
		funcall ("split-window");
		return cur_wp->next;
	}

	/* Use the window after the current one, or first window if none. */
	return cur_wp->next != NULL ? cur_wp->next : head_wp;
}

static bool
F_split_window (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	/* Windows smaller than 4 lines cannot be split. */
	if (cur_wp->fheight < 4) {
		minibuf_error ("Window height %zu too small (after splitting)", cur_wp->fheight);
		return false;
	}

	wm_split_rows (cur_wp);
	return true;
}

static bool
F_split_window_right (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	/* Windows smaller than 4 columns cannot be split. */
	if (cur_wp->fwidth < 4) {
		minibuf_error ("Window width %zu too small (after splitting)", cur_wp->fwidth);
		return false;
	}

	wm_split_cols (cur_wp);
	return true;
}

static bool
F_delete_window (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (cur_wp == head_wp && cur_wp->next == NULL) {
		minibuf_error ("Attempt to delete sole ordinary window");
		return false;
	}

	wm_delete (cur_wp);
	return true;
}

static bool
F_enlarge_window (long uniarg, Args *args)
{
	(void) args;
	return resize_window_layout (cur_wp, (int) uniarg, true);
}

static bool
F_shrink_window (long uniarg, Args *args)
{
	(void) args;
	return resize_window_layout (cur_wp, (int) -uniarg, true);
}

static bool
F_enlarge_window_horizontally (long uniarg, Args *args)
{
	(void) args;
	return resize_window_layout (cur_wp, (int) uniarg, false);
}

static bool
F_shrink_window_horizontally (long uniarg, Args *args)
{
	(void) args;
	return resize_window_layout (cur_wp, (int) -uniarg, false);
}

static bool
F_delete_other_windows (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	for (Window *wp = head_wp, *nextwp; wp != NULL; wp = nextwp) {
		nextwp = wp->next;
		if (wp != cur_wp)
			wm_delete (wp);
	}
	return true;
}

static bool
F_other_window (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	window_set_current (cur_wp->next != NULL ? cur_wp->next : head_wp);
	return true;
}

void
window_init (void)
{
	lisp_func_new ("split-window", F_split_window, true,
		       "Split current window into two windows, one above the other.\n"
		       "Both windows display the same buffer now current.");

	lisp_func_new ("split-window-right", F_split_window_right, true,
		       "Split current window into two windows, side by side.\n"
		       "Both windows display the same buffer now current.");

	lisp_func_new ("delete-window", F_delete_window, true,
		       "Remove the current window from the screen.");

	lisp_func_new ("enlarge-window", F_enlarge_window, true,
		       "Make current window one line bigger.");

	lisp_func_new ("shrink-window", F_shrink_window, true,
		       "Make current window one line smaller.");

	lisp_func_new ("enlarge-window-horizontally", F_enlarge_window_horizontally, true,
		       "Make current window one column wider.");

	lisp_func_new ("shrink-window-horizontally", F_shrink_window_horizontally, true,
		       "Make current window one column narrower.");

	lisp_func_new ("delete-other-windows", F_delete_other_windows, true,
		       "Make the selected window fill the screen.");

	lisp_func_new ("other-window", F_other_window, true,
		       "Select the first different window on the screen.\n"
		       "All windows are arranged in a cyclic order.\n"
		       "This command selects the window one step away in that order.");
}
