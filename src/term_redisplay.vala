/* Redisplay engine

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

/* Zile font codes */
const int FONT_NORMAL = 0000;
const int FONT_REVERSE = 0001;
const int FONT_UNDERLINE = 0002;

/*
 * Return a printable representation for a non-printable code point at
 * display column `x`.
 *
 * Tab   spaces up to the next tab stop.
 * C0    caret notation ("^@" … "^_").
 * DEL   "^?".
 * C1    octal escape ("\200" … "\237").
 *
 * Printable code points (including all valid multi-byte UTF-8) must
 * not be passed here; the caller is responsible for the distinction.
 */
string make_char_printable (uint32 ch, size_t x, size_t cur_tab_width) {
	if (ch == '\t')
		return "%*s".printf ((int) (cur_tab_width - x % cur_tab_width), "");
	if (ch < 0x20)
		return "^%c".printf ((int) ('@' + ch));
	if (ch == 0x7F)
		return "^?";
	if (ch >= 0x80 && ch < 0xA0)
		return "\\%o".printf ((int) ch);
	if (ch == 0xFFFD)
		return "?";
	/* Caller should not reach here for printable code points. */
	return "?";
}

/*
 * Draw one buffer line onto terminal row `line`.
 *
 * `startcol`     – first display column of buffer content to show.
 * `gutter_str`   – line number (or spaces) for the gutter; empty if gutter_width 0.
 * `gutter_width` – width of the gutter; text area width is wp.ewidth - gutter_width.
 */
void draw_line (size_t line, size_t leftcol, size_t startcol, Window wp,
				size_t o, Region? r, bool highlight, size_t cur_tab_width,
				string gutter_str, size_t gutter_width) {
	term_move (line, leftcol);

	if (gutter_width > 0) {
		term_attrset (FONT_NORMAL);
		term_addstr (gutter_str);
		term_move (line, leftcol + gutter_width);
	}

	size_t line_len = wp.bp.line_len (o);
	size_t ew = gutter_width < wp.ewidth ? wp.ewidth - gutter_width : 0;

	size_t i   = 0; /* byte index within the line */
	size_t col = 0; /* accumulated display column (from line start) */
	size_t x   = 0; /* display columns written to terminal so far */

	while (i < line_len) {
		size_t char_len;
		uint32 ch = wp.bp.get_utf8_char (o + i, out char_len);

		size_t w;
		if (ch == '\t')
			w = cur_tab_width - col % cur_tab_width;
		else
			w = (size_t) utf8_char_display_width ((unichar) ch);

		if (col + w <= startcol) {
			col += w;
			i   += char_len;
			continue;
		}

		bool in_highlight = highlight && r != null && r.contains (o + i);
		term_apply_face (in_highlight ? FACE_REGION : FACE_DEFAULT);

		if (col < startcol) {
			col += w;
			i   += char_len;
			if (x < ew) {
				term_addstr (" ");
				x++;
			}
			continue;
		}

		if (x >= ew)
			break;

		if (x + w > ew) {
			while (x < ew) {
				term_addstr (" ");
				x++;
			}
			col += w;
			i   += char_len;
			break;
		}

		/* Emit the character. */
		if (ch == '\t' || ch < 0x20 || ch == 0x7F
				|| (ch >= 0x80 && ch < 0xA0) || ch == 0xFFFD) {
			string s = make_char_printable (ch, col, cur_tab_width);
			term_addstr (s);
			x += s.length;
		} else if (char_len == 1) {
			term_addch ((char) ch);
			x += w;
		} else {
			/* Multi-byte UTF-8: write the raw bytes directly. */
			uint8[] buf = new uint8[char_len + 1];
			for (int k = 0; k < char_len; k++)
				buf[k] = (uint8) wp.bp.get_char (o + i + k);
			buf[char_len] = 0;
			term_addstr ((string) buf);
			x += w;
		}

		col += w;
		i   += char_len;
	}

	/* Draw end-of-line indicator or padding. */
	if (x >= ew) {
		term_move (line, leftcol + gutter_width + ew - 1);
		term_apply_face (FACE_DEFAULT);
		term_addstr ("$");
	} else {
		term_apply_face (FACE_DEFAULT);
		term_addstr ("%*s".printf ((int) (ew - x), ""));
	}
	term_apply_face (FACE_DEFAULT);
}

bool calculate_highlight_region (Window wp, out Region *rp) {
	rp = null;

	if ((wp != cur_wp && !get_variable_bool ("highlight-nonselected-windows"))
		|| wp.bp.mark == null
		|| !wp.bp.mark_active)
		return false;

	rp = new Region (wp.o (), wp.bp.mark.o);
	return true;
}

string make_mode_line_flags (Window wp) {
	if (wp.bp.modified && wp.bp.readonly)
		return "%*";
	else if (wp.bp.modified)
		return "**";
	else if (wp.bp.readonly)
		return "%%";
	return "--";
}

string make_screen_pos (Window wp) {
	bool tv = wp.top_visible ();
	bool bv = wp.bottom_visible ();

	if (tv && bv)
		return "All";
	else if (tv)
		return "Top";
	else if (bv)
		return "Bot";
	else
		return "%2d%%".printf((int) ((float) 100.0 * wp.o () / wp.bp.length));
}

/*
 * Single place for line-number mode: read from buffer (never cur_bp in
 * callers).  Returns whether absolute and/or relative line numbers are
 * enabled for that buffer.
 */
static void get_line_number_modes (Buffer bp, out bool abs_mode, out bool rel_mode) {
	abs_mode = false;
	rel_mode = false;
	unowned string? v = bp.get_variable ("line-number-mode");
	if (v != null && !str_equal (v, "nil"))
		abs_mode = true;
	v = bp.get_variable ("relative-line-number-mode");
	if (v != null && !str_equal (v, "nil"))
		rel_mode = true;
}

static bool buffer_has_linenum (Buffer bp) {
	bool abs_mode, rel_mode;
	get_line_number_modes (bp, out abs_mode, out rel_mode);
	return abs_mode || rel_mode;
}

static bool right_neighbor_has_linenum (size_t right_x, size_t top_y, size_t height) {
	bool has = false;
	root_node.each_leaf ((leaf) => {
		if (leaf.x == right_x &&
		    leaf.y < top_y + height && leaf.y + leaf.height > top_y &&
		    buffer_has_linenum (leaf.wp.bp))
			has = true;
	});
	return has;
}

/*
 * Compute gutter width for a window from its buffer's line-number settings.
 * Uses get_line_number_modes(wp.bp) so each window uses its own buffer.
 * Gutter width = digit count for max line number + 2 (space + margin); relative
 * mode can show negative numbers so the same formula covers both.
 */
static void update_gutter_width (Window wp) {
	bool abs_mode, rel_mode;
	get_line_number_modes (wp.bp, out abs_mode, out rel_mode);

	if (!abs_mode && !rel_mode) {
		wp.gutter_width = 0;
		return;
	}

	size_t max_line = wp.bp.offset_to_line (wp.bp.length) + 1;
	size_t n = max_line;
	int digits = 0;
	do { n /= 10; digits++; } while (n > 0);

	wp.gutter_width = (size_t) digits + 2;
}

static void draw_status_line (size_t line, size_t leftcol, Window wp) {
	term_apply_face (wp == cur_wp ? FACE_MODE_LINE : FACE_MODE_LINE_INACTIVE);

	term_move (line, leftcol);
	for (size_t i = 0; i < wp.ewidth; ++i)
		term_addstr ("-");

	string eol_type;
	if (cur_bp.eol == ImmutableEstr.eol_cr)
		eol_type = "(Mac)";
	else if (cur_bp.eol == ImmutableEstr.eol_crlf)
		eol_type = "(DOS)";
	else
		eol_type = ":";

	term_move (line, leftcol);
	size_t n = wp.bp.offset_to_line (wp.o ());
	string a = "--%s%2s  %-15s   %s %-9s (Fundamental".printf (
		eol_type, make_mode_line_flags (wp), wp.bp.name,
		make_screen_pos (wp), "(%zu,%zu)".printf (
			n + 1, wp.bp.calculate_goalc (wp.o ())
			)
		);

	if (wp.bp.autofill)
		a += " Fill";
	if (Flags.DEFINING_MACRO in thisflag)
		a += " Def";
	if (wp.bp.isearch)
		a += " Isearch";

	a += ")";
	term_addstr (a);

	term_attrset (FONT_NORMAL);
}

void draw_window (size_t topline, size_t leftcol, Window wp) {
	size_t i, o;
	Region? r;
	bool highlight = calculate_highlight_region (wp, out r);

	/* Find the first line to display on the first screen line. */
	for (o = wp.bp.start_of_line (wp.o ()), i = wp.topdelta;
		 i > 0 && o > 0;
		 assert ((o = wp.bp.prev_line (o)) != size_t.MAX), --i)
		;

	size_t gutter_width = wp.gutter_width;
	bool abs_mode, rel_mode;
	get_line_number_modes (wp.bp, out abs_mode, out rel_mode);

	size_t current_line = wp.bp.offset_to_line (wp.o ());
	size_t line_idx = (wp.topdelta <= current_line) ? current_line - wp.topdelta : 0;

	size_t cur_tab_width = wp.bp.tab_width ();
	for (i = topline; i < wp.eheight + topline; ++i) {
		term_move (i, leftcol);

		if (o == size_t.MAX) {
			if (gutter_width > 0)
				term_addstr ("%*s".printf ((int) gutter_width, ""));
			size_t text_w = (gutter_width < wp.ewidth) ? wp.ewidth - gutter_width : 0;
			term_addstr ("%*s".printf ((int) text_w, ""));
			continue;
		}

		string gutter_str = "";
		if (gutter_width > 0) {
			long num_to_show = 0;
			if (rel_mode) {
				if (line_idx == current_line)
					num_to_show = (long) line_idx + 1;
				else {
					long diff = (long) line_idx - (long) current_line;
					num_to_show = diff < 0 ? -diff : diff;
				}
			} else if (abs_mode) {
				num_to_show = (long) line_idx + 1;
			}
			gutter_str = "%*ld ".printf ((int) (gutter_width - 1), num_to_show);
		}

		draw_line (i, leftcol, wp.start_column, wp, o, r, highlight, cur_tab_width, gutter_str, gutter_width);

		if (wp.start_column > 0) {
			term_move (i, leftcol + gutter_width);
			term_addstr ("$");
		}

		o = wp.bp.next_line (o);
		line_idx++;
	}

	wp.all_displayed = o >= wp.bp.length;

	if (wp.fheight - wp.eheight > 0)
		draw_status_line (topline + wp.eheight, leftcol, wp);

	if (wp.fwidth > wp.ewidth) {
		size_t sep_col = leftcol + wp.ewidth;
		bool right_has_linenum = right_neighbor_has_linenum (leftcol + wp.fwidth, topline, wp.fheight);
		string sep_str = right_has_linenum ? " |" : "| ";
		for (size_t row = topline; row < topline + wp.fheight; ++row) {
			term_move (row, sep_col);
			term_attrset (FONT_NORMAL);
			term_addstr (sep_str);
		}
	}
}

size_t col;
size_t cur_topline = 0;
size_t cur_leftcol = 0;

public void term_redisplay () {
	update_windows_geometry (0, 0, term_width (), get_main_window_height ());

	/* Update gutter width for each window from its buffer's line-number settings. */
	root_node.each_leaf ((leaf) => update_gutter_width (leaf.wp));

	/* Calculate the start column if the line at point has to be truncated.
	 * Effective text width is ewidth minus the gutter. */
	Buffer bp = cur_wp.bp;
	size_t t = bp.tab_width ();
	size_t line_start = bp.line_o ();
	size_t pt_o = cur_wp.o ();

	col = 0;
	cur_wp.start_column = 0;

	size_t ew = cur_wp.gutter_width < cur_wp.ewidth ? cur_wp.ewidth - cur_wp.gutter_width : 0;

	/* cursor_col: display columns from the start of the line to the point. */
	size_t cursor_col = bp.calculate_goalc (pt_o);

	if (cursor_col >= ew) {
		/* The cursor is off the right edge.  Choose a start column so the
		 * cursor lands in the right third of the window.  We advance through
		 * the line byte-by-byte (at code-point boundaries) until the distance
		 * from that position to the cursor fits. */
		size_t target_from_right = ew * 2 / 3;
		size_t pos = line_start;
		size_t running_col = 0;

		while (pos < pt_o) {
			size_t char_len;
			uint32 ch = bp.get_utf8_char (pos, out char_len);
			size_t w = (ch == '\t') ? (t - running_col % t) : (size_t) utf8_char_display_width ((unichar) ch);

			if (cursor_col - running_col <= target_from_right)
				break;

			running_col += w;
			pos += char_len;
		}

		cur_wp.start_column = pos - line_start;
		col = cursor_col - running_col;
	} else {
		col = cursor_col;
	}

	/* Draw the windows. */
	cur_topline = 0;
	root_node.each_leaf ((leaf) => {
		Window wp = leaf.wp;
		draw_window (leaf.y, leaf.x, wp);
		if (wp == cur_wp) {
			cur_topline = leaf.y;
			cur_leftcol = leaf.x;
		}
	});

	term_redraw_cursor ();
}

void term_redraw_cursor () {
	term_move (cur_topline + cur_wp.topdelta, cur_leftcol + cur_wp.gutter_width + col);
}

/*
 * Tidy and close the terminal ready to leave Zile.
 */
public void term_finish () {
	term_move (term_height () - 1, 0);
	term_clrtoeol ();
	term_attrset (FONT_NORMAL);
	term_refresh ();
	term_close ();
}
