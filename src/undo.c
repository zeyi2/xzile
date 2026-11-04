/* Undo facility functions

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

/* Undo deltas form a singly-linked list, most recent first, headed by
   `last_undop', which owns them; `next_undop' points into it. */

/*
 * Save a reverse delta for doing undo.
 */
static void
undo_save (Buffer *bp, enum UndoType type, size_t o, size_t osize, size_t size)
{
	if (bp->noundo)
		return;

	Undo *u = xzalloc (sizeof (Undo));
	u->type = type;
	u->o = o;

	if (type == UNDO_SAVE_BLOCK) {
		u->size = size;
		u->text = buffer_get_region (bp, region_new (o, o + osize));
		u->unchanged = !bp->modified;
	}

	u->next = bp->last_undop;
	bp->last_undop = u;
}

void
undo_start_sequence (void)
{
	if (cur_bp != NULL)
		undo_save (cur_bp, UNDO_START_SEQUENCE, buffer_pt (cur_bp), 0, 0);
}

void
undo_end_sequence (void)
{
	if (cur_bp != NULL) {
		Undo *l = cur_bp->last_undop;
		if (l != NULL) {
			if (l->type == UNDO_START_SEQUENCE) {
				/* Drop the empty sequence. */
				cur_bp->last_undop = l->next;
				if (cur_bp->next_undop == l)
					cur_bp->next_undop = l->next;
				free (l);
			} else
				undo_save (cur_bp, UNDO_END_SEQUENCE, 0, 0, 0);
		}

		/* Update list pointer */
		if (last_command () != lisp_func_find ("undo"))
			cur_bp->next_undop = cur_bp->last_undop;
	}
}

void
undo_save_block_in (Buffer *bp, size_t o, size_t osize, size_t size)
{
	undo_save (bp, UNDO_SAVE_BLOCK, o, osize, size);
}

/*
 * Revert an action.  Return the next undo entry.
 */
static Undo *
revert_action (Undo *l)
{
	if (l->type == UNDO_END_SEQUENCE)
		for (l = l->next; l != NULL && l->type != UNDO_START_SEQUENCE; l = revert_action (l))
			;
	if (l == NULL)
		return NULL;

	if (l->type != UNDO_END_SEQUENCE)
		buffer_goto_offset (cur_bp, l->o);
	if (l->type == UNDO_SAVE_BLOCK)
		buffer_replace_estr (cur_bp, l->size, estr_view (l->text));
	if (l->unchanged)
		cur_bp->modified = false;

	return l->next;
}

/*
 * Set unchanged flags to false.
 */
void
undo_set_unchanged (Undo *l)
{
	for (; l != NULL; l = l->next)
		l->unchanged = false;
}

void
undo_free_all (Buffer *bp)
{
	for (Undo *u = bp->last_undop, *next; u != NULL; u = next) {
		next = u->next;
		estr_free (u->text);
		free (u);
	}
	bp->last_undop = bp->next_undop = NULL;
}

static bool
F_undo (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (cur_bp->noundo) {
		minibuf_error ("Undo disabled in this buffer");
		return false;
	}

	if (buffer_warn_if_readonly (cur_bp))
		return false;

	if (cur_bp->next_undop == NULL) {
		minibuf_error ("No further undo information");
		cur_bp->next_undop = cur_bp->last_undop;
		return false;
	}

	cur_bp->next_undop = revert_action (cur_bp->next_undop);
	minibuf_write ("Undo!");
	return true;
}

void
undo_init (void)
{
	lisp_func_new ("undo", F_undo, true,
		       "Undo some previous changes.\n"
		       "Repeat this command to undo more changes.");
}
