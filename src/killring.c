/* Kill ring facility functions

   Copyright (c) 2001-2020 Free Software Foundation, Inc.

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

Estr *kill_ring_text = NULL;

void
maybe_destroy_kill_ring (void)
{
	if (last_command () != lisp_func_find ("kill-region")) {
		estr_free (kill_ring_text);
		kill_ring_text = NULL;
	}
}

void
kill_ring_push (EstrView es)
{
	if (kill_ring_text == NULL)
		kill_ring_text = estr_new (es.eol);
	estr_cat (kill_ring_text, es);
	clipboard_changed ();
}

/* Make text another program copied to the clipboard the text to yank. */
void
kill_ring_take_clipboard (void)
{
	size_t len;
	char *copied = clipboard_fetch (&len);
	if (copied != NULL) {
		estr_free (kill_ring_text);
		kill_ring_text = estr_new (eol_lf);
		estr_cat (kill_ring_text, ev_of (copied, len, eol_lf));
		free (copied);
	}
}

static bool
copy_or_kill_region (bool kill, Region r)
{
	Estr *text = buffer_get_region (cur_bp, r);
	kill_ring_push (estr_view (text));
	estr_free (text);

	if (kill) {
		if (cur_bp->readonly)
			minibuf_error ("Read only text copied to kill ring");
		else
			region_delete (r);
	}

	set_this_command (lisp_func_find ("kill-region"));
	cur_bp->mark_active = false;

	return true;
}

static bool
kill_line (bool whole_line)
{
	bool ok = true;
	bool only_blanks_to_end_of_line = false;
	size_t cur_line_len = buffer_line_len (cur_bp, buffer_pt (cur_bp));

	if (!whole_line) {
		size_t i, lo = buffer_line_o (cur_bp);
		for (i = buffer_pt (cur_bp) - lo; i < cur_line_len; i++) {
			char c = buffer_get_char (cur_bp, lo + i);
			if (!(c == ' ' || c == '\t'))
				break;
		}

		only_blanks_to_end_of_line = i == cur_line_len;
	}

	if (buffer_eobp (cur_bp)) {
		minibuf_error ("End of buffer");
		return false;
	}

	if (!buffer_eolp (cur_bp))
		ok = copy_or_kill_region (true, region_new (buffer_pt (cur_bp), buffer_line_o (cur_bp) + cur_line_len));

	if (ok && (whole_line || only_blanks_to_end_of_line) && !buffer_eobp (cur_bp)) {
		if (!funcall ("delete-char"))
			return false;

		kill_ring_push (ev_cstr ("\n"));
		set_this_command (lisp_func_find ("kill-region"));
	}

	return ok;
}

static bool
kill_whole_line (void)
{
	return kill_line (true);
}

static bool
kill_line_backward (void)
{
	return buffer_move_line (cur_bp, -1) && kill_whole_line ();
}

static bool
copy_or_kill_the_region (bool kill)
{
	bool ok = false;

	if (!buffer_warn_if_no_mark (cur_bp)) {
		Region r = region_calculate ();
		maybe_destroy_kill_ring ();
		ok = copy_or_kill_region (kill, r);
	}

	return ok;
}

static bool
kill_text (long uniarg, const char *mark_func)
{
	maybe_destroy_kill_ring ();

	if (buffer_warn_if_readonly (cur_bp))
		return false;

	push_mark ();
	funcall_n (mark_func, uniarg);
	funcall ("kill-region");
	pop_mark ();

	set_this_command (lisp_func_find ("kill-region"));
	minibuf_write ("%s", "");	/* Erase "Set mark" message.  */
	return true;
}

static bool
F_kill_line (long uniarg, Args *args)
{
	maybe_destroy_kill_ring ();

	bool ok = true;
	if (noarg (args))
		ok = kill_line (buffer_bolp (cur_bp) && get_variable_bool ("kill-whole-line"));
	else {
		long arg = 1;
		if (!int_or_uniarg (args, &arg, uniarg))
			ok = false;
		else {
			if (arg <= 0)
				ok = buffer_bolp (cur_bp) ||
					copy_or_kill_region (true, region_new (buffer_line_o (cur_bp), buffer_pt (cur_bp)));
			if (arg != 0 && ok)
				ok = execute_with_uniarg (arg, kill_whole_line, kill_line_backward);
		}
	}

	cur_bp->mark_active = false;
	return ok;
}

static bool
F_kill_region (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return copy_or_kill_the_region (true);
}

static bool
F_copy_region_as_kill (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return copy_or_kill_the_region (false);
}

static bool
F_kill_word (long uniarg, Args *args)
{
	long arg = 1;
	if (!noarg (args) && !int_or_uniarg (args, &arg, uniarg))
		return false;
	return kill_text (arg, "mark-word");
}

static bool
F_backward_kill_word (long uniarg, Args *args)
{
	long arg = 1;
	if (!noarg (args) && !int_or_uniarg (args, &arg, uniarg))
		return false;
	return kill_text (-arg, "mark-word");
}

static bool
F_kill_sexp (long uniarg, Args *args)
{
	(void) args;
	return kill_text (uniarg, "mark-sexp");
}

static bool
F_yank (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	kill_ring_take_clipboard ();
	if (kill_ring_text == NULL) {
		minibuf_error ("Kill ring is empty");
		return false;
	}

	if (buffer_warn_if_readonly (cur_bp))
		return false;

	funcall ("set-mark-command");
	buffer_insert_estr (cur_bp, estr_view (kill_ring_text));
	cur_bp->mark_active = false;
	return true;
}

void
killring_init (void)
{
	lisp_func_new ("kill-line", F_kill_line, true,
		       "Kill the rest of the current line; if no nonblanks there, kill thru newline.\n"
		       "With prefix argument ARG, kill that many lines from point.\n"
		       "Negative arguments kill lines backward.\n"
		       "With zero argument, kills the text before point on the current line.\n"
		       "\n"
		       "If `kill-whole-line' is non-nil, then this command kills the whole line\n"
		       "including its terminating newline, when used at the beginning of a line\n"
		       "with no argument.");

	lisp_func_new ("kill-region", F_kill_region, true,
		       "Kill (\"cut\") text between point and mark.\n"
		       "This deletes the text from the buffer and saves it in the kill ring.\n"
		       "The command \\[yank] can retrieve it from there.\n"
		       "\n"
		       "Any command that calls this function is a \"kill command\".\n"
		       "If the previous command was also a kill command,\n"
		       "the text killed this time appends to the text killed last time\n"
		       "to make one entry in the kill ring.\n"
		       "\n"
		       "If the buffer is read-only, Zile will beep and refrain from deleting\n"
		       "the text, but put the text in the kill ring anyway.  This means that\n"
		       "you can use the killing commands to copy text from a read-only buffer.");

	lisp_func_new ("copy-region-as-kill", F_copy_region_as_kill, true,
		       "Save the region as if killed, but don't kill it.");

	lisp_func_new ("kill-word", F_kill_word, true,
		       "Kill characters forward until encountering the end of a word.\n"
		       "With argument ARG, do this that many times.");

	lisp_func_new ("backward-kill-word", F_backward_kill_word, true,
		       "Kill characters backward until encountering the end of a word.\n"
		       "With argument ARG, do this that many times.");

	lisp_func_new ("kill-sexp", F_kill_sexp, true,
		       "Kill the sexp (balanced expression) following the cursor.\n"
		       "With ARG, kill that many sexps after the cursor.\n"
		       "Negative arg -N means kill N sexps before the cursor.");

	lisp_func_new ("yank", F_yank, true,
		       "Reinsert (\"paste\") the last stretch of killed text.\n"
		       "More precisely, reinsert the stretch of killed text most recently\n"
		       "killed OR yanked.  Put point at end, and set mark at beginning.");
}
