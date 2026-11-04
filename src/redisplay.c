/* Terminal independent redisplay routines

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

size_t
get_main_window_height (void)
{
	size_t mb_h = minibuf_get_height ();
	size_t h = term_height ();
	return h > mb_h ? h - mb_h : 1;
}

void
resize_windows (void)
{
	if (cur_wp == NULL)
		return;

	for (;;) {
		Window *to_delete = update_windows_geometry (0, 0, term_width (), get_main_window_height ());
		if (to_delete == NULL || head_wp->next == NULL)
			break;
		wm_delete (to_delete);
	}
	funcall ("recenter");
	/* Repaint minibuffer content after terminal resize. */
	minibuf_refresh ();
}

void
recenter (Window *wp)
{
	size_t n = buffer_offset_to_line (wp->bp, window_o (wp));

	if (n > wp->eheight / 2)
		wp->topdelta = wp->eheight / 2;
	else
		wp->topdelta = n;
}

static bool
F_recenter (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	recenter (cur_wp);
	term_clear ();
	term_redisplay ();
	term_refresh ();
	return true;
}

void
redisplay_init (void)
{
	lisp_func_new ("recenter", F_recenter, true,
		       "Center point in selected window and redisplay frame.");
}
