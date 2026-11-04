/* Self documentation facility functions

   Copyright (c) 1997-2021 Free Software Foundation, Inc.

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
write_function_help (void *data)
{
	LispFunc *func = data;
	bprintf ("%s is %s built-in function in `C source code'.\n\n%s",
		 func->name, func->interactive ? "an interactive" : "a", func->doc);
}

static bool
F_describe_function (long uniarg, Args *args)
{
	(void) uniarg;
	char *name = xstrdup_or_null (args_poll (args));
	if (name == NULL)
		name = minibuf_read_function_name ("Describe function: ");
	if (name == NULL)
		return false;

	LispFunc *func = lisp_func_find (name);
	free (name);
	if (func == NULL)
		return false;
	write_temp_buffer ("*Help*", true, write_function_help, func);
	return true;
}

typedef struct {
	const char *name;
	const char *doc;
} VariableHelp;

static void
write_variable_help (void *data)
{
	VariableHelp *h = data;
	const char *val = get_variable (h->name);
	bprintf ("%s is a variable defined in `C source code'.\n\nIts value is %s\n\n%s",
		 h->name, val != NULL ? val : "nil", h->doc);
}

static bool
F_describe_variable (long uniarg, Args *args)
{
	(void) uniarg;
	char *name = xstrdup_or_null (args_poll (args));
	if (name == NULL)
		name = minibuf_read_variable_name ("Describe variable: ");
	if (name == NULL)
		return false;

	bool ok = true;
	const char *defval;
	const char *doc = get_variable_doc (name, &defval);
	if (doc == NULL)
		ok = false;
	else {
		VariableHelp h = { name, doc };
		write_temp_buffer ("*Help*", true, write_variable_help, &h);
	}
	free (name);
	return ok;
}

typedef struct {
	const char *binding;
	LispFunc *func;
} KeyHelp;

static void
write_key_help (void *data)
{
	KeyHelp *h = data;
	bprintf ("%s runs the command %s, which is %s built-in\nfunction in `C source code'.\n\n%s",
		 h->binding, h->func->name, h->func->interactive ? "an interactive" : "a", h->func->doc);
}

static bool
F_describe_key (long uniarg, Args *args)
{
	(void) uniarg;
	KeyVec keys;
	const char *keystr = args_poll (args);
	if (keystr != NULL) {
		if (!keystrtovec (keystr, &keys))
			return false;
	} else {
		minibuf_write ("Describe key:");
		get_key_sequence (&keys);
	}

	LispFunc *func = get_function_by_keys (&keys);
	char *binding = keyvectodesc (&keys);
	kv_free (&keys);

	bool ok = true;
	if (func == NULL) {
		minibuf_error ("%s is undefined", binding);
		ok = false;
	} else {
		minibuf_write ("%s runs the command `%s'", binding, func->name);
		KeyHelp h = { binding, func };
		write_temp_buffer ("*Help*", true, write_key_help, &h);
	}
	free (binding);
	return ok;
}

void
help_init (void)
{
	lisp_func_new ("describe-function", F_describe_function, true,
		       "Display the full documentation of a function.");

	lisp_func_new ("describe-variable", F_describe_variable, true,
		       "Display the full documentation of a variable.");

	lisp_func_new ("describe-key", F_describe_key, true,
		       "Display documentation of the command invoked by a key sequence.");
}
