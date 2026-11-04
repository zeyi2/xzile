/* Marker and region facility functions

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

void
marker_unchain (Marker *m)
{
	if (m == NULL || m->bp == NULL)
		return;

	Marker *prev = NULL;
	for (Marker *it = m->bp->markers; it != NULL; prev = it, it = it->next)
		if (it == m) {
			if (prev != NULL)
				prev->next = it->next;
			else
				m->bp->markers = it->next;
			break;
		}
	m->bp = NULL;
	m->next = NULL;
}

void
marker_free (Marker *m)
{
	if (m != NULL) {
		marker_unchain (m);
		free (m);
	}
}

void
marker_move (Marker *m, Buffer *bp, size_t o)
{
	if (bp != m->bp) {
		/* Unchain with the previous pointed buffer.  */
		marker_unchain (m);

		/* Change the buffer.  */
		m->bp = bp;

		/* Chain with the new buffer.  */
		m->next = bp->markers;
		bp->markers = m;
	}

	/* Change the point.  */
	m->o = o;
}

/* Copy a marker; returns NULL if `m' is NULL or its buffer has gone. */
Marker *
marker_copy (const Marker *m)
{
	if (m == NULL || m->bp == NULL)
		return NULL;
	Marker *marker = xzalloc (sizeof (Marker));
	marker_move (marker, m->bp, m->o);
	return marker;
}

Marker *
marker_point (void)
{
	Marker *m = xzalloc (sizeof (Marker));
	marker_move (m, cur_bp, buffer_pt (cur_bp));
	return m;
}


/*
 * Mark ring
 */

static Marker **mark_ring;
static size_t mark_ring_n, mark_ring_cap;

/* Push the current mark to the mark-ring. */
void
push_mark (void)
{
	if (mark_ring_n == mark_ring_cap) {
		mark_ring_cap = mark_ring_cap ? mark_ring_cap * 2 : 8;
		mark_ring = xrealloc (mark_ring, mark_ring_cap * sizeof (Marker *));
	}

	/* Save the mark.  */
	Marker *m = marker_copy (cur_bp->mark);
	if (m == NULL) { /* Save an invalid mark.  */
		m = xzalloc (sizeof (Marker));
		marker_move (m, cur_bp, 0);
	}
	mark_ring[mark_ring_n++] = m;

	funcall ("set-mark");
}

/* Pop a mark from the mark-ring and make it the current mark. */
void
pop_mark (void)
{
	if (mark_ring_n == 0)
		return;
	Marker *m = mark_ring[--mark_ring_n];

	/* Replace the mark, unless its buffer has been killed. */
	if (m->bp != NULL) {
		Buffer *bp = m->bp;
		marker_free (bp->mark);
		bp->mark = marker_copy (m);
	}

	marker_free (m);
}

/* Set the mark to point. */
void
set_mark (void)
{
	if (cur_bp->mark == NULL)
		cur_bp->mark = marker_point ();
	else
		marker_move (cur_bp->mark, cur_bp, buffer_pt (cur_bp));
}


/*
 * Regions
 */

/*
 * Make a region from two offsets.
 */
Region
region_new (size_t o1, size_t o2)
{
	Region r = { MIN (o1, o2), MAX (o1, o2) };
	return r;
}

size_t
region_size (Region r)
{
	return r.end - r.start;
}

/*
 * Return the region between point and mark.
 */
Region
region_calculate (void)
{
	return region_new (buffer_pt (cur_bp), cur_bp->mark->o);
}

bool
region_delete (Region r)
{
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	Marker *m = marker_point ();
	buffer_goto_offset (cur_bp, r.start);
	buffer_replace_estr (cur_bp, region_size (r), ev_empty ());
	buffer_goto_offset (cur_bp, m->o);
	marker_free (m);
	return true;
}

bool
region_contains (Region r, size_t o)
{
	return o >= r.start && o < r.end;
}
