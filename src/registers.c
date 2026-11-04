/* Registers facility functions

   Copyright (c) 2001-2022 Free Software Foundation, Inc.

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

#define NUM_REGISTERS 256
static Estr *regs[NUM_REGISTERS];

static long regnum;

static bool
insert_register (void)
{
	buffer_insert_estr (cur_bp, estr_view (regs[regnum]));
	return true;
}

/* Read a register name, either from the arguments or the keyboard. */
static bool
read_register (Args *args, const char *prompt, long *reg)
{
	*reg = 1;
	if (noarg (args)) {
		minibuf_write ("%s", prompt);
		*reg = (long) getkey (GETKEY_DEFAULT);
		return true;
	}
	return int_arg (args, reg);
}

static bool
F_copy_to_register (long uniarg, Args *args)
{
	(void) uniarg;
	long reg;
	if (!read_register (args, "Copy to register: ", &reg))
		return false;

	if (reg == (long) KBD_CANCEL)
		return funcall ("keyboard-quit");

	minibuf_clear ();
	if (reg < 0)
		reg = 0;
	reg %= NUM_REGISTERS; /* Nice numbering relies on NUM_REGISTERS
				 being a power of 2. */

	if (buffer_warn_if_no_mark (cur_bp))
		return false;

	estr_free (regs[reg]);
	regs[reg] = buffer_get_region (cur_bp, region_calculate ());
	return true;
}

static bool
F_insert_register (long uniarg, Args *args)
{
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	long reg;
	if (!read_register (args, "Insert register: ", &reg))
		return false;

	if (reg == (long) KBD_CANCEL)
		return funcall ("keyboard-quit");

	minibuf_clear ();
	if (reg < 0)
		reg = 0;
	reg %= NUM_REGISTERS;

	if (regs[reg] == NULL) {
		minibuf_error ("Register does not contain text");
		return false;
	}

	funcall ("set-mark-command");
	regnum = reg;
	execute_with_uniarg (uniarg, insert_register, NULL);
	funcall ("exchange-point-and-mark");
	cur_bp->mark_active = false;
	return true;
}

static void
write_registers_list (void *data)
{
	(void) data;
	for (size_t i = 0; i < NUM_REGISTERS; ++i)
		if (regs[i] != NULL) {
			const char *text = regs[i]->text;
			const char *s = text;
			while (isspace ((unsigned char) *s))
				s++;
			long w = (long) cur_wp->ewidth - 6;
			int len = (int) MIN (20, MAX (0, w)) + 1;

			if (isprint ((int) i))
				bprintf ("Register %c contains ", (int) i);
			else
				bprintf ("Register \\%o contains ", (unsigned) i);
			if (*s != '\0')
				bprintf ("text starting with\n    %.*s\n", len, s);
			else if (s != text)
				bprintf ("whitespace\n");
			else
				bprintf ("the empty string\n");
		}
}

static bool
F_list_registers (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	write_temp_buffer ("*Registers List*", true, write_registers_list, NULL);
	return true;
}

void
registers_init (void)
{
	lisp_func_new ("copy-to-register", F_copy_to_register, true,
		       "Copy region into register REGISTER.");

	lisp_func_new ("insert-register", F_insert_register, true,
		       "Insert contents of the user specified register.\n"
		       "Puts point before and mark after the inserted text.");

	lisp_func_new ("list-registers", F_list_registers, true,
		       "List defined registers.");
}
