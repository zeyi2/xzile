/* Minibuffer facility functions

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

static History *files_history;
static char *contents;
static bool error_displayed;
static size_t reserved_height;	/* Rows kept for a picker. */

/*--------------------------------------------------------------------------
 * Minibuffer wrapper functions.
 *--------------------------------------------------------------------------*/

void
minibuf_init (void)
{
	files_history = history_new ();
}

bool
minibuf_no_error (void)
{
	return !error_displayed;
}

bool
minibuf_showing_error (void)
{
	return error_displayed;
}

void
minibuf_refresh (void)
{
	if (cur_wp != NULL) {
		if (contents != NULL)
			term_minibuf_write (contents);

		/* Redisplay (and leave the cursor in the correct position). */
		term_redraw_cursor ();
		term_refresh ();
	}
}

static void
vwrite_with_state (bool is_error, const char *fmt, va_list ap)
{
	char *s = xvasprintf (fmt, ap);
	if (contents == NULL || strcmp (s, contents) != 0 || error_displayed != is_error) {
		free (contents);
		contents = s;
		error_displayed = is_error;
		minibuf_refresh ();
	} else {
		error_displayed = is_error;
		free (s);
	}
}

/*
 * Write the specified string in the minibuffer.
 */
void
minibuf_write (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	vwrite_with_state (false, fmt, ap);
	va_end (ap);
}

/*
 * Write the specified error string in the minibuffer and signal an error.
 */
void
minibuf_error (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	vwrite_with_state (true, fmt, ap);
	va_end (ap);
	ding ();
}

/*
 * Read a string from the minibuffer.
 */
char *
minibuf_read (const char *fmt, const char *value, ...)
{
	va_list ap;
	va_start (ap, value);
	char *prompt = xvasprintf (fmt, ap);
	va_end (ap);
	char *s = term_minibuf_read (prompt, value != NULL ? value : "", LONG_MAX, NULL, NULL);
	free (prompt);
	return s;
}

/*
 * Read a non-negative number from the minibuffer. Returns LONG_MAX if
 * cancelled, and LONG_MAX - 1 if nothing was entered.
 */
long
minibuf_read_number (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	char *buf = xvasprintf (fmt, ap);
	va_end (ap);

	long n;
	for (;;) {
		char *a = minibuf_read ("%s", "", buf);
		if (a == NULL) {
			n = LONG_MAX;
			funcall ("keyboard-quit");
			break;
		}
		if (a[0] == '\0') {
			free (a);
			n = LONG_MAX - 1;
			break;
		}
		bool ok = parse_long (a, &n);
		free (a);
		if (ok)
			break;
		minibuf_write ("Please enter a number.");
		ding ();
		waitkey ();
	}

	free (buf);
	return n;
}

static char *
vread_filename (bool visit, const char *fmt, const char *value, const char *file, va_list ap)
{
	size_t vlen = strlen (value);
	char *a = file == NULL && vlen > 0 && value[vlen - 1] != '/'
		? xstrcat3 (value, "/", "") : xstrdup (value);
	char *ea = expand_path (a);
	free (a);
	if (ea == NULL)
		return NULL;

	char *prompt = xvasprintf (fmt, ap);
	char *initial = compact_path (ea);
	free (ea);
	if (file == NULL && !str_has_suffix (initial, "/")) {
		char *t = xstrcat3 (initial, "/", "");
		free (initial);
		initial = t;
	}
	char *p = picker_read_file (prompt, initial, files_history, visit);
	free (prompt);
	free (initial);
	if (p != NULL)
		history_add_element (files_history, p);
	return p;
}

/*
 * Read a filename from the minibuffer, for a name that may be new.
 */
char *
minibuf_read_filename (const char *fmt, const char *value, const char *file, ...)
{
	va_list ap;
	va_start (ap, file);
	char *p = vread_filename (false, fmt, value, file, ap);
	va_end (ap);
	return p;
}

/*
 * Read the name of a file to visit, preselecting matches.
 */
char *
minibuf_read_file_to_visit (const char *fmt, const char *value, const char *file, ...)
{
	va_list ap;
	va_start (ap, file);
	char *p = vread_filename (true, fmt, value, file, ap);
	va_end (ap);
	return p;
}

/* Returns 1 for yes, 0 for no, and -1 if cancelled. */
int
minibuf_read_yesno (const char *fmt, ...)
{
	const char *errmsg = "Please answer yes or no.";
	Completion *cp = completion_new (false);
	int ret = -1;

	sv_add_sorted (&cp->completions, "no");
	sv_add_sorted (&cp->completions, "yes");

	va_list ap;
	va_start (ap, fmt);
	char *ms = minibuf_vread_completion (fmt, "", cp, NULL, errmsg, errmsg, ap);
	va_end (ap);

	if (ms != NULL)
		ret = strcmp (ms, "yes") == 0;

	free (ms);
	completion_free (cp);
	return ret;
}

/* Returns 1 for y, 0 for n, and -1 if cancelled. */
int
minibuf_read_y_or_n (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	char *prompt = xvasprintf (fmt, ap);
	va_end (ap);

	const char *errmsg = "";
	int ans;
	for (;;) {
		minibuf_write ("%s%s(y or n) ", errmsg, prompt);
		Keystroke key = getkey (GETKEY_DEFAULT);
		if (key == 'y' || key == 'Y' || key == ' ') {
			ans = 1;
			break;
		} else if (key == 'n' || key == 'N' || key == KBD_DEL || key == KBD_BS) {
			ans = 0;
			break;
		} else if (key == KBD_CANCEL) {
			ans = -1;
			break;
		}
		errmsg = "Please answer y or n.  ";
	}
	free (prompt);
	minibuf_clear ();
	if (ans < 0)
		funcall ("keyboard-quit");
	return ans;
}

char *
minibuf_read_completion (const char *fmt, const char *val, Completion *cp, History *hp, ...)
{
	(void) val;
	va_list ap;
	va_start (ap, hp);
	char *prompt = xvasprintf (fmt, ap);
	va_end (ap);
	char *s = picker_read_name (prompt, &cp->completions, NULL, hp, false, -1);
	free (prompt);
	return s;
}

/* Substitute `arg' for the first "%s" in `fmt'. */
static char *
format_with_arg (const char *fmt, const char *arg)
{
	const char *p = strstr (fmt, "%s");
	if (p == NULL)
		return xstrdup (fmt);
	StrBuf sb;
	sb_init (&sb);
	sb_addn (&sb, fmt, (size_t) (p - fmt));
	sb_add (&sb, arg);
	sb_add (&sb, p + 2);
	return sb_take (&sb);
}

/*
 * Read a string from the minibuffer using a completion.
 */
char *
minibuf_vread_completion (const char *fmt, const char *val, Completion *cp, History *hp,
			  const char *empty_err, const char *invalid_err, va_list ap)
{
	char *ms = NULL;
	char *buf = xvasprintf (fmt, ap);

	for (;;) {
		ms = term_minibuf_read (buf, val, LONG_MAX, cp, hp);

		if (ms == NULL) { /* Cancelled. */
			funcall ("keyboard-quit");
			break;
		} else if (ms[0] == '\0') {
			minibuf_error ("%s", empty_err);
			free (ms);
			ms = NULL;
			break;
		} else {
			/* Complete partial words if possible. */
			if (completion_try (cp, ms, false) == COMPLETION_MATCHED) {
				free (ms);
				ms = xstrdup (cp->match);
			}

			if (sv_contains (&cp->completions, ms)) {
				if (hp != NULL)
					history_add_element (hp, ms);
				minibuf_clear ();
				break;
			} else {
				char *msg = format_with_arg (invalid_err, ms);
				minibuf_error ("%s", msg);
				free (msg);
				free (ms);
				ms = NULL;
				waitkey ();
			}
		}
	}

	free (buf);
	return ms;
}

/*
 * Clear the minibuffer.
 */
void
minibuf_clear (void)
{
	error_displayed = false;
	free (contents);
	contents = NULL;
	term_minibuf_write ("");
}

void
minibuf_set_reserved_height (size_t h)
{
	reserved_height = h;
}

size_t
minibuf_get_height (void)
{
	if (reserved_height > 0)
		return reserved_height;
	size_t h = 1;
	if (contents != NULL) {
		size_t len = strlen (contents);
		h = 1;
		for (size_t i = 0; i < len; i++)
			if (contents[i] == '\n' && i + 1 < len)
				h++;
	}
	return h;
}
