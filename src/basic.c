/* Basic movement functions

   Copyright (c) 1997-2020 Free Software Foundation, Inc.

   This file is part of GNU Zile.

   GNU Zile is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GNU Zile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.  */

#include "xzile.h"

/* Move point `n' lines, or screen rows when lines wrap and
   `line-move-visual' is set. */
static bool
move_line (long n)
{
	if (cur_wp->bp == cur_bp && window_moves_visually (cur_wp))
		return window_move_rows (cur_wp, n);
	return buffer_move_line (cur_bp, n);
}

static bool
scroll_down (void)
{
	if (!window_top_visible (cur_wp))
		return move_line (-(long) cur_wp->eheight);

	minibuf_error ("Beginning of buffer");
	return false;
}

static bool
scroll_up (void)
{
	if (!window_bottom_visible (cur_wp))
		return move_line ((long) cur_wp->eheight);

	minibuf_error ("End of buffer");
	return false;
}

/* Signal an error, and abort any ongoing macro definition. */
void
ding (void)
{
	if (thisflag & FLAG_DEFINING_MACRO)
		cancel_kbd_macro ();

	if (get_variable_bool ("ring-bell"))
		term_beep ();
}

static bool
F_beginning_of_line (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	buffer_goto_offset (cur_bp, buffer_line_o (cur_bp));
	return true;
}

static bool
F_end_of_line (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	buffer_goto_offset (cur_bp, buffer_line_o (cur_bp) + buffer_line_len (cur_bp, buffer_pt (cur_bp)));
	cur_bp->goalc = NOPOS;
	return true;
}

static bool
F_previous_line (long uniarg, Args *args)
{
	(void) args;
	return move_line (-uniarg);
}

static bool
F_next_line (long uniarg, Args *args)
{
	(void) args;
	return move_line (uniarg);
}

static bool
F_goto_char (long uniarg, Args *args)
{
	long n = 1;
	if (noarg (args))
		n = minibuf_read_number ("Goto char: ");
	else if (!int_or_uniarg (args, &n, uniarg))
		return false;

	if (n >= LONG_MAX - 1)
		return false;

	buffer_goto_offset (cur_bp, MIN (buffer_length (cur_bp), (size_t) MAX (n, 1) - 1));
	return true;
}

static bool
F_goto_line (long uniarg, Args *args)
{
	long n = 1;
	if (noarg (args))
		n = minibuf_read_number ("Goto line: ");
	else if (!int_or_uniarg (args, &n, uniarg))
		return false;

	if (n >= LONG_MAX - 1)
		return false;

	buffer_move_line (cur_bp, (MAX (n, 1) - 1) - (long) buffer_offset_to_line (cur_bp, buffer_pt (cur_bp)));
	funcall ("beginning-of-line");
	return true;
}

static bool
F_beginning_of_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	buffer_goto_offset (cur_bp, 0);
	return true;
}

static bool
F_end_of_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	buffer_goto_offset (cur_bp, buffer_length (cur_bp));
	return true;
}

static bool
F_backward_char (long uniarg, Args *args)
{
	(void) args;
	bool ok = buffer_move_char (cur_bp, -uniarg);
	if (!ok)
		minibuf_error ("Beginning of buffer");
	return ok;
}

static bool
F_forward_char (long uniarg, Args *args)
{
	(void) args;
	bool ok = buffer_move_char (cur_bp, uniarg);
	if (!ok)
		minibuf_error ("End of buffer");
	return ok;
}

static bool
F_scroll_down (long uniarg, Args *args)
{
	(void) args;
	return execute_with_uniarg (uniarg, scroll_down, scroll_up);
}

static bool
F_scroll_up (long uniarg, Args *args)
{
	(void) args;
	return execute_with_uniarg (uniarg, scroll_up, scroll_down);
}

void
basic_init (void)
{
	lisp_func_new ("beginning-of-line", F_beginning_of_line, true,
		       "Move point to beginning of current line.");

	lisp_func_new ("end-of-line", F_end_of_line, true,
		       "Move point to end of current line.");

	lisp_func_new ("previous-line", F_previous_line, true,
		       "Move cursor vertically up one line.\n"
		       "If there is no character in the target line exactly over the current column,\n"
		       "the cursor is positioned after the character in that line which spans this\n"
		       "column, or at the end of the line if it is not long enough.");

	lisp_func_new ("next-line", F_next_line, true,
		       "Move cursor vertically down one line.\n"
		       "If there is no character in the target line exactly under the current column,\n"
		       "the cursor is positioned after the character in that line which spans this\n"
		       "column, or at the end of the line if it is not long enough.");

	lisp_func_new ("goto-char", F_goto_char, true,
		       "Set point to POSITION, a number.\n"
		       "Beginning of buffer is position 1.");

	lisp_func_new ("goto-line", F_goto_line, true,
		       "Go to LINE, counting from line 1 at beginning of buffer.");

	lisp_func_new ("beginning-of-buffer", F_beginning_of_buffer, true,
		       "Move point to the beginning of the buffer; leave mark at previous position.");

	lisp_func_new ("end-of-buffer", F_end_of_buffer, true,
		       "Move point to the end of the buffer; leave mark at previous position.");

	lisp_func_new ("backward-char", F_backward_char, true,
		       "Move point left N characters (right if N is negative).\n"
		       "On attempt to pass beginning or end of buffer, stop and signal error.");

	lisp_func_new ("forward-char", F_forward_char, true,
		       "Move point right N characters (left if N is negative).\n"
		       "On reaching end of buffer, stop and signal error.");

	lisp_func_new ("scroll-down", F_scroll_down, true,
		       "Scroll text of current window downward near full screen.");

	lisp_func_new ("scroll-up", F_scroll_up, true,
		       "Scroll text of current window upward near full screen.");
}
