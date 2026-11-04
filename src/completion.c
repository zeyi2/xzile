/* Completion facility functions

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

#include <dirent.h>

Completion *
completion_new (bool fileflag)
{
	Completion *cp = xzalloc (sizeof (Completion));
	if (fileflag) {
		cp->path = xstrdup ("");
		cp->flags |= COMPLETION_FILENAME;
	}
	return cp;
}

void
completion_free (Completion *cp)
{
	if (cp == NULL)
		return;
	sv_clear (&cp->completions);
	sv_clear (&cp->matches);
	marker_free (cp->old_pt);
	free (cp->match);
	free (cp->path);
	free (cp);
}

static void
scroll_completions (const char *scroll, const char *wrap)
{
	Window *old_wp = cur_wp;
	Window *wp = window_find ("*Completions*");
	if (wp == NULL)
		return;
	window_set_current (wp);
	if (!funcall (scroll)) {
		funcall (wrap);
		window_resync (cur_wp);
	}
	window_set_current (old_wp);

	term_redisplay ();
}

void
completion_scroll_up (void)
{
	scroll_completions ("scroll-up", "beginning-of-buffer");
}

void
completion_scroll_down (void)
{
	scroll_completions ("scroll-down", "end-of-buffer");
}

static void
set_match (Completion *cp, const char *s, size_t matchsize)
{
	free (cp->match);
	cp->match = xstrdup (s);
	cp->matchsize = matchsize;
}

typedef struct {
	const StrVec *l;
} PopupData;

static void
write_completions (void *data)
{
	const StrVec *l = ((PopupData *) data)->l;

	/* Print the list of completions in a set of columns. */
	size_t max = 0;
	for (size_t i = 0; i < l->n; i++)
		max = MAX (max, strlen (l->v[i]));
	max += 5;
	size_t numcols = cur_wp->ewidth > 1 ? (cur_wp->ewidth - 1) / max : 0;
	if (numcols == 0)
		numcols = 1;

	bprintf ("Possible completions are:\n");
	size_t col = 0;
	for (size_t i = 0; i < l->n; i++) {
		bprintf ("%-*s", (int) max, l->v[i]);

		col = (col + 1) % numcols;
		if (col == 0)
			insert_newline ();
	}
}

static void
popup_completion (Completion *cp, bool allflag)
{
	cp->flags |= COMPLETION_POPPEDUP;
	if (head_wp->next == NULL)
		cp->flags |= COMPLETION_CLOSE;
	else if (cp->old_bp == NULL && window_find ("*Completions*") == NULL) {
		/* Remember what the popup window showed, to restore it later. */
		Window *wp = cur_wp->next != NULL ? cur_wp->next : head_wp;
		cp->old_bp = wp->bp;
		cp->old_pt = marker_copy (wp->saved_pt);
	}

	PopupData data = { allflag ? &cp->completions : &cp->matches };
	write_temp_buffer ("*Completions*", true, write_completions, &data);

	term_redisplay ();
}

/*
 * Reread directory for completions. Returns the basename part of
 * `in_path' (to be completed), or NULL on error.
 */
static char *
completion_readdir (Completion *cp, const char *in_path)
{
	sv_clear (&cp->completions);

	char *path = expand_path (in_path);
	if (path == NULL)
		return NULL;

	/* Split up path with dirname and basename, unless it ends in `/',
	   in which case it's considered to be entirely dirname. */
	char *pdir, *base;
	size_t len = strlen (path);
	if (len == 0 || path[len - 1] != '/') {
		char *d = path_dirname (path);
		pdir = strcmp (d, "/") != 0 ? xstrcat3 (d, "/", "") : xstrdup (d);
		free (d);
		base = path_basename (path);
	} else {
		pdir = xstrdup (path);
		base = xstrdup ("");
	}
	free (path);

	DIR *dir = opendir (pdir);
	if (dir != NULL) {
		struct dirent *d;
		while ((d = readdir (dir)) != NULL) {
			if (strcmp (d->d_name, ".") == 0 || strcmp (d->d_name, "..") == 0)
				continue;
			char *p = path_join (pdir, d->d_name);
			if (file_is_dir (p)) {
				char *s = xstrcat3 (d->d_name, "/", "");
				sv_add_sorted (&cp->completions, s);
				free (s);
			} else
				sv_add_sorted (&cp->completions, d->d_name);
			free (p);
		}
		closedir (dir);
	}

	free (cp->path);
	cp->path = compact_path (pdir);
	free (pdir);

	if (dir == NULL) {
		free (base);
		return NULL;
	}
	return base;
}

/*
 * Match completions.
 */
enum CompletionCode
completion_try (Completion *cp, const char *in_search, bool popup_when_complete)
{
	sv_clear (&cp->matches);

	char *search;
	if (cp->flags & COMPLETION_FILENAME) {
		if ((search = completion_readdir (cp, in_search)) == NULL)
			return COMPLETION_NOTMATCHED;
	} else
		search = xstrdup (in_search);

	enum CompletionCode ret;
	size_t slen = strlen (search);

	if (slen == 0) {
		if (cp->completions.n == 0)
			ret = COMPLETION_NOTMATCHED;
		else if (cp->completions.n > 1) {
			set_match (cp, cp->completions.v[0], 0);
			popup_completion (cp, true);
			ret = COMPLETION_NONUNIQUE;
		} else {
			set_match (cp, cp->completions.v[0], strlen (cp->completions.v[0]));
			ret = COMPLETION_MATCHED;
		}
		free (search);
		return ret;
	}

	size_t fullmatches = 0;
	for (size_t i = 0; i < cp->completions.n; i++) {
		const char *s = cp->completions.v[i];
		if (strncmp (s, search, slen) == 0) {
			sv_add_sorted (&cp->matches, s);
			if (strcmp (s, search) == 0)
				++fullmatches;
		}
	}

	if (cp->matches.n == 0)
		ret = COMPLETION_NOTMATCHED;
	else if (cp->matches.n == 1) {
		set_match (cp, cp->matches.v[0], strlen (cp->matches.v[0]));
		ret = COMPLETION_MATCHED;
	} else if (fullmatches == 1) {
		set_match (cp, cp->matches.v[0], strlen (cp->matches.v[0]));
		if (popup_when_complete)
			popup_completion (cp, false);
		ret = COMPLETION_MATCHEDNONUNIQUE;
	} else {
		const char *first = cp->matches.v[0];
		size_t i;
		for (i = slen;; ++i) {
			char c = first[i];
			bool differ = false;
			for (size_t k = 1; k < cp->matches.n; k++)
				if (cp->matches.v[k][i] != c) {
					differ = true;
					break;
				}
			if (differ || c == '\0')
				break;
		}
		set_match (cp, first, i);
		popup_completion (cp, false);
		ret = COMPLETION_NONUNIQUE;
	}

	free (search);
	return ret;
}
