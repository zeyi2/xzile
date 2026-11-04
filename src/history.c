/* History facility functions

   Copyright (c) 2004-2020 Free Software Foundation, Inc.

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

History *
history_new (void)
{
	History *hp = xzalloc (sizeof (History));
	hp->sel = -1;
	return hp;
}

void
history_add_element (History *hp, const char *s)
{
	StrVec *el = &hp->elements;
	if (el->n == 0 || strcmp (el->v[el->n - 1], s) != 0)
		sv_push (el, s);
}

void
history_prepare (History *hp)
{
	hp->sel = -1;
}

const char *
history_previous_element (History *hp)
{
	/* First time that we use `previous-history-element'. */
	if (hp->sel < 0) { /* Select last element. */
		if (hp->elements.n > 0)
			hp->sel = (long) hp->elements.n - 1;
	} else if (hp->sel > 0) /* Is there another element? */
		hp->sel--;

	return hp->sel < 0 ? NULL : hp->elements.v[hp->sel];
}

const char *
history_next_element (History *hp)
{
	if (hp->elements.n > 0 && hp->sel >= 0) {
		/* Next element. */
		if ((size_t) hp->sel + 1 < hp->elements.n)
			hp->sel++;
		else /* No more elements (back to original status). */
			hp->sel = -1;
	}

	return hp->sel < 0 ? NULL : hp->elements.v[hp->sel];
}
