/* Tests for the estr module.

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

static void
assert_eq (const Estr *es, const char *s)
{
	if (strcmp (es->text, s) != 0) {
		printf ("test failed: \"%s\" != \"%s\"\n", es->text, s);
		exit (EXIT_FAILURE);
	}
}

static void
cat_cstr (Estr *es, const char *s)
{
	estr_cat (es, ev_cstr (s));
}

int
main (void)
{
	Estr *es1 = estr_new (eol_lf);
	cat_cstr (es1, "hello world!");
	assert_eq (es1, "hello world!");
	estr_free (es1);

	es1 = estr_new (eol_lf);
	cat_cstr (es1, "1234567");
	Estr *es2 = estr_new (eol_lf);
	cat_cstr (es2, "foo");
	estr_replace (es1, 1, estr_view (es2));
	assert_eq (es1, "1foo567");
	estr_free (es1);
	estr_free (es2);

	/* Line endings are converted when concatenating. */
	es1 = estr_new (eol_crlf);
	cat_cstr (es1, "a\nb\n");
	assert_eq (es1, "a\r\nb\r\n");
	if (ev_lines (estr_view (es1)) != 2) {
		printf ("test failed: wrong line count\n");
		return EXIT_FAILURE;
	}
	estr_free (es1);

	printf ("estr test successful.\n");

	return EXIT_SUCCESS;
}
