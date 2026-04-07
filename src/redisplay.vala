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

public size_t get_main_window_height () {
	size_t mb_h = Minibuf.get_height ();
	size_t main_h = term_height () - mb_h;
	if (main_h < 1) main_h = 1;
	return main_h;
}

public void resize_windows () {
	bool repeat = true;
	while (repeat) {
		repeat = false;
		Window? to_delete = update_windows_geometry (0, 0, term_width (), get_main_window_height ());

		if (to_delete != null && head_wp.next != null) {
			wm_delete (to_delete);
			repeat = true;
		}
	}
	funcall ("recenter");
	/* Repaint minibuffer content after terminal resize. */
	Minibuf.refresh ();
}

public void recenter (Window wp) {
	size_t n = wp.bp.offset_to_line (wp.o ());

	if (n > wp.eheight / 2)
		wp.topdelta = wp.eheight / 2;
	else
		wp.topdelta = n;
}


public void redisplay_init () {
	new LispFunc (
		"recenter",
		(uniarg, args) => {
			recenter (cur_wp);
			term_clear ();
			term_redisplay ();
			term_refresh ();
			return true;
		},
		true,
		"""Center point in selected window and redisplay frame."""
		);
}
