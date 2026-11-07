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
#include "tui.h"

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
	bool follow;		/* Scroll to show the selection. */
	long wheel;		/* Rows to scroll by, from the mouse wheel. */
	int clicked;		/* Match chosen with the mouse, or -1. */
	int cursor_y, cursor_x;	/* Where the input's cursor was drawn. */
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
	p->follow = true;
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

/* The picker is a panel docked at the bottom: a rounded border
   with the prompt and the count in its top edge and the keys in its
   bottom edge, around the input, a blank line, and the candidates with
   a scroll bar. A screen too short for that gets the input and the
   candidates alone. */

#define PICKER_KIND 0x50	/* For region keys. */

static bool
compact (void)
{
	return term_height () < 14;
}

static size_t
rows_available (void)
{
	size_t h = term_height ();
	if (compact ())
		return h > 6 ? MIN ((size_t) MAX_ROWS, h - 5) : 1;
	return MIN ((size_t) MAX_ROWS, h - 9);
}

/* The rows of the screen the picker takes, at the bottom. */
static size_t
panel_height (void)
{
	return rows_available () + (compact () ? 1 : 4);
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
	return (size_t) tui_width (s, len);
}

/* The input line: the prompt (when there is no border to show it), the
   input scrolled to show the cursor, and a message or hint on the
   right. */
static void
paint_input (TuiRect r, void *data)
{
	Picker *p = data;
	int x = r.x, end = r.x + r.w;
	if (compact ()) {
		char *count = xasprintf ("%zu/%zu ", p->nmatches, p->nitems);
		x += tui_put (r.y, x, end - x, count, FACE_TUI_MUTED, 0);
		x += tui_put (r.y, x, end - x, p->spec.prompt, FACE_MINIBUFFER_PROMPT, 0);
		free (count);
	} else {
		x += tui_put (r.y, x, end - x, " ", NULL, 0);
		x += tui_put (r.y, x, end - x, tui_unicode () ? "❯ " : "> ", FACE_TUI_KEY, 0);
	}

	/* What goes on the right, if it leaves room for the input. */
	const char *message = p->list_error != NULL ? p->list_error : p->notice;
	bool show_new = !p->spec.require_match && p->selected < 0 && p->input.len > 0 && !p->spec.file_mode;
	const char *right = message != NULL ? message : show_new ? "RET uses the input" : NULL;
	int rw = right != NULL ? tui_width (right, strlen (right)) + 2 : 0;
	int avail = end - x - 1;
	if (rw > 0 && avail - rw < (int) MAX (display_width (p->input.s, p->input.len), (size_t) 12) + 1)
		rw = 0;
	if (rw > 0) {
		tui_put (r.y, end - rw, rw - 1, right, message != NULL ? FACE_ERROR : FACE_TUI_MUTED, 0);
		avail -= rw;
	}

	/* Scroll the input to keep the cursor visible. */
	avail = MAX (avail, 1);
	size_t cursor_w = display_width (p->input.s, p->pos), start = 0;
	while (cursor_w - display_width (p->input.s, start) >= (size_t) avail && start < p->pos)
		start = utf8_next_char (p->input.s, p->input.len, start);
	tui_put (r.y, x, avail, p->input.s + start, NULL, 0);
	p->cursor_y = r.y;
	p->cursor_x = x + (int) display_width (p->input.s + start, p->pos - start);
}

/* A candidate was clicked: select it, or choose it if it was selected
   already or clicked twice. */
static bool
item_event (const TuiEvent *ev, void *data)
{
	Picker *p = data;
	if (ev->kind == TUI_WHEEL) {
		p->wheel += ev->wheel * 3;
		return true;
	}
	if (ev->kind != TUI_PRESS)
		return true;
	if ((size_t) ev->a >= p->nmatches)
		return true;
	if (p->selected == (int) ev->a || ev->clicks >= 2)
		p->clicked = (int) ev->a;
	p->selected = (int) ev->a;
	p->moved = true;
	return true;
}

static void
scroll_to (double offset, void *data, long a, long b)
{
	(void) a;
	(void) b;
	Picker *p = data;
	p->top = (size_t) (offset + 0.5);
	p->follow = false;
}

/* The text of item `it', with the characters that `q' matched in
   their face, in spans added to `spans'. */
static size_t
item_spans (const PickerItem *it, const Query *q, bool sel, TuiSpan *spans, char **bufs, size_t max)
{
	size_t len = strlen (it->text), n = 0;
	bool *pos = xzalloc (len + 1);
	if (q->nterms > 0)
		score_item (it->text, q, pos);
	for (size_t i = 0; i < len && n < max;) {
		size_t j = i;
		while (j < len && pos[j] == pos[i])
			j++;
		bufs[n] = xstrndup (it->text + i, j - i);
		spans[n].text = bufs[n];
		spans[n].face = pos[i] ? FACE_PICKER_MATCH : NULL;
		spans[n].flags = sel ? TUI_BOLD : 0;
		n++;
		i = j;
	}
	free (pos);
	return n;
}

static void
draw (void *data)
{
	Picker *p = data;
	size_t width = term_width (), height = term_height ();
	size_t nrows = rows_available ();
	size_t ph = panel_height ();
	if (height <= ph)
		return;
	size_t first_row = height - ph;
	bool small = compact ();

	/* Which rows show, keeping the selection in view when it moves. */
	Row *rows = xmalloc ((p->nmatches * 2 + 1) * sizeof (Row));
	size_t total = build_rows (p, rows, p->nmatches * 2 + 1);
	if (p->wheel != 0) {
		long t = (long) p->top + p->wheel;
		p->top = (size_t) MAX (t, 0);
		p->wheel = 0;
		p->follow = false;
	}
	if (p->selected >= 0 && p->follow) {
		size_t sel_row = 0;
		for (size_t i = 0; i < total; i++)
			if (!rows[i].header && (int) rows[i].match == p->selected)
				sel_row = i;
		if (sel_row < p->top)
			p->top = sel_row > 0 && rows[sel_row - 1].header ? sel_row - 1 : sel_row;
		if (sel_row >= p->top + nrows)
			p->top = sel_row - nrows + 1;
	}
	p->top = total > nrows ? MIN (p->top, total - nrows) : 0;

	/* Names line up, in up to 3/5 of the width. */
	size_t text_w = 0;
	for (size_t r = p->top; r < total && r < p->top + nrows; r++)
		if (!rows[r].header) {
			const char *t = p->items[p->matches[rows[r].match]].text;
			text_w = MAX (text_w, display_width (t, strlen (t)));
		}
	text_w = MIN (text_w + 2, width * 3 / 5);

	Query q;
	parse_query (query_text (p), &q);
	TuiNode *list = tui_column (NULL);
	char *bufs[64 * MAX_ROWS];
	size_t nbufs = 0;
	for (size_t r = p->top; r < total && r < p->top + nrows; r++) {
		if (rows[r].header) {
			TuiNode *h = tui_text (p->spec.groups[rows[r].group], FACE_PICKER_GROUP);
			tui_add (list, tui_padding (0, 0, 0, small ? 1 : 3, h));
			continue;
		}
		size_t idx = p->matches[rows[r].match];
		const PickerItem *it = &p->items[idx];
		bool sel = (int) rows[r].match == p->selected;
		TuiKey key = TUI_KEY (PICKER_KIND, rows[r].match + 1);
		TuiSpan spans[64];
		size_t n = item_spans (it, &q, sel, spans, bufs + nbufs, 64);
		nbufs += n;
		TuiNode *mark = tui_text (sel ? (tui_unicode () ? "▌ " : "> ") : "  ", FACE_TUI_KEY);
		TuiNode *row = tui_row (tui_text (small ? "" : " ", NULL), mark,
					tui_sized ((int) text_w, 1, tui_spans (spans, n)),
					tui_expanded (1, tui_text (it->annotation != NULL ? it->annotation : "",
								   FACE_PICKER_ANNOTATION)),
					NULL);
		const char *bg = sel ? FACE_PICKER_SELECTION : tui_hovered (key) ? FACE_TUI_HOVER : NULL;
		TuiRegionSpec spec = { key, item_event, p, 0, (long) rows[r].match, 0, TUI_CURSOR_POINTER, true };
		tui_add (list, tui_clickable (&spec, bg != NULL ? tui_fill (bg, row) : row));
	}
	if (total == 0) {
		bool loading = p->thread_started && !async_finished (p);
		const char *what = loading ? " Loading..." : p->nitems == 0 ? " Nothing to choose from" : " No matches";
		TuiNode *empty = loading ? tui_row (tui_text (small ? "" : "  ", NULL), tui_spinner (FACE_TUI_KEY),
						      tui_text (what, FACE_TUI_MUTED), NULL)
			: tui_padding (0, 0, 0, small ? 1 : 3, tui_text (what + 1, FACE_TUI_MUTED));
		tui_add (list, empty);
	}

	/* Wheel events over empty rows scroll as well. */
	TuiRegionSpec wheel_spec = { TUI_KEY (PICKER_KIND, 0), item_event, p, 0, (long) SIZE_MAX / 2, 0,
				     TUI_CURSOR_DEFAULT, true };
	TuiScroll s = { (double) total, (double) nrows, (double) p->top };
	TuiNode *body = tui_row (tui_expanded (1, tui_clickable (&wheel_spec, list)),
				 tui_scrollbar (&s, TUI_KEY (PICKER_KIND + 1, 0), scroll_to, p, 0, 0), NULL);
	TuiNode *content = tui_column (tui_sized (-1, 1, tui_custom (-1, 1, paint_input, p)),
				       small ? NULL : tui_sized (-1, 1, NULL), NULL);
	tui_add (content, tui_expanded (1, body));

	/* The keys there are, in the bottom border. */
	TuiHint hints[8];
	size_t nh = 0;
	hints[nh++] = (TuiHint) { "RET", p->spec.file_mode ? "open" : "choose", KBD_RET };
	if (p->spec.file_mode || !p->spec.require_match)
		hints[nh++] = (TuiHint) { "TAB", "complete", KBD_TAB };
	if (p->spec.other_window)
		hints[nh++] = (TuiHint) { "C-o", "other window", KBD_CTRL | 'o' };
	if (p->spec.kill != NULL)
		hints[nh++] = (TuiHint) { "C-k", "remove", KBD_CTRL | 'k' };
	if (p->spec.locations)
		hints[nh++] = (TuiHint) { "M-l", "location", KBD_META | 'l' };
	else if (p->spec.back_key)
		hints[nh++] = (TuiHint) { "M-l", "back", KBD_META | 'l' };
	if (!p->spec.require_match && !p->spec.file_mode)
		hints[nh++] = (TuiHint) { "C-j", "use input", KBD_CTRL | 'j' };
	hints[nh++] = (TuiHint) { "C-g", "cancel", KBD_CANCEL };

	/* The prompt, as a title. */
	char *title = xstrdup (p->spec.prompt);
	size_t tl = strlen (title);
	while (tl > 0 && (title[tl - 1] == ' ' || title[tl - 1] == ':'))
		title[--tl] = '\0';
	char *badge = xasprintf ("%s%zu / %zu", p->thread_started && !async_finished (p) ? "… " : "",
				 p->nmatches, p->nitems);
	TuiPanel panel = { small ? TUI_BORDER_NONE : TUI_BORDER_ROUNDED, FACE_TUI_PANEL, NULL, title, badge,
			   hints, nh, 0, 0 };
	tui_render (tui_panel (&panel, content), (TuiRect) { 0, (int) first_row, (int) width, (int) ph });
	free (title);
	free (badge);
	for (size_t i = 0; i < nbufs; i++)
		free (bufs[i]);
	free (rows);

	term_set_cursor_visible (true);
	term_move ((size_t) p->cursor_y, (size_t) MIN ((size_t) MAX (p->cursor_x, 0), width - 1));
}

static void
redraw_all (Picker *p)
{
	(void) p;
	term_redisplay ();
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
	p->follow = true;
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

	minibuf_set_reserved_height (panel_height ());
	term_minibuf_enter ();
	tui_overlay_push (draw, p);
	p->clicked = -1;

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
		if (KBD_IS_MOUSE (key)) {
			/* A click on a candidate selects it; one on the selection
			   or a double click chooses it, as RET would. */
			p->clicked = -1;
			if (!tui_dispatch_mouse (key) || p->clicked < 0 || p->clicked != p->selected)
				continue;
			key = KBD_RET;
		}
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
	tui_overlay_pop (draw, p);
	tui_forget (p);
	if (choice->kind != PICK_CANCEL && choice->input == NULL)
		choice->input = xstrdup (p->input.s);
	if (choice->kind == PICK_ITEM && p->spec.history != NULL)
		history_add_element (p->spec.history, p->items[choice->item].text);
	term_minibuf_leave ();
	minibuf_set_reserved_height (0);
	minibuf_clear ();
}
