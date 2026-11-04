/* Line-oriented editing functions

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

#include <ctype.h>

static void
insert_expanded_tab (void)
{
	size_t t = buffer_tab_width (cur_bp);
	bprintf ("%*s", (int) (t - cur_bp->goalc % t), "");
}

static bool
insert_tab (void)
{
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	if (get_variable_bool ("indent-tabs-mode"))
		buffer_insert_char (cur_bp, '\t');
	else
		insert_expanded_tab ();

	return true;
}

/*
 * Insert a newline at the current position without moving the cursor.
 */
bool
intercalate_newline (void)
{
	return insert_newline () && buffer_move_char (cur_bp, -1);
}

static bool
is_space_char (char c)
{
	return isspace ((unsigned char) c);
}

/*
 * If point is greater than fill-column, then split the line at the
 * right-most space character at or before fill-column, if there is
 * one, or at the left-most at or after fill-column, if not. If the
 * line contains no spaces, no break is made.
 *
 * Return 1 if a break was made, 0 if not, and -1 on error.
 */
int
fill_break_line (void)
{
	long n;
	if (!parse_number (buffer_get_variable (cur_bp, "fill-column"), &n) || n < 0) {
		minibuf_error ("Wrong type argument: number-or-markerp, nil");
		return -1;
	}
	size_t fillcol = (size_t) n;

	int break_made = 0;

	/* Only break if we're beyond fill-column. */
	if (cur_bp->goalc > fillcol) {
		size_t break_col = 0;

		/* Save point. */
		Marker *m = marker_point ();

		/* Move cursor back to fill column */
		size_t old_col = buffer_pt (cur_bp) - buffer_line_o (cur_bp);
		while (cur_bp->goalc > fillcol + 1)
			buffer_move_char (cur_bp, -1);

		/* Find break point moving left from fill-column. */
		size_t lo = buffer_line_o (cur_bp);
		for (size_t i = buffer_pt (cur_bp) - lo; i > 0; i--)
			if (is_space_char (buffer_get_char (cur_bp, lo + i - 1))) {
				break_col = i;
				break;
			}

		/* If no break point moving left from fill-column, find first
		   possible moving right. */
		if (break_col == 0)
			for (size_t i = buffer_pt (cur_bp) + 1;
			     i < buffer_end_of_line (cur_bp, lo);
			     i++)
				if (is_space_char (buffer_get_char (cur_bp, i - 1))) {
					break_col = i - lo;
					break;
				}

		if (break_col >= 1) {
			/* Break line. */
			buffer_goto_offset (cur_bp, lo + break_col);
			funcall ("delete-horizontal-space");
			insert_newline ();
			buffer_goto_offset (cur_bp, m->o);
			break_made = 1;
		} else
			/* Undo fiddling with point. */
			buffer_goto_offset (cur_bp, lo + old_col);

		marker_free (m);
	}

	return break_made;
}

static bool
newline (void)
{
	if (cur_bp->autofill && fill_break_line () < 0)
		return false;
	return insert_newline ();
}

void
bprintf (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	char *s = xvasprintf (fmt, ap);
	va_end (ap);
	buffer_insert_estr (cur_bp, ev_cstr (s));
	free (s);
}

/***********************************************************************
			 Indentation command
***********************************************************************/
/*
 * Go to cur_bp->goalc in the previous non-blank line.
 */
static void
previous_nonblank_goalc (void)
{
	size_t cur_goalc = cur_bp->goalc;

	/* Find previous non-blank line. */
	while (funcall_n ("forward-line", -1) && buffer_is_blank_line (cur_bp))
		;

	/* Go to `cur_goalc' in that non-blank line. */
	while (!buffer_eolp (cur_bp) && cur_bp->goalc < cur_goalc)
		buffer_move_char (cur_bp, 1);
}

bool
insert_newline (void)
{
	return buffer_insert_estr (cur_bp, ev_cstr ("\n"));
}

static size_t
previous_line_indent (void)
{
	Marker *m = marker_point ();

	funcall ("previous-line");
	funcall ("beginning-of-line");

	/* Find first non-blank char. */
	while (!buffer_eolp (cur_bp) && is_space_char (buffer_following_char (cur_bp)))
		buffer_move_char (cur_bp, 1);

	size_t cur_indent = cur_bp->goalc;

	/* Restore point. */
	buffer_goto_offset (cur_bp, m->o);
	marker_free (m);

	return cur_indent;
}

static bool
delete_trailing_whitespace (void)
{
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	Marker *m = marker_point ();

	for (size_t line_start = 0; line_start != NOPOS; line_start = buffer_next_line (cur_bp, line_start)) {
		size_t line_end = buffer_end_of_line (cur_bp, line_start);
		size_t trim_start = line_end;

		while (trim_start > line_start) {
			char c = buffer_get_char (cur_bp, trim_start - 1);
			if (c != ' ' && c != '\t')
				break;
			trim_start--;
		}

		if (trim_start < line_end) {
			buffer_goto_offset (cur_bp, trim_start);
			buffer_replace_estr (cur_bp, line_end - trim_start, ev_empty ());
		}
	}

	buffer_goto_offset (cur_bp, m->o);
	marker_free (m);
	return true;
}

static size_t
line_end_with_eol (size_t line_start)
{
	size_t next = buffer_next_line (cur_bp, line_start);
	return next != NOPOS ? next : buffer_length (cur_bp);
}

static void
goto_line_goalc (size_t line_start, size_t goal)
{
	buffer_goto_offset (cur_bp, line_start);
	cur_bp->goalc = goal;
	buffer_goto_goalc (cur_bp);
	cur_bp->goalc = goal;
}

static void
cat_region (Estr *es, size_t start, size_t end)
{
	Estr *r = buffer_get_region (cur_bp, region_new (start, end));
	estr_cat (es, estr_view (r));
	estr_free (r);
}

static EstrView
eol_view (void)
{
	const char *eol = buffer_eol (cur_bp);
	return ev_of (eol, strlen (eol), eol);
}

static bool
move_current_line_up_once (size_t goal)
{
	size_t current_start = buffer_line_o (cur_bp);
	if (current_start == 0) {
		minibuf_error ("Beginning of buffer");
		return false;
	}

	size_t eol_len = strlen (buffer_eol (cur_bp));
	size_t current_end = buffer_end_of_line (cur_bp, current_start);
	size_t current_end_eol = line_end_with_eol (current_start);
	size_t previous_start = buffer_prev_line (cur_bp, current_start);

	Estr *replacement = estr_new (buffer_eol (cur_bp));
	if (current_end_eol == buffer_length (cur_bp)) {
		cat_region (replacement, current_start, current_end);
		estr_cat (replacement, eol_view ());
		cat_region (replacement, previous_start, current_start - eol_len);
	} else {
		cat_region (replacement, current_start, current_end_eol);
		cat_region (replacement, previous_start, current_start);
	}

	buffer_goto_offset (cur_bp, previous_start);
	buffer_replace_estr (cur_bp, current_end_eol - previous_start, estr_view (replacement));
	estr_free (replacement);
	goto_line_goalc (previous_start, goal);
	return true;
}

static bool
move_current_line_down_once (size_t goal)
{
	size_t current_start = buffer_line_o (cur_bp);
	size_t current_next = buffer_next_line (cur_bp, current_start);
	if (current_next == NOPOS) {
		minibuf_error ("End of buffer");
		return false;
	}

	size_t eol_len = strlen (buffer_eol (cur_bp));
	size_t current_end = buffer_end_of_line (cur_bp, current_start);
	size_t next_end = buffer_end_of_line (cur_bp, current_next);
	size_t next_next = buffer_next_line (cur_bp, current_next);
	size_t replace_end = next_next != NOPOS ? next_next : next_end;
	size_t new_start;

	Estr *replacement = estr_new (buffer_eol (cur_bp));
	if (next_next == NOPOS) {
		cat_region (replacement, current_next, next_end);
		estr_cat (replacement, eol_view ());
		cat_region (replacement, current_start, current_end);
		new_start = current_start + (next_end - current_next) + eol_len;
	} else {
		cat_region (replacement, current_next, next_next);
		cat_region (replacement, current_start, current_next);
		new_start = current_start + (next_next - current_next);
	}

	buffer_goto_offset (cur_bp, current_start);
	buffer_replace_estr (cur_bp, replace_end - current_start, estr_view (replacement));
	estr_free (replacement);
	goto_line_goalc (new_start, goal);
	return true;
}

static bool
move_active_region_lines (long line_delta)
{
	Region r = region_calculate ();
	if (region_size (r) == 0)
		return true;

	Estr *text = buffer_get_region (cur_bp, r);
	buffer_goto_offset (cur_bp, r.start);
	buffer_replace_estr (cur_bp, region_size (r), ev_empty ());
	buffer_move_line (cur_bp, line_delta);
	size_t new_start = buffer_pt (cur_bp);
	buffer_insert_estr (cur_bp, estr_view (text));
	estr_free (text);
	if (cur_bp->mark == NULL)
		cur_bp->mark = marker_point ();
	marker_move (cur_bp->mark, cur_bp, new_start);
	cur_bp->mark_active = true;
	return true;
}

static bool
move_text_by_lines (long line_delta)
{
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	if (line_delta == 0)
		return true;

	if (cur_bp->mark_active && cur_bp->mark != NULL && region_size (region_calculate ()) > 0)
		return move_active_region_lines (line_delta);

	size_t goal = buffer_calculate_goalc (cur_bp, buffer_pt (cur_bp));
	long steps = line_delta > 0 ? line_delta : -line_delta;
	for (long i = 0; i < steps; i++) {
		bool ok = line_delta < 0
			? move_current_line_up_once (goal)
			: move_current_line_down_once (goal);
		if (!ok)
			return i > 0;
	}

	cur_bp->mark_active = false;
	return true;
}

static bool
F_tab_to_tab_stop (long uniarg, Args *args)
{
	(void) args;
	return execute_with_uniarg (uniarg, insert_tab, NULL);
}

static bool
F_newline (long uniarg, Args *args)
{
	(void) args;
	return execute_with_uniarg (uniarg, newline, NULL);
}

static bool
F_open_line (long uniarg, Args *args)
{
	(void) args;
	return execute_with_uniarg (uniarg, intercalate_newline, NULL);
}

static bool
F_move_text_up (long uniarg, Args *args)
{
	long n = 1;
	if (!noarg (args) && !int_or_uniarg (args, &n, uniarg))
		return false;
	return move_text_by_lines (-n);
}

static bool
F_move_text_down (long uniarg, Args *args)
{
	long n = 1;
	if (!noarg (args) && !int_or_uniarg (args, &n, uniarg))
		return false;
	return move_text_by_lines (n);
}

static bool
F_duplicate_line (long uniarg, Args *args)
{
	long n = 1;
	if (!noarg (args) && !int_or_uniarg (args, &n, uniarg))
		return false;
	if (n < 1) {
		minibuf_error ("Argument must be positive");
		return false;
	}
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	size_t line_start = buffer_line_o (cur_bp);
	size_t old_col = buffer_pt (cur_bp) - line_start;
	size_t line_end = buffer_end_of_line (cur_bp, buffer_pt (cur_bp));
	size_t insert_at = buffer_next_line (cur_bp, buffer_pt (cur_bp));
	bool has_newline = insert_at != NOPOS;
	size_t eol_len = strlen (buffer_eol (cur_bp));

	Estr *chunk = estr_new (buffer_eol (cur_bp));
	if (has_newline)
		cat_region (chunk, line_start, insert_at);
	else {
		estr_cat (chunk, eol_view ());
		cat_region (chunk, line_start, line_end);
		insert_at = buffer_length (cur_bp);
	}

	buffer_goto_offset (cur_bp, insert_at);
	for (long i = 0; i < n; i++)
		buffer_insert_estr (cur_bp, estr_view (chunk));

	size_t duplicate_start = insert_at + (size_t) (n - 1) * chunk->len;
	if (!has_newline)
		duplicate_start += eol_len;
	estr_free (chunk);
	buffer_goto_offset (cur_bp, duplicate_start + old_col);
	cur_bp->mark_active = false;
	return true;
}

static bool
F_insert (long uniarg, Args *args)
{
	(void) uniarg;
	const char *arg = args_poll (args);
	if (arg != NULL)
		bprintf ("%s", arg);
	return true;
}

static bool
delete_region_if_active (bool *ok)
{
	/* With active region, delete the region first */
	if (cur_bp->mark_active && cur_bp->mark != NULL && region_size (region_calculate ()) > 0) {
		*ok = funcall ("kill-region");
		return true;
	}
	return false;
}

static bool
F_delete_char (long uniarg, Args *args)
{
	bool ok;
	if (delete_region_if_active (&ok))
		return ok;
	long n = 1;
	int_or_uniarg (args, &n, uniarg);
	return execute_with_uniarg (n, cur_delete_char, cur_backward_delete_char);
}

static bool
F_backward_delete_char (long uniarg, Args *args)
{
	bool ok;
	if (delete_region_if_active (&ok))
		return ok;
	long n = 1;
	int_or_uniarg (args, &n, uniarg);
	return execute_with_uniarg (n, cur_backward_delete_char, cur_delete_char);
}

static bool
F_delete_horizontal_space (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	while (!buffer_eolp (cur_bp) && is_space_char (buffer_following_char (cur_bp)))
		buffer_delete_char (cur_bp);

	while (!buffer_bolp (cur_bp) && is_space_char (buffer_preceding_char (cur_bp)))
		buffer_backward_delete_char (cur_bp);

	return true;
}

static bool
F_delete_trailing_whitespace (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return delete_trailing_whitespace ();
}

static bool
F_just_one_space (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	funcall ("delete-horizontal-space");
	buffer_insert_char (cur_bp, ' ');
	return true;
}

static bool
F_indent_relative (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	size_t target_goalc = 0, cur_goalc = cur_bp->goalc;
	size_t t = buffer_tab_width (cur_bp);

	bool ok = false;

	if (buffer_warn_if_readonly (cur_bp))
		return false;

	cur_bp->mark_active = false;

	/* If we're on the first line, set target to 0. */
	if (buffer_line_o (cur_bp) != 0) {
		/* Find goalc in previous non-blank line. */
		Marker *m = marker_point ();

		previous_nonblank_goalc ();

		/* Now find the next blank char. */
		if (!(buffer_preceding_char (cur_bp) == '\t' && cur_bp->goalc > cur_goalc))
			while (!buffer_eolp (cur_bp) && !is_space_char (buffer_following_char (cur_bp)))
				buffer_move_char (cur_bp, 1);

		/* Find next non-blank char. */
		while (!buffer_eolp (cur_bp) && is_space_char (buffer_following_char (cur_bp)))
			buffer_move_char (cur_bp, 1);

		/* Target column. */
		if (!buffer_eolp (cur_bp))
			target_goalc = cur_bp->goalc;

		buffer_goto_offset (cur_bp, m->o);
		marker_free (m);
	}

	/* Insert indentation.  */
	if (target_goalc > 0) {
		/* If not at EOL on target line, insert spaces & tabs up to
		   target_goalc; if already at EOL on target line, insert a tab. */
		if (cur_bp->goalc < target_goalc) {
			do {
				if (cur_bp->goalc % t == 0 && cur_bp->goalc + t <= target_goalc)
					ok = insert_tab ();
				else
					ok = buffer_insert_char (cur_bp, ' ');
			} while (ok && cur_bp->goalc < target_goalc);
		} else
			ok = insert_tab ();
	} else
		ok = insert_tab ();
	return ok;
}

static bool
F_indent_for_tab_command (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (get_variable_bool ("tab-always-indent"))
		return insert_tab ();
	else if (cur_bp->goalc < previous_line_indent ())
		return funcall ("indent-relative");
	return true;
}

static bool
F_newline_and_indent (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	bool ok = false;

	if (buffer_warn_if_readonly (cur_bp))
		return false;

	cur_bp->mark_active = false;

	if (insert_newline ()) {
		Marker *m = marker_point ();

		/* Check where last non-blank goalc is. */
		previous_nonblank_goalc ();
		size_t pos = cur_bp->goalc;
		bool indent = pos > 0 || (!buffer_eolp (cur_bp) && is_space_char (buffer_following_char (cur_bp)));
		buffer_goto_offset (cur_bp, m->o);
		marker_free (m);
		/* Only indent if we're in column > 0 or we're in column 0 and
		   there is a space character there in the last non-blank line. */
		if (indent)
			funcall ("indent-for-tab-command");
		ok = true;
	}
	return ok;
}

void
line_init (void)
{
	lisp_func_new ("tab-to-tab-stop", F_tab_to_tab_stop, true,
		       "Insert a tabulation at the current point position into the current\n"
		       "buffer.");

	lisp_func_new ("newline", F_newline, true,
		       "Insert a newline at the current point position into\n"
		       "the current buffer.");

	lisp_func_new ("open-line", F_open_line, true,
		       "Insert a newline and leave point before it.");

	lisp_func_new ("move-text-up", F_move_text_up, true,
		       "Move the current line, or the active region, up by N lines.");

	lisp_func_new ("move-text-down", F_move_text_down, true,
		       "Move the current line, or the active region, down by N lines.");

	lisp_func_new ("duplicate-line", F_duplicate_line, true,
		       "Duplicate the current line below point.\n"
		       "With prefix argument N, duplicate the line N times and leave point on the\n"
		       "last duplicate at the same column.");

	lisp_func_new ("insert", F_insert, false,
		       "Insert the argument at point.");

	lisp_func_new ("delete-char", F_delete_char, true,
		       "Delete the following N characters (previous if N is negative).");

	lisp_func_new ("backward-delete-char", F_backward_delete_char, true,
		       "Delete the previous N characters (following if N is negative).");

	lisp_func_new ("delete-horizontal-space", F_delete_horizontal_space, true,
		       "Delete all spaces and tabs around point.");

	lisp_func_new ("delete-trailing-whitespace", F_delete_trailing_whitespace, true,
		       "Delete trailing spaces and tabs from each line in the current buffer.");

	lisp_func_new ("just-one-space", F_just_one_space, true,
		       "Delete all spaces and tabs around point, leaving one space.");

	lisp_func_new ("indent-relative", F_indent_relative, true,
		       "Space out to under next indent point in previous nonblank line.\n"
		       "An indent point is a non-whitespace character following whitespace.\n"
		       "The following line shows the indentation points in this line.\n"
		       "    ^         ^    ^     ^   ^           ^      ^  ^    ^\n"
		       "If the previous nonblank line has no indent points beyond the\n"
		       "column point starts at, `tab-to-tab-stop' is done instead, unless\n"
		       "this command is invoked with a numeric argument, in which case it\n"
		       "does nothing.");

	lisp_func_new ("indent-for-tab-command", F_indent_for_tab_command, true,
		       "Indent line or insert a tab.\n"
		       "Depending on `tab-always-indent', either insert a tab or indent.\n"
		       "If initial point was within line's indentation, position after\n"
		       "the indentation.  Else stay at same point in text.");

	lisp_func_new ("newline-and-indent", F_newline_and_indent, true,
		       "Insert a newline, then indent.\n"
		       "Indentation is done using the `indent-for-tab-command' function.");
}
