/* Matching parentheses

   Copyright (c) 2026 Zeyi2 <zeyi2@nekoarch.cc>

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

/* How far to look for a match, as Emacs's blink-matching-paren-distance. */
#define MATCH_DISTANCE 102400

static int
bracket_dir (char c)
{
	switch (c) {
	case '(': case '[': case '{':
		return 1;
	case ')': case ']': case '}':
		return -1;
	default:
		return 0;
	}
}

static char
bracket_partner (char c)
{
	switch (c) {
	case '(': return ')';
	case '[': return ']';
	case '{': return '}';
	case ')': return '(';
	case ']': return '[';
	case '}': return '{';
	default: return 0;
	}
}

/* A line being scanned, with its syntax faces if the buffer has a
   highlighter. */
typedef struct {
	size_t lo, len, idx;
	const char **faces;
} ScanLine;

static void
scan_line_load (Buffer *bp, ScanLine *l, size_t lo, size_t idx)
{
	free (l->faces);
	l->faces = NULL;
	l->lo = lo;
	l->idx = idx;
	l->len = buffer_line_len (bp, lo);
	if (bp->highlighter != NULL && bp->highlighter->scan_line != NULL && l->len > 0) {
		l->faces = xzalloc (l->len * sizeof (char *));
		bp->highlighter->scan_line (bp->highlighter, bp, idx, lo, l->len,
					    buffer_get_line_start_state (bp, idx), l->faces);
	}
}

/* Whether the byte at `o' is in a string or comment. */
static bool
in_text (const ScanLine *l, size_t o)
{
	if (l->faces == NULL || o >= l->lo + l->len)
		return false;
	const char *f = l->faces[o - l->lo];
	return f != NULL && (strcmp (f, FACE_FONT_LOCK_COMMENT) == 0 || strcmp (f, FACE_FONT_LOCK_STRING) == 0);
}

/* Find the bracket matching the one after point, or else before it.
   Brackets in strings and comments only match each other. Sets `*here'
   to the bracket at point and `*there' to its match, or NOPOS if it has
   none; `*mismatch' tells whether they fail to pair up. Returns false if
   there is no bracket at point or the search gave up. */
bool
paren_match (Buffer *bp, size_t pt, size_t *here, size_t *there, bool *mismatch)
{
	size_t len = buffer_length (bp), o;
	int dir;
	if (pt > 0 && bracket_dir (buffer_get_char (bp, pt - 1)) < 0) {
		o = pt - 1;
		dir = -1;
	} else if (pt < len && bracket_dir (buffer_get_char (bp, pt)) > 0) {
		o = pt;
		dir = 1;
	} else
		return false;

	ScanLine l = { 0 };
	size_t lo = buffer_start_of_line (bp, o);
	scan_line_load (bp, &l, lo, buffer_offset_to_line (bp, lo));
	bool text = in_text (&l, o);
	char want = bracket_partner (buffer_get_char (bp, o));

	*here = o;
	*there = NOPOS;
	*mismatch = true;
	bool found = true;
	size_t depth = 0, i = o;
	for (size_t scanned = 0;; scanned++) {
		if (scanned > MATCH_DISTANCE) {
			found = false;
			break;
		}
		if (dir > 0) {
			if (++i >= len)
				break;
			if (i > l.lo + l.len) {
				size_t next = buffer_next_line (bp, l.lo);
				if (next == NOPOS)
					break;
				scan_line_load (bp, &l, next, l.idx + 1);
				i = next;
			}
		} else {
			if (i == 0)
				break;
			i--;
			if (i < l.lo) {
				size_t prev = buffer_prev_line (bp, l.lo);
				if (prev == NOPOS)
					break;
				scan_line_load (bp, &l, prev, l.idx - 1);
			}
		}
		char c = buffer_get_char (bp, i);
		int d = bracket_dir (c);
		if (d == 0 || in_text (&l, i) != text)
			continue;
		if (d == dir)
			depth++;
		else if (depth > 0)
			depth--;
		else {
			*there = i;
			*mismatch = c != want;
			break;
		}
	}
	free (l.faces);
	return found;
}
