/* Search and replace functions

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

/* Return true if there are no upper-case letters in the given string.
   If `regex' is true, ignore escaped characters. */
static bool
no_upper (const char *s, size_t len, bool regex)
{
	bool quote_flag = false;

	for (size_t i = 0; i < len; i++) {
		if (regex && s[i] == '\\')
			quote_flag = !quote_flag;
		else if (!quote_flag && isupper ((unsigned char) s[i]))
			return false;
	}

	return true;
}

static const char *re_find_err = NULL;

/* Whether a search for `s' ignores case. */
static bool
search_icase (const char *s, bool regexp)
{
	return get_variable_bool ("case-fold-search") && no_upper (s, strlen (s), regexp);
}

/* Find `n' in `es', setting its match to bytes [*start, *end). */
static bool
find_substr (EstrView es, const char *n, bool forward, int before, int after, bool regex, bool icase,
	     size_t *start, size_t *end)
{
	long ret = -1;
	Regex *re = regex_compile (n, strlen (n), !regex, icase, &re_find_err);
	if (re != NULL) {
		ret = regex_search (re, es.text, es.len, forward, before, after, end);
		if (ret == REGEX_TOO_COMPLEX) {
			re_find_err = "Stack overflow in regexp matcher";
			ret = -1;
		}
		regex_free (re);
	}
	if (ret < 0)
		return false;
	*start = (size_t) ret;
	return true;
}

/* The match the last successful search found, [match_start, match_end). */
static size_t match_start, match_end;

static bool
search (const char *s, bool forward, bool regexp)
{
	if (s[0] == '\0')
		return false;

	/* Attempt match. */
	size_t o = buffer_pt (cur_bp);
	size_t len = buffer_length (cur_bp);
	int before = o > 0 ? (unsigned char) buffer_get_char (cur_bp, o - 1) : -1;
	int after = o < len ? (unsigned char) buffer_get_char (cur_bp, o) : -1;
	EstrView es = forward ? buffer_post_point (cur_bp) : buffer_pre_point (cur_bp);
	size_t start, end;
	if (!find_substr (es, s, forward, forward ? before : -1, forward ? -1 : after, regexp,
			  search_icase (s, regexp), &start, &end))
		return false;

	size_t base = forward ? o : 0;
	match_start = base + start;
	match_end = base + end;
	buffer_goto_offset (cur_bp, forward ? match_end : match_start);
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}


/*
 * Highlighting the matches of a search.
 */

/* The search shown: its pattern, compiled, in the buffer searched, and
   the current match, if `have_current'. */
static Regex *shown_re;
static const Buffer *shown_bp;
static bool have_current;
static size_t current_start, current_end;

/* Counting stops after this many matches, and bigger buffers are not
   counted, as that is done at each key. */
#define COUNT_MAX 9999
#define COUNT_MAX_BYTES (8 * 1024 * 1024)

static void
hide_matches (void)
{
	regex_free (shown_re);
	shown_re = NULL;
	shown_bp = NULL;
	have_current = false;
}

/* Show the matches of `s' in the current buffer, the current one being
   the last found if `found'. */
static void
show_matches (const char *s, bool regexp, bool found)
{
	hide_matches ();
	if (s[0] == '\0')
		return;
	const char *err;
	shown_re = regex_compile (s, strlen (s), !regexp, search_icase (s, regexp), &err);
	if (shown_re == NULL)
		return;
	shown_bp = cur_bp;
	have_current = found;
	current_start = match_start;
	current_end = match_end;
}

/* Call `f' on each non-empty match of the search shown in `text', of
   `len' bytes, with `before' and `after' the bytes around it or -1,
   until it returns false. */
static void
each_match (const char *text, size_t len, int before, int after,
	    bool (*f) (size_t start, size_t end, void *data), void *data)
{
	for (size_t pos = 0; pos <= len;) {
		size_t end;
		int b = pos > 0 ? (unsigned char) text[pos - 1] : before;
		long start = regex_search (shown_re, text + pos, len - pos, true, b, after, &end);
		if (start < 0)
			return;
		size_t s = pos + (size_t) start, e = pos + end;
		if (e > s && !f (s, e, data))
			return;
		pos = e > s ? e : s + 1;
	}
}

typedef struct {
	size_t n, before;	/* The matches, and those before the current. */
} Count;

static bool
count_match (size_t start, size_t end, void *data)
{
	(void) end;
	Count *c = data;
	if (have_current && start < current_start)
		c->before++;
	return ++c->n < COUNT_MAX;
}

/* How many matches of the search shown the buffer has, as "N/M " with
   the current one N, or "M " if there is none; "" if there is no
   search. */
static char *
count_matches (void)
{
	if (shown_re == NULL || buffer_length (shown_bp) > COUNT_MAX_BYTES)
		return xstrdup ("");
	Estr *text = buffer_get_region (shown_bp, region_new (0, buffer_length (shown_bp)));
	Count c = { 0, 0 };
	each_match (text->text, text->len, -1, -1, count_match, &c);
	estr_free (text);
	const char *more = c.n >= COUNT_MAX ? "+" : "";
	return have_current ? xasprintf ("%zu/%zu%s ", c.before + 1, c.n, more) : xasprintf ("%zu%s ", c.n, more);
}

bool
search_current_match (const Window *wp, size_t *start, size_t *end)
{
	if (shown_re == NULL || wp->bp != shown_bp || !have_current)
		return false;
	*start = current_start;
	*end = current_end;
	return true;
}

typedef struct {
	LinkSpan *v;
	size_t n, cap;
} Spans;

static bool
add_span (size_t start, size_t end, void *data)
{
	Spans *s = data;
	if (s->n == s->cap) {
		s->cap = s->cap ? 2 * s->cap : 8;
		s->v = xrealloc (s->v, s->cap * sizeof *s->v);
	}
	s->v[s->n++] = (LinkSpan) { start, end };
	return true;
}

size_t
search_line_matches (const Window *wp, size_t lo, size_t len, LinkSpan **spans)
{
	*spans = NULL;
	if (shown_re == NULL || wp->bp != shown_bp || len == 0)
		return 0;
	Buffer *bp = wp->bp;
	Estr *line = buffer_get_region (bp, region_new (lo, lo + len));
	int before = lo > 0 ? (unsigned char) buffer_get_char (bp, lo - 1) : -1;
	int after = lo + len < buffer_length (bp) ? (unsigned char) buffer_get_char (bp, lo + len) : -1;
	Spans s = { NULL, 0, 0 };
	each_match (line->text, line->len, before, after, add_span, &s);
	estr_free (line);
	*spans = s.v;
	return s.n;
}

static char *last_search = NULL;

static void
set_last_search (const char *s)
{
	char *old = last_search;
	last_search = xstrdup (s);
	free (old);
}

static bool
do_search (bool forward, bool regexp, const char *pattern_in)
{
	bool ok = false;
	char *pattern = xstrdup_or_null (pattern_in);

	if (pattern == NULL)
		pattern = minibuf_read ("%s%s: ", last_search,
					regexp ? "RE search" : "Search", forward ? "" : " backward");

	if (pattern == NULL)
		return funcall ("keyboard-quit");
	if (pattern[0] != '\0') {
		set_last_search (pattern);

		if (search (pattern, forward, regexp))
			ok = true;
		else if (re_find_err != NULL) {
			minibuf_error ("Invalid regexp: \"%s\"", re_find_err);
			re_find_err = NULL;
		} else
			minibuf_error ("Search failed: \"%s\"", pattern);
	}

	free (pattern);
	return ok;
}

/*
 * Incremental search engine.
 */
static bool
isearch (bool forward, bool regexp)
{
	Marker *old_mark = marker_copy (cur_wp->bp->mark);

	cur_wp->bp->isearch = true;

	bool last = true;
	StrBuf pattern;
	sb_init (&pattern);
	size_t start = buffer_pt (cur_bp), cur = start;
	for (;;) {
		/* Make the minibuf message. */
		StrBuf msg;
		sb_init (&msg);
		char *count = count_matches ();
		sb_addf (&msg, "%s%sI-search%s: %s", count,
			 last ? (regexp ? "Regexp " : "") : (regexp ? "Failing regexp " : "Failing "),
			 forward ? "" : " backward",
			 pattern.s);
		free (count);

		/* Regex error. */
		if (re_find_err != NULL) {
			if (str_has_prefix (re_find_err, "Premature ") ||
			    str_has_prefix (re_find_err, "Unmatched ") ||
			    str_has_prefix (re_find_err, "Invalid ") ||
			    str_has_prefix (re_find_err, "Trailing "))
				re_find_err = "incomplete input";
			sb_addf (&msg, " [%s]", re_find_err);
			re_find_err = NULL;
		}

		minibuf_write ("%s", msg.s);

		Keystroke c = getkey (GETKEY_DEFAULT);

		if (c == KBD_CANCEL) {
			buffer_goto_offset (cur_bp, start);
			thisflag |= FLAG_NEED_RESYNC;

			/* Quit. */
			funcall ("keyboard-quit");

			/* Restore old mark position. */
			marker_free (cur_bp->mark);
			cur_bp->mark = marker_copy (old_mark);
			sb_free (&msg);
			break;
		} else if (c == KBD_BS) {
			if (pattern.len > 0) {
				pattern.len = utf8_prev_char (pattern.s, pattern.len, pattern.len);
				pattern.s[pattern.len] = '\0';
				cur = start;
				buffer_goto_offset (cur_bp, start);
				thisflag |= FLAG_NEED_RESYNC;
			} else
				ding ();
		} else if ((c & KBD_CTRL) && (c & 0xff) == 'q') {
			minibuf_write ("%s^Q-", msg.s);
			unsigned q = getkey_unfiltered (GETKEY_DEFAULT);
			if (q <= 0xff)
				sb_addc (&pattern, (char) q);
		} else if ((c & KBD_CTRL) && ((c & 0xff) == 'r' || (c & 0xff) == 's')) {
			/* Invert direction. */
			forward = (c & 0xff) == 's';
			if (pattern.len > 0) {
				/* Find next match. */
				cur = buffer_pt (cur_bp);
				/* Save search string. */
				set_last_search (pattern.s);
			} else if (last_search != NULL)
				sb_add (&pattern, last_search);
		} else if ((c & KBD_META) || (c & KBD_CTRL) || c > KBD_TAB) {
			if (c == KBD_RET && pattern.len == 0)
				do_search (forward, regexp, NULL);
			else {
				if (pattern.len > 0) {
					/* Save mark. */
					set_mark ();
					cur_bp->mark->o = start;

					/* Save search string. */
					set_last_search (pattern.s);

					minibuf_write ("Mark saved when search started");
				} else
					minibuf_clear ();
				if (c != KBD_RET)
					ungetkey (c);
			}
			sb_free (&msg);
			break;
		} else
			sb_addc (&pattern, (char) c);
		sb_free (&msg);

		if (pattern.len > 0) {
			buffer_goto_offset (cur_bp, cur);
			last = search (pattern.s, forward, regexp);
		} else
			last = true;
		show_matches (pattern.len > 0 ? pattern.s : "", regexp, pattern.len > 0 && last);

		if (thisflag & FLAG_NEED_RESYNC)
			window_resync (cur_wp);
		term_redisplay ();
	}

	/* done */
	hide_matches ();
	cur_wp->bp->isearch = false;

	marker_free (old_mark);
	sb_free (&pattern);

	return true;
}

/*
 * Check the case of a string.
 */
static enum Case
check_case (const Estr *a)
{
	size_t i;
	for (i = 0; i < a->len && isupper ((unsigned char) a->text[i]); i++)
		;
	if (i == a->len)
		return CASE_UPPER;
	else if (i == 1)
		for (; i < a->len && !isupper ((unsigned char) a->text[i]); i++)
			;
	return i == a->len ? CASE_CAPITALIZED : CASE_LOWER;
}

static bool
F_search_forward (long uniarg, Args *args)
{
	(void) uniarg;
	return do_search (true, false, args_poll (args));
}

static bool
F_search_backward (long uniarg, Args *args)
{
	(void) uniarg;
	return do_search (false, false, args_poll (args));
}

static bool
F_search_forward_regexp (long uniarg, Args *args)
{
	(void) uniarg;
	return do_search (true, true, args_poll (args));
}

static bool
F_search_backward_regexp (long uniarg, Args *args)
{
	(void) uniarg;
	return do_search (false, true, args_poll (args));
}

static bool
F_isearch_forward (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return isearch (true, lastflag & FLAG_SET_UNIARG);
}

static bool
F_isearch_backward (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return isearch (false, lastflag & FLAG_SET_UNIARG);
}

static bool
F_isearch_forward_regexp (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return isearch (true, !(lastflag & FLAG_SET_UNIARG));
}

static bool
F_isearch_backward_regexp (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return isearch (false, !(lastflag & FLAG_SET_UNIARG));
}

static bool
F_query_replace (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	bool ok = true;
	char *find = minibuf_read ("Query replace string: ", "");
	if (find == NULL)
		return funcall ("keyboard-quit");
	if (find[0] == '\0') {
		free (find);
		return false;
	}
	size_t find_len = strlen (find);
	bool find_no_upper = no_upper (find, find_len, false);

	char *repl = minibuf_read ("Query replace `%s' with: ", "", find);
	if (repl == NULL) {
		free (find);
		return funcall ("keyboard-quit");
	}

	bool noask = false;
	size_t count = 0;
	while (search (find, true, false)) {
		Keystroke c = ' ';

		if (!noask) {
			if (thisflag & FLAG_NEED_RESYNC)
				window_resync (cur_wp);

			show_matches (find, false, true);
			minibuf_write ("Query replacing `%s' with `%s' (y, n, !, ., q)? ", find, repl);
			c = getkey (GETKEY_DEFAULT);
			hide_matches ();
			minibuf_clear ();

			if (c == 'q')			/* Quit immediately. */
				break;
			else if (c == KBD_CANCEL) {
				ok = funcall ("keyboard-quit");
				break;
			} else if (c == '!')
				noask = true;
		}

		if (c == KBD_RET || c == ' ' || c == 'y' || c == 'Y' || c == '.' || c == '!') {
			/* Perform replacement. */
			++count;
			Region r = region_new (buffer_pt (cur_bp) - find_len, buffer_pt (cur_bp));
			char *case_repl = xstrdup (repl);
			if (find_no_upper && get_variable_bool ("case-replace")) {
				Estr *matched = buffer_get_region (cur_bp, r);
				enum Case case_type = check_case (matched);
				estr_free (matched);
				if (case_type != CASE_LOWER) {
					free (case_repl);
					case_repl = recase (repl, strlen (repl), case_type);
				}
			}

			Marker *m = marker_point ();
			buffer_goto_offset (cur_bp, r.start);
			buffer_replace_estr (cur_bp, find_len, ev_of (case_repl, strlen (case_repl), buffer_eol (cur_bp)));
			buffer_goto_offset (cur_bp, m->o);
			marker_free (m);
			free (case_repl);

			if (c == '.')		/* Replace and quit. */
				break;
		} else if (!(c == KBD_RET || c == KBD_DEL || c == 'n' || c == 'N')) {
			ungetkey (c);
			ok = false;
			break;
		}
	}

	if (thisflag & FLAG_NEED_RESYNC)
		window_resync (cur_wp);

	if (ok)
		minibuf_write ("Replaced %zu occurrences", count);

	free (find);
	free (repl);
	return ok;
}

void
search_init (void)
{
	lisp_func_new ("search-forward", F_search_forward, true,
		       "Search forward from point for the user specified text.");

	lisp_func_new ("search-backward", F_search_backward, true,
		       "Search backward from point for the user specified text.");

	lisp_func_new ("search-forward-regexp", F_search_forward_regexp, true,
		       "Search forward from point for regular expression REGEXP.");

	lisp_func_new ("search-backward-regexp", F_search_backward_regexp, true,
		       "Search backward from point for match for regular expression REGEXP.");

	lisp_func_new ("isearch-forward", F_isearch_forward, true,
		       "Do incremental search forward.\n"
		       "With a prefix argument, do an incremental regular expression search instead.\n"
		       "\n"
		       "As you type characters, they add to the search string and are found.\n"
		       "The match found is shown in the `isearch' face, the other matches on\n"
		       "the screen in the `lazy-highlight' face, and the prompt counts them:\n"
		       "\"2/7 I-search\" is at the second match of seven in the buffer.\n"
		       "\n"
		       "Type \\[isearch-exit] to exit, leaving point at location found.\n"
		       "Type \\[isearch-repeat-forward] to search again forward, \\[isearch-repeat-backward] to search again backward.\n"
		       "\\[isearch-abort] when search is successful aborts and moves point to starting point.");

	lisp_func_new ("isearch-backward", F_isearch_backward, true,
		       "Do incremental search backward.\n"
		       "With a prefix argument, do a regular expression search instead.\n"
		       "See the command `isearch-forward' for more information.");

	lisp_func_new ("isearch-forward-regexp", F_isearch_forward_regexp, true,
		       "Do incremental search forward for regular expression.\n"
		       "With a prefix argument, do a regular string search instead.\n"
		       "Like ordinary incremental search except that your input is treated\n"
		       "as a regexp.  See the command `isearch-forward' for more information.");

	lisp_func_new ("isearch-backward-regexp", F_isearch_backward_regexp, true,
		       "Do incremental search backward for regular expression.\n"
		       "With a prefix argument, do a regular string search instead.\n"
		       "Like ordinary incremental search except that your input is treated\n"
		       "as a regexp.  See the command `isearch-forward-regexp` for more information.");

	lisp_func_new ("query-replace", F_query_replace, true,
		       "Replace occurrences of a string with other text.\n"
		       "As each match is found, the user must type a character saying\n"
		       "what to do with it.");
}
