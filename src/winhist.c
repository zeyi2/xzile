/* Per-window buffer history

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

/* Each window remembers the buffers it has shown, like a browser tab:
   previous-buffer goes back, next-buffer forward again, and showing a
   new buffer forgets the way forward. A buffer appears at most once in
   a window's history, at the place it was left. Changes are noticed by
   comparing windows before and after each command, so a command that
   shows a buffer only for a moment (such as a picker preview) leaves
   no trace. */

#include "xzile.h"

#define MAX_PLACES 100

/* Buffers that are only shown in passing are not part of history. */
static bool
buffer_is_internal (const Buffer *bp)
{
	if (bp->magit != NULL)
		return magit_buffer_is_transient (bp);
	if (bp->filename != NULL || bp->dired != NULL || bp->shell != NULL || bp->compilation != NULL)
		return false;
	return bp->temporary && strcmp (bp->name, "*scratch*") != 0;
}

static void
places_remove (WinPlaces *ps, size_t i)
{
	marker_free (ps->v[i].pt);
	memmove (ps->v + i, ps->v + i + 1, (ps->n - i - 1) * sizeof (WinPlace));
	ps->n--;
}

static bool
places_have (const WinPlaces *ps, const Buffer *bp)
{
	for (size_t i = 0; i < ps->n; i++)
		if (ps->v[i].pt->bp == bp)
			return true;
	return false;
}

/* Drop places in killed or internal buffers, and in `bp'. */
static void
places_prune (WinPlaces *ps, const Buffer *bp)
{
	for (size_t i = ps->n; i-- > 0;) {
		const Buffer *it = ps->v[i].pt->bp;
		if (it == NULL || it == bp || buffer_is_internal (it))
			places_remove (ps, i);
	}
}

static void
places_push (WinPlaces *ps, Buffer *bp, size_t o, size_t topdelta)
{
	places_prune (ps, bp);
	if (ps->n == MAX_PLACES)
		places_remove (ps, 0);
	if (ps->n == ps->cap) {
		ps->cap = ps->cap ? ps->cap * 2 : 8;
		ps->v = xrealloc (ps->v, ps->cap * sizeof (WinPlace));
	}
	Marker *m = xzalloc (sizeof (Marker));
	marker_move (m, bp, MIN (o, buffer_length (bp)));
	ps->v[ps->n++] = (WinPlace) { m, topdelta };
}

static void
places_clear (WinPlaces *ps)
{
	while (ps->n > 0)
		places_remove (ps, ps->n - 1);
}

void
winhist_free (Window *wp)
{
	places_clear (&wp->back);
	places_clear (&wp->fwd);
	free (wp->back.v);
	free (wp->fwd.v);
	wp->back.v = wp->fwd.v = NULL;
	wp->back.cap = wp->fwd.cap = 0;
}

void
winhist_copy (Window *to, const Window *from)
{
	for (size_t i = 0; i < from->back.n; i++)
		if (from->back.v[i].pt->bp != NULL)
			places_push (&to->back, from->back.v[i].pt->bp, from->back.v[i].pt->o, from->back.v[i].topdelta);
	for (size_t i = 0; i < from->fwd.n; i++)
		if (from->fwd.v[i].pt->bp != NULL)
			places_push (&to->fwd, from->fwd.v[i].pt->bp, from->fwd.v[i].pt->o, from->fwd.v[i].topdelta);
}

/* Record that `wp' stopped showing `bp' at `o'. */
static void
record (Window *wp, Buffer *bp, size_t o, size_t topdelta)
{
	if (buffer_is_internal (bp))
		return;
	places_push (&wp->back, bp, o, topdelta);
	places_clear (&wp->fwd);
}


/* Noticing buffer changes around commands. */

typedef struct {
	Window *wp;
	unsigned long id;
	Buffer *bp;
	size_t o, topdelta;
} Snapshot;

static Snapshot *snaps;
static size_t nsnaps, capsnaps;
static bool navigating;

void
winhist_before_command (void)
{
	navigating = false;
	nsnaps = 0;
	for (Window *wp = head_wp; wp != NULL; wp = wp->next) {
		if (nsnaps == capsnaps) {
			capsnaps = capsnaps ? capsnaps * 2 : 8;
			snaps = xrealloc (snaps, capsnaps * sizeof (Snapshot));
		}
		snaps[nsnaps++] = (Snapshot) { wp, wp->id, wp->bp, window_o (wp), wp->topdelta };
	}
}

void
winhist_after_command (void)
{
	if (navigating)
		return;
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		for (size_t i = 0; i < nsnaps; i++) {
			Snapshot *s = &snaps[i];
			if (s->wp == wp && s->id == wp->id && s->bp != wp->bp && buffer_is_live (s->bp))
				record (wp, s->bp, s->o, s->topdelta);
		}
	nsnaps = 0;
}

/* The buffer to show in `wp' instead of `dying', taken from its history,
   or NULL. */
Buffer *
winhist_replacement (Window *wp, const Buffer *dying, size_t *o, size_t *topdelta)
{
	places_prune (&wp->back, dying);
	places_prune (&wp->fwd, dying);
	if (wp->back.n == 0)
		return NULL;
	WinPlace p = wp->back.v[--wp->back.n];
	Buffer *bp = p.pt->bp;
	*o = p.pt->o;
	*topdelta = p.topdelta;
	marker_free (p.pt);
	return bp;
}


/* Moving through history. */

/* A buffer to go back to once the window's history runs out: the most
   recently used one it has not shown. */
static Buffer *
fallback_buffer (const Window *wp, size_t skip)
{
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp != wp->bp && !buffer_is_internal (bp) &&
		    !places_have (&wp->back, bp) && !places_have (&wp->fwd, bp) && skip-- == 0)
			return bp;
	return NULL;
}

static void
show_place (Window *wp, Buffer *bp, size_t o, size_t topdelta)
{
	o = MIN (o, buffer_length (bp));
	buffer_switch_to (bp, false);
	buffer_goto_offset (bp, o);
	window_set_top (wp, topdelta == NOPOS ? wp->eheight / 2 : topdelta);
}

/* Go one buffer back (dir < 0) or forward in the current window. */
static bool
step (int dir)
{
	Window *wp = cur_wp;
	WinPlaces *from = dir < 0 ? &wp->back : &wp->fwd, *to = dir < 0 ? &wp->fwd : &wp->back;
	places_prune (from, cur_bp);

	Buffer *bp;
	size_t o, topdelta;
	if (from->n > 0) {
		WinPlace p = from->v[--from->n];
		bp = p.pt->bp;
		o = p.pt->o;
		topdelta = p.topdelta;
		marker_free (p.pt);
	} else if (dir < 0 && (bp = fallback_buffer (wp, 0)) != NULL) {
		o = buffer_pt (bp);
		topdelta = NOPOS;
	} else
		return false;

	if (!buffer_is_internal (cur_bp))
		places_push (to, cur_bp, buffer_pt (cur_bp), wp->topdelta);
	show_place (wp, bp, o, topdelta);
	return true;
}

/* Show where the current window is in its history: earlier buffers to
   the left, later ones to the right, as the arrow keys go. */
static void
show_strip (void)
{
	Window *wp = cur_wp;
	places_prune (&wp->back, cur_bp);
	places_prune (&wp->fwd, cur_bp);

	size_t width = term_width () > 1 ? term_width () - 1 : 1;
	char *cur = xasprintf ("[%s]", cur_bp->name);
	size_t used = strlen (cur);
	StrVec left = { 0 }, right = { 0 };
	bool more_left = false, more_right = false;
	for (size_t i = 0;; i++) {
		const Buffer *l = NULL, *r = NULL;
		if (!more_left) {
			if (i < wp->back.n)
				l = wp->back.v[wp->back.n - 1 - i].pt->bp;
			else
				l = fallback_buffer (wp, i - wp->back.n);
		}
		if (!more_right && i < wp->fwd.n)
			r = wp->fwd.v[wp->fwd.n - 1 - i].pt->bp;
		if (l == NULL && r == NULL)
			break;
		if (l != NULL) {
			if (used + 2 + strlen (l->name) + 2 <= width) {
				sv_push (&left, l->name);
				used += 2 + strlen (l->name);
			} else
				more_left = true;
		}
		if (r != NULL) {
			if (used + 2 + strlen (r->name) + 2 <= width) {
				sv_push (&right, r->name);
				used += 2 + strlen (r->name);
			} else
				more_right = true;
		}
		if ((more_left || l == NULL) && (more_right || r == NULL) && (more_left || more_right))
			break;
	}

	StrBuf sb;
	sb_init (&sb);
	if (more_left)
		sb_add (&sb, "… ");
	for (size_t i = left.n; i-- > 0;) {
		sb_add (&sb, left.v[i]);
		sb_add (&sb, "  ");
	}
	sb_add (&sb, cur);
	for (size_t i = 0; i < right.n; i++) {
		sb_add (&sb, "  ");
		sb_add (&sb, right.v[i]);
	}
	if (more_right)
		sb_add (&sb, " …");
	minibuf_write ("%s", sb.s);
	sb_free (&sb);
	sv_clear (&left);
	sv_clear (&right);
	free (cur);
}

/* Move through history, then keep moving with bare LEFT and RIGHT
   until another key is pressed. */
static bool
navigate (long n, int dir)
{
	navigating = true;
	if (n < 0) {
		n = -n;
		dir = -dir;
	}
	bool moved = false;
	while (n-- > 0 && step (dir))
		moved = true;
	if (!moved) {
		minibuf_error (dir < 0 ? "No previous buffer in this window" : "No next buffer in this window");
		return false;
	}

	for (;;) {
		show_strip ();
		Keystroke key = getkey (GETKEY_DEFAULT);
		if (key == KBD_LEFT || key == KBD_RIGHT) {
			if (!step (key == KBD_LEFT ? -1 : 1))
				ding ();
			continue;
		}
		if (key != KBD_NOKEY)
			ungetkey (key);
		break;
	}
	buffer_make_recent (cur_bp);
	return true;
}

static bool
F_previous_buffer (long uniarg, Args *args)
{
	(void) args;
	return navigate (uniarg, -1);
}

static bool
F_next_buffer (long uniarg, Args *args)
{
	(void) args;
	return navigate (uniarg, 1);
}

void
winhist_init (void)
{
	lisp_func_new ("previous-buffer", F_previous_buffer, true,
		       "Go back to the buffer this window showed before, where it was left.\n"
		       "Once the window's history runs out, go to buffers it has not shown,\n"
		       "most recently used first. Afterwards LEFT and RIGHT keep moving back\n"
		       "and forward, and the echo area shows the buffers on either side.");

	lisp_func_new ("next-buffer", F_next_buffer, true,
		       "Go forward again to the buffer `previous-buffer' left in this window.\n"
		       "Afterwards LEFT and RIGHT keep moving back and forward, and the echo\n"
		       "area shows the buffers on either side.");
}
