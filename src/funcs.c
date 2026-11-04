/* Miscellaneous Emacs functions

   Copyright (c) 1997-2021 Free Software Foundation, Inc.

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
#include <signal.h>
#include <wctype.h>

void
write_temp_buffer (const char *name, bool show, BufferWriter func, void *data)
{
	/* Popup a window with the buffer "name". */
	Window *old_wp = cur_wp;
	Buffer *old_bp = cur_bp;
	Window *wp = window_find (name);
	if (show && wp != NULL)
		window_set_current (wp);
	else {
		Buffer *bp = buffer_find (name);
		if (show)
			window_set_current (popup_window ());
		if (bp == NULL) {
			bp = buffer_new (NULL);
			bp->name = xstrdup (name);
		}
		buffer_switch_to (bp, true);
	}

	/* Remove the contents of that buffer. */
	Buffer *new_bp = buffer_new (NULL);
	new_bp->name = xstrdup (cur_bp->name);
	buffer_kill (cur_bp);
	cur_bp = new_bp;
	cur_wp->bp = cur_bp;

	/* Make the buffer a temporary one. */
	cur_bp->needname = true;
	cur_bp->noundo = true;
	cur_bp->nosave = true;
	buffer_set_temporary (cur_bp);

	/* Use the callback. */
	func (data);

	funcall ("beginning-of-buffer");
	cur_bp->readonly = true;
	cur_bp->modified = false;

	/* Restore old current window. */
	window_set_current (old_wp);

	/* If we're not showing the new buffer, switch back to the old one. */
	if (!show && buffer_is_live (old_bp))
		buffer_switch_to (old_bp, true);
}

/***********************************************************************
			  Move through words
***********************************************************************/
bool
iswordchar (char c)
{
	return isalnum ((unsigned char) c) || c == '$';
}

bool
iswordcodepoint (uint32_t c)
{
	if (c < 0x80)
		return isalnum ((int) c) || c == '$';
	return c != UTF8_REPLACEMENT && iswalnum ((wint_t) c);
}

/***********************************************************************
	       Move through balanced expressions (sexps)
***********************************************************************/
bool
isopenbracketchar (char c, bool single_quote, bool double_quote)
{
	return c == '(' || c == '[' || c == '{' ||
		(c == '\"' && !double_quote) ||
		(c == '\'' && !single_quote);
}

bool
isclosebracketchar (char c, bool single_quote, bool double_quote)
{
	return c == ')' || c == ']' || c == '}' ||
		(c == '\"' && double_quote) ||
		(c == '\'' && single_quote);
}

/***********************************************************************
			  Transpose functions
***********************************************************************/
static void
estr_append_region (Estr *es)
{
	cur_bp->mark_active = true;
	Estr *r = buffer_get_region (cur_bp, region_calculate ());
	estr_cat (es, estr_view (r));
	estr_free (r);
}

static bool
transpose_subr (MovementNFn move_func)
{
	/* For transpose-chars. */
	if (move_func == cur_move_char && buffer_eolp (cur_bp))
		move_func (-1);

	/* For transpose-lines. */
	if (move_func == cur_move_line && buffer_line_o (cur_bp) == 0)
		move_func (1);

	/* Backward. */
	if (!move_func (-1)) {
		minibuf_error ("Beginning of buffer");
		return false;
	}

	/* Mark the beginning of first string. */
	push_mark ();
	Marker *m1 = marker_point ();

	/* Check to make sure we can go forwards twice. */
	if (!move_func (1) || !move_func (1)) {
		if (move_func == cur_move_line) {
			/* Add an empty line. */
			funcall ("end-of-line");
			funcall ("newline");
		} else {
			pop_mark ();
			buffer_goto_offset (cur_bp, m1->o);
			minibuf_error ("End of buffer");

			marker_free (m1);
			return false;
		}
	}

	buffer_goto_offset (cur_bp, m1->o);

	/* Forward. */
	move_func (1);

	/* Save and delete 1st marked region. */
	Estr *es1 = estr_new (buffer_eol (cur_bp));
	estr_append_region (es1);

	funcall ("delete-region");

	/* Forward. */
	move_func (1);

	/* For transpose-lines. */
	Estr *es2 = NULL;
	Marker *m2;
	if (move_func == cur_move_line)
		m2 = marker_point ();
	else {
		/* Mark the end of second string. */
		set_mark ();

		/* Backward. */
		move_func (-1);
		m2 = marker_point ();

		/* Save and delete 2nd marked region. */
		es2 = estr_new (buffer_eol (cur_bp));
		estr_append_region (es2);
		funcall ("delete-region");
	}

	/* Insert the first string. */
	buffer_goto_offset (cur_bp, m2->o);
	marker_free (m2);
	buffer_insert_estr (cur_bp, estr_view (es1));
	estr_free (es1);

	/* Insert the second string. */
	if (es2 != NULL) {
		buffer_goto_offset (cur_bp, m1->o);
		buffer_insert_estr (cur_bp, estr_view (es2));
		estr_free (es2);
	}
	marker_free (m1);

	/* Restore mark. */
	pop_mark ();
	cur_bp->mark_active = false;

	/* Move forward if necessary. */
	if (move_func != cur_move_line)
		move_func (1);

	return true;
}

static bool
transpose (long uniarg, MovementNFn move)
{
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	bool ret = true;
	unsigned long n = uniarg < 0 ? (unsigned long) -uniarg : (unsigned long) uniarg;
	for (unsigned long uni = 0; ret && uni < n; ++uni)
		ret = transpose_subr (move);

	return ret;
}

static bool
mark (long uniarg, const char *func)
{
	funcall ("set-mark-command");
	bool ok = funcall_n (func, uniarg);
	if (ok)
		funcall ("exchange-point-and-mark");
	return ok;
}

static bool
move_paragraph (long uniarg, MovementFn forward, MovementFn backward, const char *line_extremum)
{
	if (uniarg < 0) {
		uniarg = -uniarg;
		forward = backward;
	}

	while (uniarg-- > 0) {
		while (buffer_is_empty_line (cur_bp) && forward ())
			;
		while (!buffer_is_empty_line (cur_bp) && forward ())
			;
	}

	if (buffer_is_empty_line (cur_bp))
		funcall ("beginning-of-line");
	else
		funcall_n (line_extremum, 1);

	return true;
}

char *
recase (const char *s, size_t len, enum Case rcase)
{
	char *ret = xstrndup (s, len);
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char) ret[i];
		if (rcase == CASE_UPPER || (rcase == CASE_CAPITALIZED && i == 0))
			ret[i] = (char) toupper (c);
		else
			ret[i] = (char) tolower (c);
	}
	return ret;
}

static bool
setcase_word (enum Case rcase)
{
	if (!iswordchar (buffer_following_char (cur_bp)))
		if (!buffer_move_word (cur_bp, 1) || !buffer_move_word (cur_bp, -1))
			return false;

	StrBuf a;
	sb_init (&a);
	size_t lo = buffer_line_o (cur_bp);
	char c;
	for (size_t i = buffer_pt (cur_bp) - lo;
	     i < buffer_line_len (cur_bp, buffer_pt (cur_bp)) &&
		     iswordchar ((c = buffer_get_char (cur_bp, lo + i)));
	     i++)
		sb_addc (&a, c);

	if (a.len > 0) {
		char *s = recase (a.s, a.len, rcase);
		buffer_replace_estr (cur_bp, a.len, ev_of (s, a.len, eol_lf));
		free (s);
	}
	sb_free (&a);

	cur_bp->modified = true;

	return true;
}

static bool
setcase_word_lowercase (void)
{
	return setcase_word (CASE_LOWER);
}

static bool
setcase_word_uppercase (void)
{
	return setcase_word (CASE_UPPER);
}

static bool
setcase_word_capitalize (void)
{
	return setcase_word (CASE_CAPITALIZED);
}

/*
 * Set the region case.
 */
static bool
setcase_region (int (*func) (int))
{
	if (buffer_warn_if_readonly (cur_bp) || buffer_warn_if_no_mark (cur_bp))
		return false;

	Region r = region_calculate ();
	Marker *m = marker_point ();
	buffer_goto_offset (cur_bp, r.start);
	while (buffer_pt (cur_bp) < r.end) {
		unsigned char c = (unsigned char) buffer_get_char (cur_bp, buffer_pt (cur_bp));
		if (buffer_eolp (cur_bp) || c >= 0x80)
			buffer_move_char (cur_bp, 1);
		else {
			char nc = (char) func (c);
			buffer_replace_estr (cur_bp, 1, ev_of (&nc, 1, eol_lf));
		}
	}
	buffer_goto_offset (cur_bp, m->o);
	marker_free (m);

	return true;
}


typedef struct {
	Buffer *old_bp;
} ListBuffersData;

static void
write_buffers_list (void *data)
{
	Buffer *old_bp = ((ListBuffersData *) data)->old_bp;

	bprintf ("CRM Buffer                Size  Mode             File\n");
	bprintf ("--- ------                ----  ----             ----\n");

	/* Print buffers. */
	Buffer *bp = cur_bp;
	do {
		/* Print all buffers whose names don't start with space except
		   this one (the *Buffer List*). */
		if (cur_bp != bp && bp->name[0] != ' ') {
			bprintf ("%c%c%c %-19s %6zu  %-17s",
				 old_bp == bp ? '.' : ' ',
				 bp->readonly ? '%' : ' ',
				 bp->modified ? '*' : ' ',
				 bp->name, buffer_length (bp), buffer_mode_name (bp));
			if (bp->filename != NULL) {
				char *p = compact_path (bp->filename);
				bprintf ("%s", p);
				free (p);
			}
			insert_newline ();
		}
		bp = bp->next;
		if (bp == NULL)
			bp = head_bp;
	} while (bp != cur_bp);
}

static bool
F_list_buffers (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	ListBuffersData data = { cur_bp };
	write_temp_buffer ("*Buffer List*", true, write_buffers_list, &data);
	return true;
}

static bool
F_toggle_read_only (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	cur_bp->readonly = !cur_bp->readonly;
	return true;
}

static bool
F_auto_fill_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	cur_bp->autofill = !cur_bp->autofill;
	return true;
}

static bool
F_set_fill_column (long uniarg, Args *args)
{
	bool ok = true;
	size_t o = buffer_pt (cur_bp) - buffer_line_o (cur_bp);
	long fill_col = (lastflag & FLAG_UNIARG_EMPTY) ? (long) o : uniarg;
	char *buf = NULL;

	if (noarg (args)) {
		fill_col = minibuf_read_number ("Set fill-column to (default %zu): ", o);
		if (fill_col == LONG_MAX)
			return false;
		else if (fill_col == LONG_MAX - 1)
			fill_col = (long) o;
	}

	if (args != NULL) {
		if (!args_empty (args))
			buf = xstrdup_or_null (args_poll (args));
		else {
			minibuf_error ("set-fill-column requires an explicit argument");
			ok = false;
		}
	} else {
		buf = xasprintf ("%ld", fill_col);
		/* Only print message when run interactively. */
		minibuf_write ("Fill column set to %s (was %s)", buf, get_variable ("fill-column"));
	}

	if (ok && buf != NULL)
		set_variable ("fill-column", buf);
	free (buf);
	return ok;
}

static bool
F_set_mark (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	set_mark ();
	cur_bp->mark_active = true;
	return true;
}

static bool
F_set_mark_command (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	funcall ("set-mark");
	minibuf_write ("Mark set");
	return true;
}

static bool
F_exchange_point_and_mark (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (cur_bp->mark == NULL) {
		minibuf_error ("No mark set in this buffer");
		return false;
	}

	size_t o = buffer_pt (cur_bp);
	buffer_goto_offset (cur_bp, cur_bp->mark->o);
	cur_bp->mark->o = o;
	cur_bp->mark_active = true;
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}

static bool
F_mark_whole_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	funcall ("end-of-buffer");
	funcall ("set-mark-command");
	funcall ("beginning-of-buffer");
	return true;
}

static bool
F_quoted_insert (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	minibuf_write ("C-q-");
	unsigned c = getkey_unfiltered (GETKEY_DEFAULT);
	if (c <= 0xff)
		buffer_insert_char (cur_bp, (char) c);
	minibuf_clear ();
	return true;
}

static bool
F_universal_argument (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	bool ok = true;
	int i = 0, arg = 1, sgn = 1;
	StrBuf a;
	sb_init (&a);

	/* Need to process key used to invoke universal-argument. */
	ungetkey (lastkey ());

	thisflag |= FLAG_UNIARG_EMPTY;

	for (;;) {
		Keystroke key = binding_completion (a.s);

		/* Cancelled. */
		if (key == KBD_CANCEL) {
			ok = funcall ("keyboard-quit");
			break;
		} else if (isdigit ((int) (key & 0xff)) && (key & ~(KBD_META | 0xff)) == 0) {
			/* Digit pressed. */
			int digit = (int) ((key & 0xff) - '0');
			thisflag &= ~FLAG_UNIARG_EMPTY;

			if (key & KBD_META) {
				if (a.len > 0)
					sb_add (&a, " ");
				sb_add (&a, "ESC");
			}

			sb_addf (&a, " %d", digit);

			if (i == 0)
				arg = digit;
			else
				arg = arg * 10 + digit;

			i++;
		} else if (key == (KBD_CTRL | 'u')) {
			sb_add (&a, "C-u");
			if (i == 0)
				arg *= 4;
			else
				break;
		} else if ((key & ~KBD_META) == '-' && i == 0) {
			if (sgn > 0) {
				sgn = -sgn;
				sb_add (&a, " -");
				/* The default negative arg is -1, not -4. */
				arg = 1;
				thisflag &= ~FLAG_UNIARG_EMPTY;
			}
		} else {
			ungetkey (key);
			break;
		}
	}
	sb_free (&a);

	if (ok) {
		last_uniarg = arg * sgn;
		thisflag |= FLAG_SET_UNIARG;
		minibuf_clear ();
	}

	return ok;
}

static bool
F_sit_for (long uniarg, Args *args)
{
	(void) uniarg;
	const char *arg = args_poll (args);
	double secs = arg != NULL ? strtod (arg, NULL) : 0;
	int64_t deadline = monotonic_ms () + (int64_t) (secs * 1000);
	/* Redisplay and wait, serving subprocesses, until input arrives. */
	for (;;) {
		int64_t left = deadline - monotonic_ms ();
		Keystroke key = getkey (left > 0 ? (int) left : 0);
		if (key != KBD_NOKEY) {
			ungetkey (key);
			return false;
		}
		if (left <= 0)
			return true;
	}
}

static bool
F_back_to_indentation (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	buffer_goto_offset (cur_bp, buffer_line_o (cur_bp));
	while (!buffer_eolp (cur_bp) && isspace ((unsigned char) buffer_following_char (cur_bp)))
		buffer_move_char (cur_bp, 1);
	return true;
}

static bool
F_suspend_emacs (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	raise (SIGTSTP);
	return true;
}

static bool
F_keyboard_quit (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	cur_bp->mark_active = false;
	minibuf_error ("Quit");
	return false;
}

static bool
F_forward_word (long uniarg, Args *args)
{
	(void) args;
	return move_with_uniarg (uniarg, cur_move_word);
}

static bool
F_backward_word (long uniarg, Args *args)
{
	(void) args;
	return move_with_uniarg (-uniarg, cur_move_word);
}

static bool
F_forward_sexp (long uniarg, Args *args)
{
	(void) args;
	return move_with_uniarg (uniarg, cur_move_sexp);
}

static bool
F_backward_sexp (long uniarg, Args *args)
{
	(void) args;
	return move_with_uniarg (-uniarg, cur_move_sexp);
}

static bool
F_transpose_chars (long uniarg, Args *args)
{
	(void) args;
	return transpose (uniarg, cur_move_char);
}

static bool
F_transpose_words (long uniarg, Args *args)
{
	(void) args;
	return transpose (uniarg, cur_move_word);
}

static bool
F_transpose_sexps (long uniarg, Args *args)
{
	(void) args;
	return transpose (uniarg, cur_move_sexp);
}

static bool
F_transpose_lines (long uniarg, Args *args)
{
	(void) args;
	return transpose (uniarg, cur_move_line);
}

static bool
F_mark_word (long uniarg, Args *args)
{
	(void) args;
	return mark (uniarg, "forward-word");
}

static bool
F_mark_sexp (long uniarg, Args *args)
{
	(void) args;
	return mark (uniarg, "forward-sexp");
}

static bool
F_forward_line (long uniarg, Args *args)
{
	bool ok = true;
	long n = 1;
	if (!noarg (args) && !int_or_uniarg (args, &n, uniarg))
		ok = false;
	if (ok) {
		funcall ("beginning-of-line");
		ok = buffer_move_line (cur_bp, n);
	}
	return ok;
}

static bool
line_up (void)
{
	return buffer_move_line (cur_bp, -1);
}

static bool
line_down (void)
{
	return buffer_move_line (cur_bp, 1);
}

static bool
F_backward_paragraph (long uniarg, Args *args)
{
	(void) args;
	return move_paragraph (uniarg, line_up, line_down, "beginning-of-line");
}

static bool
F_forward_paragraph (long uniarg, Args *args)
{
	(void) args;
	return move_paragraph (uniarg, line_down, line_up, "end-of-line");
}

static bool
F_mark_paragraph (long uniarg, Args *args)
{
	(void) args;
	if (last_command () == lisp_func_find ("mark-paragraph")) {
		funcall ("exchange-point-and-mark");
		funcall_n ("forward-paragraph", uniarg);
		funcall ("exchange-point-and-mark");
	} else {
		funcall_n ("forward-paragraph", uniarg);
		funcall ("set-mark-command");
		funcall_n ("backward-paragraph", uniarg);
	}
	return true;
}

static bool
F_fill_paragraph (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	bool ok = true;
	Marker *m = marker_point ();

	funcall ("forward-paragraph");
	if (buffer_is_empty_line (cur_bp))
		buffer_move_line (cur_bp, -1);
	Marker *m_end = marker_point ();

	funcall ("backward-paragraph");
	if (buffer_is_empty_line (cur_bp))
		/* Move to next line if between two paragraphs. */
		buffer_move_line (cur_bp, 1);

	while (buffer_end_of_line (cur_bp, buffer_pt (cur_bp)) < m_end->o) {
		funcall ("end-of-line");
		buffer_delete_char (cur_bp);
		funcall ("just-one-space");
	}
	marker_free (m_end);

	funcall ("end-of-line");
	int ret;
	while ((ret = fill_break_line ()) == 1)
		;
	if (ret < 0)
		ok = false;

	buffer_goto_offset (cur_bp, m->o);
	marker_free (m);

	return ok;
}

static bool
setcase_words (long uniarg, Args *args, MovementFn fn)
{
	long arg = 1;
	if (!noarg (args) && !int_or_uniarg (args, &arg, uniarg))
		return false;
	return execute_with_uniarg (arg, fn, NULL);
}

static bool
F_downcase_word (long uniarg, Args *args)
{
	return setcase_words (uniarg, args, setcase_word_lowercase);
}

static bool
F_upcase_word (long uniarg, Args *args)
{
	return setcase_words (uniarg, args, setcase_word_uppercase);
}

static bool
F_capitalize_word (long uniarg, Args *args)
{
	return setcase_words (uniarg, args, setcase_word_capitalize);
}

static bool
F_upcase_region (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return setcase_region (toupper);
}

static bool
F_downcase_region (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return setcase_region (tolower);
}

static bool
F_delete_region (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (buffer_warn_if_no_mark (cur_bp) || !region_delete (region_calculate ()))
		return false;
	cur_bp->mark_active = false;
	return true;
}

static bool
F_delete_blank_lines (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Marker *m = marker_point ();
	Region r = { buffer_line_o (cur_bp), buffer_line_o (cur_bp) };

	/* Find following blank lines.  */
	if (funcall ("forward-line") && buffer_is_blank_line (cur_bp)) {
		r.start = buffer_pt (cur_bp);
		do
			r.end = buffer_next_line (cur_bp, buffer_pt (cur_bp));
		while (funcall ("forward-line") && buffer_is_blank_line (cur_bp));
		r.end = MIN (r.end, buffer_length (cur_bp));
	}
	buffer_goto_offset (cur_bp, m->o);

	/* If this line is blank, find any preceding blank lines.  */
	bool singleblank = true;
	if (buffer_is_blank_line (cur_bp)) {
		r.end = MAX (r.end, buffer_next_line (cur_bp, buffer_pt (cur_bp)));
		do
			r.start = buffer_line_o (cur_bp);
		while (funcall_n ("forward-line", -1) && buffer_is_blank_line (cur_bp));
		buffer_goto_offset (cur_bp, m->o);
		if (r.start != buffer_line_o (cur_bp) ||
		    r.end > buffer_next_line (cur_bp, buffer_pt (cur_bp)))
			singleblank = false;
		r.end = MIN (r.end, buffer_length (cur_bp));
	}

	/* If we are deleting to EOB, need to fudge extra line. */
	bool at_eob = r.end == buffer_length (cur_bp) && r.start > 0;
	if (at_eob)
		r.start = r.start - strlen (buffer_eol (cur_bp));

	/* Delete any blank lines found. */
	if (r.start < r.end)
		region_delete (r);

	/* If we found more than one blank line, leave one. */
	if (!singleblank) {
		if (!at_eob)
			intercalate_newline ();
		else
			insert_newline ();
	}

	marker_free (m);
	cur_bp->mark_active = false;

	return true;
}

void
funcs_init (void)
{
	lisp_func_new ("list-buffers", F_list_buffers, true,
		       "Display a list of names of existing buffers.\n"
		       "The list is displayed in a buffer named `*Buffer List*'.\n"
		       "Note that buffers with names starting with spaces are omitted.\n"
		       "\n"
		       "The \"R\" column has a \"%\" if the buffer is read-only.\n"
		       "The \"M\" column has a \"*\" if it is modified.");

	lisp_func_new ("toggle-read-only", F_toggle_read_only, true,
		       "Change whether this buffer is visiting its file read-only.");

	lisp_func_new ("auto-fill-mode", F_auto_fill_mode, true,
		       "Toggle Auto Fill mode.\n"
		       "In Auto Fill mode, inserting a space at a column beyond `fill-column'\n"
		       "automatically breaks the line at a previous space.");

	lisp_func_new ("set-fill-column", F_set_fill_column, true,
		       "Set `fill-column' to specified argument.\n"
		       "Use C-u followed by a number to specify a column.\n"
		       "Just C-u as argument means to use the current column.");

	lisp_func_new ("set-mark", F_set_mark, false,
		       "Set this buffer's mark to point.");

	lisp_func_new ("set-mark-command", F_set_mark_command, true,
		       "Set the mark where point is.");

	lisp_func_new ("exchange-point-and-mark", F_exchange_point_and_mark, true,
		       "Put the mark where point is now, and point where the mark is now.");

	lisp_func_new ("mark-whole-buffer", F_mark_whole_buffer, true,
		       "Put point at beginning and mark at end of buffer.");

	lisp_func_new ("quoted-insert", F_quoted_insert, true,
		       "Read next input character and insert it.\n"
		       "This is useful for inserting control characters.");

	lisp_func_new ("universal-argument", F_universal_argument, true,
		       "Begin a numeric argument for the following command.\n"
		       "Digits or minus sign following \\[universal-argument] make up the numeric argument.\n"
		       "\\[universal-argument] following the digits or minus sign ends the argument.\n"
		       "\\[universal-argument] without digits or minus sign provides 4 as argument.\n"
		       "Repeating \\[universal-argument] without digits or minus sign\n"
		       " multiplies the argument by 4 each time.");

	lisp_func_new ("sit-for", F_sit_for, false,
		       "Redisplay, then wait for SECONDS seconds, or until input is available.\n"
		       "Subprocess output is processed meanwhile.");

	lisp_func_new ("back-to-indentation", F_back_to_indentation, true,
		       "Move point to the first non-whitespace character on this line.");

	lisp_func_new ("suspend-emacs", F_suspend_emacs, true,
		       "Stop Zile and return to superior process.");

	lisp_func_new ("keyboard-quit", F_keyboard_quit, true,
		       "Cancel current command.");

	lisp_func_new ("forward-word", F_forward_word, true,
		       "Move point forward one word (backward if the argument is negative).\n"
		       "With argument, do this that many times.");

	lisp_func_new ("backward-word", F_backward_word, true,
		       "Move backward until encountering the end of a word (forward if the\n"
		       "argument is negative).\n"
		       "With argument, do this that many times.");

	lisp_func_new ("forward-sexp", F_forward_sexp, true,
		       "Move forward across one balanced expression (sexp).\n"
		       "With argument, do it that many times.  Negative arg -N means\n"
		       "move backward across N balanced expressions.");

	lisp_func_new ("backward-sexp", F_backward_sexp, true,
		       "Move backward across one balanced expression (sexp).\n"
		       "With argument, do it that many times.  Negative arg -N means\n"
		       "move forward across N balanced expressions.");

	lisp_func_new ("transpose-chars", F_transpose_chars, true,
		       "Interchange characters around point, moving forward one character.\n"
		       "With prefix arg ARG, effect is to take character before point\n"
		       "and drag it forward past ARG other characters (backward if ARG negative).\n"
		       "If no argument and at end of line, the previous two chars are exchanged.");

	lisp_func_new ("transpose-words", F_transpose_words, true,
		       "Interchange words around point, leaving point at end of them.\n"
		       "With prefix arg ARG, effect is to take word before or around point\n"
		       "and drag it forward past ARG other words (backward if ARG negative).\n"
		       "If ARG is zero, the words around or after point and around or after mark\n"
		       "are interchanged.");

	lisp_func_new ("transpose-sexps", F_transpose_sexps, true,
		       "Like `transpose-words', but applies to sexps.");

	lisp_func_new ("transpose-lines", F_transpose_lines, true,
		       "Exchange current line and previous line, leaving point after both.\n"
		       "With argument ARG, takes previous line and moves it past ARG lines.\n"
		       "With argument 0, interchanges line point is in with line mark is in.");

	lisp_func_new ("mark-word", F_mark_word, true,
		       "Set mark argument words away from point.");

	lisp_func_new ("mark-sexp", F_mark_sexp, true,
		       "Set mark ARG sexps from point.\n"
		       "The place mark goes is the same place \\[forward-sexp] would\n"
		       "move to with the same argument.");

	lisp_func_new ("forward-line", F_forward_line, true,
		       "Move N lines forward (backward if N is negative).\n"
		       "Precisely, if point is on line I, move to the start of line I + N.");

	lisp_func_new ("backward-paragraph", F_backward_paragraph, true,
		       "Move backward to start of paragraph.  With argument N, do it N times.");

	lisp_func_new ("forward-paragraph", F_forward_paragraph, true,
		       "Move forward to end of paragraph.  With argument N, do it N times.");

	lisp_func_new ("mark-paragraph", F_mark_paragraph, true,
		       "Put point at beginning of this paragraph, mark at end.\n"
		       "The paragraph marked is the one that contains point or follows point.");

	lisp_func_new ("fill-paragraph", F_fill_paragraph, true,
		       "Fill paragraph at or after point.");

	lisp_func_new ("downcase-word", F_downcase_word, true,
		       "Convert following word (or ARG words) to lower case, moving over.");

	lisp_func_new ("upcase-word", F_upcase_word, true,
		       "Convert following word (or ARG words) to upper case, moving over.");

	lisp_func_new ("capitalize-word", F_capitalize_word, true,
		       "Capitalize the following word (or ARG words), moving over.\n"
		       "This gives the word(s) a first character in upper case\n"
		       "and the rest lower case.");

	lisp_func_new ("upcase-region", F_upcase_region, true,
		       "Convert the region to upper case.");

	lisp_func_new ("downcase-region", F_downcase_region, true,
		       "Convert the region to lower case.");

	lisp_func_new ("delete-region", F_delete_region, true,
		       "Delete the text between point and mark.");

	lisp_func_new ("delete-blank-lines", F_delete_blank_lines, true,
		       "On blank line, delete all surrounding blank lines, leaving just one.\n"
		       "On isolated blank line, delete that one.\n"
		       "On nonblank line, delete any immediately following blank lines.");
}
