/* Dynamically-allocated strings with line ending encoding.

   Copyright (c) 2011-2020 Free Software Foundation, Inc.

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

#include <assert.h>

/*
 * String positions start at zero, as with ordinary C strings.
 *
 * Strings may contain NULs, but always have a terminating NUL, so treating
 * them as NUL-terminated will not result in buffer overflows.
 */

/* End-of-line types. */
const char eol_lf[] = "\n";
const char eol_crlf[] = "\r\n";
const char eol_cr[] = "\r";

#define ALLOCATION_CHUNK_SIZE 16

EstrView
ev_of (const char *text, size_t len, const char *eol)
{
	EstrView ev = { text, len, eol != NULL ? eol : eol_lf };
	return ev;
}

EstrView
ev_cstr (const char *s)
{
	return ev_of (s, strlen (s), eol_lf);
}

EstrView
ev_empty (void)
{
	return ev_of ("", 0, eol_lf);
}

EstrView
ev_substring (EstrView ev, size_t pos, size_t len)
{
	return ev_of (ev.text + pos, len, ev.eol);
}

static const char *
memrmem (const char *s, size_t slen, const char *t, size_t tlen)
{
	if (slen >= tlen) {
		size_t i = slen - tlen;
		do
			if (memcmp (s + i, t, tlen) == 0)
				return s + i;
		while (i-- != 0);
	}
	return NULL;
}

size_t
ev_start_of_line (EstrView ev, size_t o)
{
	size_t eol_len = strlen (ev.eol);
	const char *prev = memrmem (ev.text, o, ev.eol, eol_len);
	return prev != NULL ? (size_t) (prev - ev.text) + eol_len : 0;
}

size_t
ev_end_of_line (EstrView ev, size_t o)
{
	const char *next = xmemmem (ev.text + o, ev.len - o, ev.eol, strlen (ev.eol));
	return next != NULL ? (size_t) (next - ev.text) : ev.len;
}

size_t
ev_prev_line (EstrView ev, size_t o)
{
	size_t so = ev_start_of_line (ev, o);
	return so == 0 ? NOPOS : ev_start_of_line (ev, so - strlen (ev.eol));
}

size_t
ev_next_line (EstrView ev, size_t o)
{
	size_t eo = ev_end_of_line (ev, o);
	return eo == ev.len ? NOPOS : eo + strlen (ev.eol);
}

size_t
ev_lines (EstrView ev)
{
	size_t eol_len = strlen (ev.eol), lines = 0;
	const char *s = ev.text, *next;
	for (size_t len = ev.len;
	     (next = xmemmem (s, len, ev.eol, eol_len)) != NULL;
	     len -= (size_t) (next - s) + eol_len, s = next + eol_len)
		lines++;
	return lines;
}

/*
 * Return the length in bytes if the newlines were replaced with
 * `eol_type`.
 */
size_t
ev_len_with_eol (EstrView ev, const char *eol_type)
{
	return ev.len + ev_lines (ev) * (strlen (eol_type) - strlen (ev.eol));
}


/*
 * Allocate a new string with zero length.
 */
Estr *
estr_new (const char *eol)
{
	Estr *es = xzalloc (sizeof (Estr));
	es->buf_size = ALLOCATION_CHUNK_SIZE;
	es->text = xzalloc (es->buf_size + 1);
	es->eol = eol != NULL ? eol : eol_lf;
	return es;
}

Estr *
estr_copy (EstrView ev)
{
	Estr *es = estr_new (eol_lf);
	estr_cat (es, ev);
	return es;
}

void
estr_free (Estr *es)
{
	if (es != NULL) {
		free (es->text);
		free (es);
	}
}

EstrView
estr_view (const Estr *es)
{
	return ev_of (es->text, es->len, es->eol);
}

/*
 * Change the length of an estr, possibly reallocating it if it needs to
 * grow, or has shrunk considerably.
 */
static void
estr_resize (Estr *es, size_t length)
{
	if (length > es->buf_size || length < es->buf_size / 2) {
		es->buf_size = length + ALLOCATION_CHUNK_SIZE;
		es->text = xrealloc (es->text, es->buf_size + 1);
	}
	es->len = length;
	es->text[length] = '\0';
}

/* Maximum number of EOLs to check before deciding type. */
#define MAX_EOL_CHECK_COUNT 3
void
estr_set_eol_from_text (Estr *es)
{
	bool first_eol = true;
	size_t total_eols = 0;
	es->eol = eol_lf;	/* Set default value */
	for (size_t i = 0; i < es->len && total_eols < MAX_EOL_CHECK_COUNT; i++) {
		char c = es->text[i];
		if (c == '\n' || c == '\r') {
			const char *this_eol_type;
			total_eols++;
			if (c == '\n')
				this_eol_type = eol_lf;
			else if (i == es->len - 1 || es->text[i + 1] != '\n')
				this_eol_type = eol_cr;
			else {
				this_eol_type = eol_crlf;
				i++;
			}

			if (first_eol) {
				/* This is the first end-of-line. */
				es->eol = this_eol_type;
				first_eol = false;
			} else if (es->eol != this_eol_type) {
				/* This EOL is different from the last; arbitrarily choose LF. */
				es->eol = eol_lf;
				break;
			}
		}
	}
}

/*
 * Overwrite characters starting at `pos', with the argument string.
 */
void
estr_replace (Estr *es, size_t pos, EstrView src)
{
	size_t src_eol_len = strlen (src.eol), eol_len = strlen (es->eol);
	const char *s = src.text;
	for (size_t len = src.len; len > 0;) {
		const char *next = xmemmem (s, len, src.eol, src_eol_len);
		size_t line_len = next != NULL ? (size_t) (next - s) : len;
		memmove (es->text + pos, s, line_len);
		pos += line_len;
		len -= line_len;
		s = next;
		if (len > 0) {
			memmove (es->text + pos, es->eol, eol_len);
			s += src_eol_len;
			len -= src_eol_len;
			pos += eol_len;
		}
	}
}

void
estr_cat (Estr *es, EstrView src)
{
	size_t oldlen = es->len;
	estr_insert (es, oldlen, ev_len_with_eol (src, es->eol));
	estr_replace (es, oldlen, src);
}

/*
 * Remove `size' chars from `es' at position `pos'.
 */
void
estr_remove (Estr *es, size_t pos, size_t size)
{
	assert (pos <= es->len);
	assert (size <= es->len - pos);
	memmove (es->text + pos, es->text + pos + size, es->len - (pos + size));
	estr_resize (es, es->len - size);
}

/*
 * Insert gap of `size' characters in `es' at position `pos'.
 */
void
estr_insert (Estr *es, size_t pos, size_t size)
{
	assert (pos <= es->len);
	assert (pos + size >= MAX (pos, size));	/* Check for overflow. */
	estr_resize (es, es->len + size);
	memmove (es->text + pos + size, es->text + pos, es->len - (pos + size));
	memset (es->text + pos, '\0', size);
}

void
estr_move (Estr *es, size_t to, size_t from, size_t n)
{
	assert (to <= es->len);
	assert (from <= es->len);
	assert (n <= es->len - MAX (from, to));
	memmove (es->text + to, es->text + from, n);
}

void
estr_set (Estr *es, size_t pos, int c, size_t n)
{
	assert (pos <= es->len);
	assert (n <= es->len - pos);
	memset (es->text + pos, c, n);
}

/* Read a file into a new string, detecting its EOL type.
   Returns NULL (with errno set) on failure. */
Estr *
estr_from_file (const char *filename)
{
	char *s;
	size_t len;
	if (!read_file (filename, &s, &len))
		return NULL;
	Estr *es = xzalloc (sizeof (Estr));
	es->text = s;
	es->len = len;
	es->buf_size = len;
	estr_set_eol_from_text (es);
	return es;
}
