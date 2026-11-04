/* Produce dotzile.sample from the table of variables

   Copyright (c) 1997-2020 Free Software Foundation, Inc.
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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef PACKAGE
#define PACKAGE "xzile"
#endif

static void
print_comment (const char *s)
{
	for (; *s != '\0'; s++) {
		putchar (*s);
		if (*s == '\n')
			fputs ("; ", stdout);
	}
}

int
main (void)
{
	printf (";;;; ." PACKAGE " configuration\n"
		"\n"
		";; Rebind keys with:\n"
		";; (global-set-key \"key\" 'func)\n"
		"\n");

#define X(name, defval, local, doc)			\
	fputs ("; ", stdout);				\
	print_comment (doc);				\
	printf ("\n; Default value is %s.\n"		\
		"(setq %s %s)\n\n", defval, name, defval);
#include "tbl_vars.h"
#undef X

	return ferror (stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}
