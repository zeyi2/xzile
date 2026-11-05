/* Interactive selection from a list of candidates

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

/* The picker shows candidates below the prompt, filtering them as the
   input changes. Matching is fuzzy: each space-separated term of the
   input must match, as a substring or else as a subsequence, and
   substrings, word starts and runs score higher. Upper case in the input
   makes matching case-sensitive.

   The first candidate is preselected only when it matches strongly (as
   a substring, or from a group that allows fuzzy preselection), so that
   RET with no selection uses the input as typed; C-j always does.

   In file mode the input is a path: the candidates are the entries of
   its directory, matched against its last component. The directory is
   on the spec's remote machine if there is one; M-l, if the spec
   allows, returns to ask for another location. */

#include "xzile.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#define MAX_ROWS 10
#define MAX_TERMS 8
/* Score candidates on several threads beyond this many. */
#define PARALLEL_SCORE_THRESHOLD 16384
#define SUBSTRING_SCORE 1000

struct Picker {
	PickerSpec spec;
	PickerItem *items;
	size_t nitems, cap;

	/* Candidates added by the asynchronous source. */
	pthread_t thread;
	bool thread_started;
	pthread_mutex_t lock;
	PickerItem *pending;
	size_t npending, pending_cap;
	bool async_done;	/* Guarded by `lock'. */

	/* The input. */
	StrBuf input;
	size_t pos;		/* Byte offset of the cursor. */

	/* Matches, in display order. */
	size_t *matches;
	int *scores;
	size_t nmatches;
	int selected;		/* Index in `matches', or -1. */
	bool moved;		/* The user moved the selection. */
	size_t top;		/* First row shown. */
	int previewed;		/* Item last previewed, or -1. */

	/* File mode. */
	char *dir;		/* The directory listed, as typed (e.g. "~/src/"). */
	char *list_error;	/* Why that directory could not be listed. */
	char *notice;		/* From the spec, until a key is typed. */
};

/*--------------------------------------------------------------------------
 * Candidates.
 *--------------------------------------------------------------------------*/

Picker *
picker_new (const PickerSpec *spec)
{
	Picker *p = xzalloc (sizeof (Picker));
	p->spec = *spec;
	pthread_mutex_init (&p->lock, NULL);
	sb_init (&p->input);
	p->selected = -1;
	p->previewed = -1;
	p->notice = xstrdup_or_null (spec->notice);
	return p;
}

static void
item_free (PickerItem *it)
{
	free (it->text);
	free (it->annotation);
}

static void
push_item (PickerItem **v, size_t *n, size_t *cap, const char *text, const char *annotation,
	   int group, void *data)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*v = xrealloc (*v, *cap * sizeof (PickerItem));
	}
	PickerItem *it = &(*v)[(*n)++];
	it->text = xstrdup (text);
	it->annotation = xstrdup_or_null (annotation);
	it->group = group;
	it->data = data;
}

void
picker_add (Picker *p, const char *text, const char *annotation, int group, void *data)
{
	push_item (&p->items, &p->nitems, &p->cap, text, annotation, group, data);
}

/* Add a candidate from the asynchronous source's thread. */
void
picker_add_async (Picker *p, const char *text, const char *annotation, int group)
{
	pthread_mutex_lock (&p->lock);
	push_item (&p->pending, &p->npending, &p->pending_cap, text, annotation, group, NULL);
	pthread_mutex_unlock (&p->lock);
}

PickerItem *
picker_item (Picker *p, int i)
{
	return i >= 0 && (size_t) i < p->nitems ? &p->items[i] : NULL;
}

static void *
async_thread (void *arg)
{
	Picker *p = arg;
	p->spec.async (p, p->spec.data);
	pthread_mutex_lock (&p->lock);
	p->async_done = true;
	pthread_mutex_unlock (&p->lock);
	return NULL;
}

static bool
async_finished (Picker *p)
{
	pthread_mutex_lock (&p->lock);
	bool done = p->async_done;
	pthread_mutex_unlock (&p->lock);
	return done;
}

/* Take the candidates produced so far by the asynchronous source. */
static bool
merge_pending (Picker *p)
{
	pthread_mutex_lock (&p->lock);
	bool any = p->npending > 0;
	for (size_t i = 0; i < p->npending; i++) {
		PickerItem *it = &p->pending[i];
		if (p->spec.accept_async == NULL || p->spec.accept_async (it->text, p->spec.data))
			push_item (&p->items, &p->nitems, &p->cap, it->text, it->annotation, it->group, NULL);
		item_free (it);
	}
	p->npending = 0;
	pthread_mutex_unlock (&p->lock);
	return any;
}

void
picker_free (Picker *p)
{
	if (p == NULL)
		return;
	if (p->thread_started)
		pthread_join (p->thread, NULL);
	merge_pending (p);
	for (size_t i = 0; i < p->nitems; i++)
		item_free (&p->items[i]);
	free (p->items);
	free (p->pending);
	free (p->matches);
	free (p->scores);
	sb_free (&p->input);
	free (p->dir);
	free (p->list_error);
	free (p->notice);
	pthread_mutex_destroy (&p->lock);
	free (p);
}


/*--------------------------------------------------------------------------
 * Matching.
 *--------------------------------------------------------------------------*/

typedef struct {
	const char *s;
	size_t len;
} Term;

static bool
is_boundary (const char *s, size_t i)
{
	if (i == 0)
		return true;
	char c = s[i - 1];
	return c == '/' || c == '-' || c == '_' || c == '.' || c == ' ' || c == '*' || c == ':' ||
		(islower ((unsigned char) c) && isupper ((unsigned char) s[i]));
}

static char
fold (char c, bool icase)
{
	return icase ? (char) tolower ((unsigned char) c) : c;
}

static size_t
find_substring (const char *s, size_t len, const Term *t, bool icase, size_t from)
{
	for (size_t i = from; i + t->len <= len; i++) {
		size_t k = 0;
		while (k < t->len && fold (s[i + k], icase) == fold (t->s[k], icase))
			k++;
		if (k == t->len)
			return i;
	}
	return NOPOS;
}

/* Score `t' against `s'; -1 if it does not match. If `pos' is not
   NULL, set the matched positions in it. Sets `*substring' if the
   term matched as a substring. */
static int
score_term (const char *s, size_t len, const Term *t, bool icase, bool *pos, bool *substring)
{
	const char *base = strrchr (s, '/');
	size_t base_start = base != NULL && (size_t) (base - s) + 1 < len ? (size_t) (base - s) + 1 : 0;

	/* The best substring occurrence: at a boundary if possible. */
	size_t best = NOPOS;
	for (size_t i = find_substring (s, len, t, icase, 0); i != NOPOS; i = find_substring (s, len, t, icase, i + 1)) {
		if (best == NOPOS)
			best = i;
		if (is_boundary (s, i)) {
			best = i;
			if (i == base_start)
				break;
		}
	}
	if (best != NOPOS) {
		int score = SUBSTRING_SCORE - (int) MIN (best, 200);
		if (is_boundary (s, best))
			score += 200;
		if (best == base_start)
			score += 250;
		if (best == 0)
			score += 300;
		if (t->len == len)
			score += 2000;
		if (pos != NULL)
			for (size_t k = 0; k < t->len; k++)
				pos[best + k] = true;
		*substring = true;
		return score;
	}

	/* A subsequence, matched greedily. */
	int score = 100;
	size_t i = 0, last = NOPOS;
	for (size_t k = 0; k < t->len; k++) {
		while (i < len && fold (s[i], icase) != fold (t->s[k], icase))
			i++;
		if (i == len)
			return -1;
		if (last != NOPOS && i == last + 1)
			score += 10;
		else if (last != NOPOS)
			score -= (int) MIN (i - last, 20);
		if (is_boundary (s, i))
			score += 8;
		if (pos != NULL)
			pos[i] = true;
		last = i++;
	}
	*substring = false;
	return MAX (score, 1);
}

typedef struct {
	Term terms[MAX_TERMS];
	size_t nterms;
	bool icase;
} Query;

static void
parse_query (const char *s, Query *q)
{
	q->nterms = 0;
	q->icase = true;
	for (const char *c = s; *c != '\0'; c++)
		if (isupper ((unsigned char) *c))
			q->icase = false;
	while (*s != '\0' && q->nterms < MAX_TERMS) {
		while (*s == ' ')
			s++;
		size_t n = strcspn (s, " ");
		if (n > 0) {
			q->terms[q->nterms].s = s;
			q->terms[q->nterms].len = n;
			q->nterms++;
		}
		s += n;
	}
}

/* Score a candidate against all terms; -1 if any fails. Strong matches
   (every term a substring) get a bonus. */
static int
score_item (const char *s, const Query *q, bool *pos)
{
	size_t len = strlen (s);
	int total = 0;
	bool all_substrings = true;
	for (size_t i = 0; i < q->nterms; i++) {
		bool substring;
		int sc = score_term (s, len, &q->terms[i], q->icase, pos, &substring);
		if (sc < 0)
			return -1;
		total += sc;
		all_substrings = all_substrings && substring;
	}
	return total + (all_substrings ? 0 : 0);
}

static bool
strong_match (const char *s, const Query *q)
{
	size_t len = strlen (s);
	for (size_t i = 0; i < q->nterms; i++)
		if (find_substring (s, len, &q->terms[i], q->icase, 0) == NOPOS)
			return false;
	return true;
}

typedef struct {
	const PickerItem *items;
	const Query *q;
	size_t from, to;
	int *scores;
} ScoreJob;

static void *
score_thread (void *arg)
{
	ScoreJob *j = arg;
	for (size_t i = j->from; i < j->to; i++)
		j->scores[i] = score_item (j->items[i].text, j->q, NULL);
	return NULL;
}

/* Score all items, in parallel when there are many. */
static void
score_all (Picker *p, const Query *q, int *scores)
{
	size_t n = p->nitems;
	long ncpu = sysconf (_SC_NPROCESSORS_ONLN);
	size_t nthreads = n > PARALLEL_SCORE_THRESHOLD && ncpu > 1 ? MIN ((size_t) ncpu, 16) : 1;
	ScoreJob jobs[16];
	pthread_t threads[16];
	bool started[16] = { false };
	for (size_t t = 0; t < nthreads; t++) {
		jobs[t].items = p->items;
		jobs[t].q = q;
		jobs[t].from = n * t / nthreads;
		jobs[t].to = n * (t + 1) / nthreads;
		jobs[t].scores = scores;
		if (t > 0)
			started[t] = pthread_create (&threads[t], NULL, score_thread, &jobs[t]) == 0;
	}
	score_thread (&jobs[0]);
	for (size_t t = 1; t < nthreads; t++) {
		if (started[t])
			pthread_join (threads[t], NULL);
		else
			score_thread (&jobs[t]);
	}
}

static const Picker *sorting;

static int
compare_matches (const void *a, const void *b)
{
	size_t ia = *(const size_t *) a, ib = *(const size_t *) b;
	const PickerItem *x = &sorting->items[ia], *y = &sorting->items[ib];
	/* Groups only order candidates when they are shown. */
	if (sorting->spec.groups != NULL && x->group != y->group)
		return x->group < y->group ? -1 : 1;
	int sx = sorting->scores[ia], sy = sorting->scores[ib];
	if (sx != sy)
		return sx > sy ? -1 : 1;
	return ia < ib ? -1 : ia > ib ? 1 : 0;
}

/* The part of the input that is matched. */
static const char *
query_text (Picker *p)
{
	if (!p->spec.file_mode)
		return p->input.s;
	const char *slash = strrchr (p->input.s, '/');
	return slash != NULL ? slash + 1 : p->input.s;
}

static void
filter (Picker *p)
{
	const char *qs = query_text (p);
	Query q;
	parse_query (qs, &q);
	bool show_hidden = !p->spec.file_mode || qs[0] == '.';

	free (p->scores);
	p->scores = xmalloc ((p->nitems ? p->nitems : 1) * sizeof (int));
	if (q.nterms == 0)
		for (size_t i = 0; i < p->nitems; i++)
			p->scores[i] = 0;
	else
		score_all (p, &q, p->scores);

	free (p->matches);
	p->matches = xmalloc ((p->nitems ? p->nitems : 1) * sizeof (size_t));
	p->nmatches = 0;
	for (size_t i = 0; i < p->nitems; i++)
		if (p->scores[i] >= 0 && (show_hidden || p->items[i].text[0] != '.'))
			p->matches[p->nmatches++] = i;
	if (q.nterms > 0) {
		sorting = p;
		qsort (p->matches, p->nmatches, sizeof (size_t), compare_matches);
	} else if (p->spec.groups != NULL) {
		sorting = p;
		/* Keep the given order within groups. */
		for (size_t i = 0; i < p->nitems; i++)
			p->scores[i] = 0;
		qsort (p->matches, p->nmatches, sizeof (size_t), compare_matches);
	}

	/* Preselect only strong matches, or the default. */
	p->selected = -1;
	p->top = 0;
	p->moved = false;
	if (p->nmatches == 0)
		return;
	if (q.nterms == 0) {
		if (p->spec.default_item >= 0)
			for (size_t i = 0; i < p->nmatches; i++)
				if ((int) p->matches[i] == p->spec.default_item) {
					p->selected = (int) i;
					break;
				}
		if (p->spec.require_match && p->selected < 0)
			p->selected = 0;
		return;
	}
	/* Groups come before scores: an exact match may not be first. */
	for (size_t i = 0; i < p->nmatches; i++)
		if (strcmp (p->items[p->matches[i]].text, qs) == 0) {
			p->selected = (int) i;
			return;
		}
	const PickerItem *first = &p->items[p->matches[0]];
	bool fuzzy_ok = p->spec.require_match ||
		(p->spec.fuzzy_groups != 0 && (p->spec.fuzzy_groups & (1u << first->group)));
	if (p->spec.preselect && (fuzzy_ok || strong_match (first->text, &q)))
		p->selected = 0;
}


/*--------------------------------------------------------------------------
 * File mode.
 *--------------------------------------------------------------------------*/

/* List the directory of the input, if it has changed. */
static void
load_directory (Picker *p)
{
	const char *slash = strrchr (p->input.s, '/');
	size_t dlen = slash != NULL ? (size_t) (slash - p->input.s) + 1 : 0;
	if (p->dir != NULL && strlen (p->dir) == dlen && strncmp (p->dir, p->input.s, dlen) == 0)
		return;
	free (p->dir);
	p->dir = xstrndup (p->input.s, dlen);

	for (size_t i = 0; i < p->nitems; i++)
		item_free (&p->items[i]);
	p->nitems = 0;
	free (p->list_error);
	p->list_error = NULL;

	if (p->spec.remote != NULL) {
		/* Never the local directory of the same name. */
		char *rpath = remote_expand_path (p->spec.remote, dlen > 0 ? p->dir : "~/");
		RemoteEntry *v = NULL;
		size_t n = 0;
		if (rpath != NULL && remote_list (p->spec.remote, rpath, false, &v, &n)) {
			for (size_t i = 0; i < n; i++) {
				if (strcmp (v[i].name, ".") == 0 || strcmp (v[i].name, "..") == 0)
					continue;
				if (v[i].dir) {
					char *s = xstrcat3 (v[i].name, "/", "");
					picker_add (p, s, NULL, 0, NULL);
					free (s);
				} else
					picker_add (p, v[i].name, NULL, 1, NULL);
			}
		} else
			p->list_error = xstrdup (remote_last_error ());
		remote_entries_free (v, n);
		free (rpath);
	}

	char *path = p->spec.remote == NULL ? expand_path (dlen > 0 ? p->dir : "./") : NULL;
	DIR *d = path != NULL ? opendir (path) : NULL;
	if (d != NULL) {
		struct dirent *e;
		while ((e = readdir (d)) != NULL) {
			if (strcmp (e->d_name, ".") == 0 || strcmp (e->d_name, "..") == 0)
				continue;
			char *full = path_join (path, e->d_name);
			bool dir = file_is_dir (full);
			free (full);
			if (dir) {
				char *s = xstrcat3 (e->d_name, "/", "");
				picker_add (p, s, NULL, 0, NULL);
				free (s);
			} else
				picker_add (p, e->d_name, NULL, 1, NULL);
		}
		closedir (d);
	}
	free (path);

	/* Directories first, each group alphabetically. */
	for (size_t i = 1; i < p->nitems; i++)
		for (size_t j = i; j > 0; j--) {
			PickerItem *a = &p->items[j - 1], *b = &p->items[j];
			if (a->group < b->group || (a->group == b->group && strcmp (a->text, b->text) <= 0))
				break;
			PickerItem t = *a;
			*a = *b;
			*b = t;
		}
}

/* The input with candidate `it' in place of the query. */
static char *
file_choice (Picker *p, const PickerItem *it)
{
	return xstrcat3 (p->dir != NULL ? p->dir : "", it->text, "");
}


/*--------------------------------------------------------------------------
 * Display.
 *--------------------------------------------------------------------------*/

static size_t
rows_available (void)
{
	size_t h = term_height ();
	return h > 6 ? MIN ((size_t) MAX_ROWS, h - 5) : 1;
}

typedef struct {
	bool header;
	size_t match;		/* Index in matches, for items. */
	int group;
} Row;

/* Rows: items, with a header when the group changes. */
static size_t
build_rows (Picker *p, Row *rows, size_t max)
{
	size_t n = 0;
	int group = -1;
	for (size_t i = 0; i < p->nmatches && n < max; i++) {
		const PickerItem *it = &p->items[p->matches[i]];
		if (p->spec.groups != NULL && it->group != group) {
			group = it->group;
			rows[n].header = true;
			rows[n].group = group;
			rows[n].match = i;
			n++;
			if (n == max)
				break;
		}
		rows[n].header = false;
		rows[n].match = i;
		rows[n].group = it->group;
		n++;
	}
	return n;
}

static size_t
display_width (const char *s, size_t len)
{
	size_t w = 0;
	for (size_t i = 0; i < len;) {
		size_t cl;
		uint32_t c = utf8_decode (s, len, i, &cl);
		w += (size_t) utf8_char_display_width (c);
		i += cl ? cl : 1;
	}
	return w;
}

/* Add `s', at most `width' columns, highlighting matched characters. */
static void
add_text (const char *s, const bool *pos, size_t width, const char *face)
{
	size_t len = strlen (s), col = 0;
	for (size_t i = 0; i < len;) {
		size_t cl;
		uint32_t c = utf8_decode (s, len, i, &cl);
		size_t w = (size_t) utf8_char_display_width (c);
		if (col + w > width)
			break;
		term_apply_face (pos != NULL && pos[i] ? FACE_PICKER_MATCH : face);
		term_addnstr (s + i, cl ? cl : 1);
		col += w;
		i += cl ? cl : 1;
	}
	term_apply_face (face);
	for (; col < width; col++)
		term_addch (' ');
}

static void
draw (Picker *p)
{
	size_t width = term_width (), height = term_height ();
	size_t nrows = rows_available ();
	size_t first_row = height - 1 - nrows;

	/* The prompt line. */
	term_move (first_row, 0);
	term_apply_face (FACE_DEFAULT);
	char *count = xasprintf ("%zu/%zu ", p->nmatches, p->nitems);
	term_apply_face (FACE_PICKER_ANNOTATION);
	term_addstr (count);
	term_apply_face (FACE_MINIBUFFER_PROMPT);
	term_addstr (p->spec.prompt);
	term_apply_face (FACE_DEFAULT);
	size_t prompt_w = display_width (count, strlen (count)) + display_width (p->spec.prompt, strlen (p->spec.prompt));
	free (count);
	size_t avail = width > prompt_w + 1 ? width - prompt_w - 1 : 1;
	/* Scroll the input to keep the cursor visible. */
	size_t cursor_w = display_width (p->input.s, p->pos), start = 0;
	while (cursor_w - display_width (p->input.s, start) >= avail && start < p->pos)
		start = utf8_next_char (p->input.s, p->input.len, start);
	add_text (p->input.s + start, NULL, avail, FACE_DEFAULT);
	bool show_new = !p->spec.require_match && p->selected < 0 && p->input.len > 0 && !p->spec.file_mode;
	const char *message = p->list_error != NULL ? p->list_error : p->notice;
	if (message != NULL) {
		char *hint = xasprintf ("  [%s]", message);
		size_t used = prompt_w + display_width (p->input.s + start, p->input.len - start);
		size_t room = width > used + 2 ? width - used - 2 : 0;
		room = MAX (room, MIN (width / 2, (size_t) 40));
		size_t hw = MIN (display_width (hint, strlen (hint)), room);
		if (width > hw + 1) {
			term_move (first_row, width - hw - 1);
			add_text (hint, NULL, hw, FACE_ERROR);
		}
		free (hint);
	} else if (show_new || p->spec.hints != NULL) {
		const char *hint = "  [RET: use input]";
		if (!show_new) {
			/* The longest that does not hide the input, if any. */
			size_t used = prompt_w + display_width (p->input.s + start, p->input.len - start) + 1;
			size_t k = 0;
			while (p->spec.hints[k] != NULL && used + strlen (p->spec.hints[k]) + 1 > width)
				k++;
			hint = p->spec.hints[k] != NULL ? p->spec.hints[k] : "";
		}
		size_t hw = strlen (hint);
		if (hw > 0 && width > hw) {
			term_move (first_row, width - hw - 1);
			term_apply_face (FACE_PICKER_ANNOTATION);
			term_addstr (hint);
		}
	}
	size_t cursor_x = prompt_w + display_width (p->input.s + start, p->pos - start);

	/* The candidates. */
	Row *rows = xmalloc ((p->nmatches * 2 + 1) * sizeof (Row));
	size_t total = build_rows (p, rows, p->nmatches * 2 + 1);
	size_t sel_row = 0;
	for (size_t i = 0; i < total; i++)
		if (!rows[i].header && (int) rows[i].match == p->selected)
			sel_row = i;
	if (p->selected >= 0) {
		if (sel_row < p->top)
			p->top = sel_row > 0 && rows[sel_row - 1].header ? sel_row - 1 : sel_row;
		if (sel_row >= p->top + nrows)
			p->top = sel_row - nrows + 1;
	}
	if (p->top > total)
		p->top = 0;

	size_t text_w = 0;
	for (size_t r = p->top; r < total && r < p->top + nrows; r++)
		if (!rows[r].header)
			text_w = MAX (text_w, display_width (p->items[p->matches[rows[r].match]].text,
							      strlen (p->items[p->matches[rows[r].match]].text)));
	text_w = MIN (text_w + 2, width * 3 / 5);

	Query q;
	parse_query (query_text (p), &q);
	for (size_t r = 0; r < nrows; r++) {
		term_move (first_row + 1 + r, 0);
		size_t ri = p->top + r;
		if (ri >= total) {
			term_apply_face (FACE_DEFAULT);
			for (size_t x = 0; x < width; x++)
				term_addch (' ');
			continue;
		}
		if (rows[ri].header) {
			const char *title = p->spec.groups[rows[ri].group];
			add_text (title, NULL, width, FACE_PICKER_GROUP);
			continue;
		}
		size_t idx = p->matches[rows[ri].match];
		const PickerItem *it = &p->items[idx];
		bool sel = (int) rows[ri].match == p->selected;
		const char *face = sel ? FACE_PICKER_SELECTION : FACE_DEFAULT;
		size_t len = strlen (it->text);
		bool *pos = xzalloc (len + 1);
		if (q.nterms > 0)
			score_item (it->text, &q, pos);
		term_apply_face (face);
		term_addch (sel ? '>' : ' ');
		add_text (it->text, pos, text_w, face);
		free (pos);
		size_t rest = width > text_w + 1 ? width - text_w - 1 : 0;
		add_text (it->annotation != NULL ? it->annotation : "", NULL, rest,
			  sel ? FACE_PICKER_SELECTION : FACE_PICKER_ANNOTATION);
	}
	free (rows);
	term_attrset (FONT_NORMAL);
	term_set_cursor_visible (true);
	term_move (first_row, MIN (cursor_x, width - 1));
}

static void
redraw_all (Picker *p)
{
	term_redisplay ();
	draw (p);
	term_refresh ();
}


/*--------------------------------------------------------------------------
 * The loop.
 *--------------------------------------------------------------------------*/

static void
set_input (Picker *p, const char *s)
{
	sb_free (&p->input);
	sb_init (&p->input);
	sb_add (&p->input, s);
	p->pos = p->input.len;
}

static void
splice_input (Picker *p, size_t from, size_t to, const char *s, size_t n)
{
	StrBuf sb;
	sb_init (&sb);
	sb_addn (&sb, p->input.s, from);
	sb_addn (&sb, s, n);
	sb_add (&sb, p->input.s + to);
	sb_free (&p->input);
	p->input = sb;
	p->pos = from + n;
}

static void
preview (Picker *p)
{
	if (p->spec.preview == NULL)
		return;
	int item = p->selected >= 0 ? (int) p->matches[p->selected] : -1;
	if (item != p->previewed && item >= 0) {
		p->spec.preview (&p->items[item], p->spec.data);
		p->previewed = item;
	}
}

static void
move_selection (Picker *p, long delta)
{
	if (p->nmatches == 0)
		return;
	long s = p->selected < 0 ? (delta > 0 ? -1 : 0) : p->selected;
	s += delta;
	s = MAX (0, MIN (s, (long) p->nmatches - 1));
	p->selected = (int) s;
	p->moved = true;
}

/* Remove item `idx' from the candidates. */
static void
remove_item (Picker *p, size_t idx)
{
	item_free (&p->items[idx]);
	memmove (p->items + idx, p->items + idx + 1, (p->nitems - idx - 1) * sizeof (PickerItem));
	p->nitems--;
	if (p->spec.default_item > (int) idx)
		p->spec.default_item--;
	else if (p->spec.default_item == (int) idx)
		p->spec.default_item = -1;
	p->previewed = -1;
}

void
picker_run (Picker *p, PickerChoice *choice)
{
	memset (choice, 0, sizeof *choice);
	choice->kind = PICK_CANCEL;
	choice->item = -1;

	set_input (p, p->spec.initial != NULL ? p->spec.initial : "");
	if (p->spec.async != NULL && !p->thread_started)
		p->thread_started = pthread_create (&p->thread, NULL, async_thread, p) == 0;
	if (p->spec.history != NULL)
		history_prepare (p->spec.history);

	minibuf_set_reserved_height (rows_available () + 1);
	term_minibuf_enter ();

	Utf8Pending utf8 = { { 0 }, 0, 0 };
	bool dirty = true;
	for (;;) {
		if (p->spec.file_mode)
			load_directory (p);
		if (dirty) {
			int keep = p->moved && p->selected >= 0 ? (int) p->matches[p->selected] : -1;
			filter (p);
			if (keep >= 0)
				for (size_t i = 0; i < p->nmatches; i++)
					if ((int) p->matches[i] == keep) {
						p->selected = (int) i;
						p->moved = true;
					}
			dirty = false;
		}
		preview (p);
		redraw_all (p);

		bool waiting = p->thread_started && !async_finished (p);
		Keystroke key = getkeystroke (waiting ? 50 : GETKEY_DEFAULT);
		if (key == KBD_NOKEY) {
			if (merge_pending (p) || async_finished (p))
				dirty = true;
			continue;
		}
		if (merge_pending (p))
			dirty = true;
		free (p->notice);
		p->notice = NULL;

		const PickerItem *sel = p->selected >= 0 ? &p->items[p->matches[p->selected]] : NULL;
		switch (key) {
		case KBD_CANCEL:
			goto done;
		case KBD_META | 'l':
			if (p->spec.locations) {
				choice->kind = PICK_LOCATION;
				goto done;
			}
			if (p->spec.back_key)
				goto done;
			ding ();
			break;
		case KBD_RET:
			if (sel != NULL) {
				choice->kind = PICK_ITEM;
				choice->item = (int) p->matches[p->selected];
				if (p->spec.file_mode)
					choice->input = file_choice (p, sel);
				goto done;
			}
			if (p->spec.require_match && p->nmatches > 0) {
				choice->kind = PICK_ITEM;
				choice->item = (int) p->matches[0];
				goto done;
			}
			if (!p->spec.require_match) {
				choice->kind = PICK_INPUT;
				goto done;
			}
			ding ();
			break;
		case KBD_CTRL | 'j':
			if (!p->spec.require_match) {
				choice->kind = PICK_INPUT;
				goto done;
			}
			ding ();
			break;
		case KBD_CTRL | 'o':
			if (p->spec.other_window && sel != NULL) {
				choice->kind = PICK_ITEM;
				choice->item = (int) p->matches[p->selected];
				choice->other_window = true;
				goto done;
			}
			ding ();
			break;
		case KBD_TAB:
			if (sel == NULL && p->nmatches > 0)
				sel = &p->items[p->matches[0]];
			if (sel != NULL) {
				char *s = p->spec.file_mode ? file_choice (p, sel) : xstrdup (sel->text);
				set_input (p, s);
				free (s);
				dirty = true;
			} else
				ding ();
			break;
		case KBD_CTRL | 'n':
		case KBD_DOWN:
			move_selection (p, 1);
			break;
		case KBD_CTRL | 'p':
		case KBD_UP:
			move_selection (p, -1);
			break;
		case KBD_CTRL | 'v':
		case KBD_PGDN:
			move_selection (p, (long) rows_available ());
			break;
		case KBD_META | 'v':
		case KBD_PGUP:
			move_selection (p, -(long) rows_available ());
			break;
		case KBD_META | '<':
			move_selection (p, -(long) p->nmatches);
			break;
		case KBD_META | '>':
			move_selection (p, (long) p->nmatches);
			break;
		case KBD_CTRL | 'a':
		case KBD_HOME:
			p->pos = 0;
			break;
		case KBD_CTRL | 'e':
		case KBD_END:
			p->pos = p->input.len;
			break;
		case KBD_CTRL | 'b':
		case KBD_LEFT:
			p->pos = utf8_prev_char (p->input.s, p->input.len, p->pos);
			break;
		case KBD_CTRL | 'f':
		case KBD_RIGHT:
			p->pos = utf8_next_char (p->input.s, p->input.len, p->pos);
			break;
		case KBD_BS:
			if (p->pos == 0) {
				ding ();
				break;
			}
			if (p->spec.file_mode && p->pos == p->input.len && p->input.s[p->pos - 1] == '/' &&
			    p->pos > 1) {
				/* Delete the whole last directory. */
				size_t i = p->pos - 1;
				while (i > 0 && p->input.s[i - 1] != '/')
					i--;
				splice_input (p, i, p->pos, "", 0);
			} else
				splice_input (p, utf8_prev_char (p->input.s, p->input.len, p->pos), p->pos, "", 0);
			dirty = true;
			break;
		case KBD_CTRL | 'd':
		case KBD_DEL:
			if (p->pos < p->input.len) {
				splice_input (p, p->pos, utf8_next_char (p->input.s, p->input.len, p->pos), "", 0);
				dirty = true;
			} else
				ding ();
			break;
		case KBD_CTRL | 'k':
			if (p->spec.kill != NULL) {
				if (sel != NULL) {
					size_t idx = p->matches[p->selected];
					if (p->spec.kill (&p->items[idx], p->spec.data)) {
						remove_item (p, idx);
						int keep_pos = p->selected;
						filter (p);
						if (p->nmatches > 0) {
							p->selected = MIN (keep_pos, (int) p->nmatches - 1);
							p->moved = true;
						}
					}
				} else
					ding ();
			} else {
				p->input.len = p->pos;
				p->input.s[p->pos] = '\0';
				dirty = true;
			}
			break;
		case KBD_CTRL | 'y':
			if (kill_ring_text != NULL) {
				splice_input (p, p->pos, p->pos, kill_ring_text->text, strlen (kill_ring_text->text));
				dirty = true;
			} else
				ding ();
			break;
		case KBD_META | 'p':
		case KBD_META | 'n':
			if (p->spec.history != NULL) {
				const char *h = key == (KBD_META | 'p') ? history_previous_element (p->spec.history)
					: history_next_element (p->spec.history);
				set_input (p, h != NULL ? h : "");
				dirty = true;
			} else
				ding ();
			break;
		default:
			if (key <= 0xff && (key >= 0x20 || key >= 0x80)) {
				char ch[4];
				size_t n;
				if (utf8_pending_feed (&utf8, (unsigned char) key, ch, &n)) {
					splice_input (p, p->pos, p->pos, ch, n);
					dirty = true;
				}
			} else
				ding ();
			break;
		}
	}

done:
	if (choice->kind != PICK_CANCEL && choice->input == NULL)
		choice->input = xstrdup (p->input.s);
	if (choice->kind == PICK_ITEM && p->spec.history != NULL)
		history_add_element (p->spec.history, p->items[choice->item].text);
	term_minibuf_leave ();
	minibuf_set_reserved_height (0);
	minibuf_clear ();
}
