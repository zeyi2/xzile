/* Lisp eval

   Copyright (c) 2001-2020 Free Software Foundation, Inc.

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

bool
execute_with_uniarg (long uniarg, MovementFn forward, MovementFn backward)
{
	if (backward != NULL && uniarg < 0) {
		forward = backward;
		uniarg = -uniarg;
	}
	bool ret = true;
	for (long uni = 0; ret && uni < uniarg; ++uni)
		ret = forward ();

	return ret;
}

bool
move_with_uniarg (long uniarg, MovementNFn move)
{
	bool ret = true;
	unsigned long n = uniarg < 0 ? (unsigned long) -uniarg : (unsigned long) uniarg;
	for (unsigned long uni = 0; ret && uni < n; ++uni)
		ret = move (uniarg < 0 ? -1 : 1);
	return ret;
}

bool
execute_function (const char *name, long uniarg, bool is_uniarg)
{
	LispFunc *func = lisp_func_find (name);
	if (func == NULL)
		return false;
	if (is_uniarg)
		return call_command (func, uniarg, NULL);
	Args *args = args_new ();
	bool ok = call_command (func, uniarg, args);
	args_free (args);
	return ok;
}

/*
 * Read a function name from the minibuffer.
 */
static History *functions_history = NULL;
char *
minibuf_read_function_name (const char *fmt, ...)
{
	Completion *cp = completion_new (false);

	MapIter it;
	const char *name;
	void *val;
	map_iter_init (&it);
	while (map_iter_next (lisp_funcs, &it, &name, &val))
		if (((LispFunc *) val)->interactive)
			sv_add_sorted (&cp->completions, name);

	va_list ap;
	va_start (ap, fmt);
	char *ms = minibuf_vread_completion (fmt, "", cp, functions_history,
					     "No function name given",
					     "Undefined function name `%s'", ap);
	va_end (ap);
	completion_free (cp);
	return ms;
}

static bool
F_setq (long uniarg, Args *args)
{
	(void) uniarg;
	while (!args_empty (args)) {
		const char *name = args_poll (args);
		const char *val = args_poll (args);
		if (name != NULL && val != NULL)
			set_variable (name, val);
	}
	return true;
}

static bool
F_execute_extended_command (long uniarg, Args *args)
{
	(void) args;
	char *msg;

	if (lastflag & FLAG_SET_UNIARG) {
		if (lastflag & FLAG_UNIARG_EMPTY)
			msg = xstrdup ("C-u M-x ");
		else
			msg = xasprintf ("%ld M-x ", uniarg);
	} else
		msg = xstrdup ("M-x ");

	char *name = minibuf_read_function_name ("%s", msg);
	free (msg);
	if (name == NULL)
		return false;

	bool ok = execute_function (name, uniarg, lastflag & FLAG_SET_UNIARG);
	free (name);
	return ok;
}

void
eval_init (void)
{
	functions_history = history_new ();

	lisp_func_new ("setq", F_setq, false,
		       "(setq [sym val]...)\n"
		       "\n"
		       "Set each sym to the value of its val.\n"
		       "The symbols sym are variables; they are literal (not evaluated).\n"
		       "The values val are expressions; they are evaluated.");

	lisp_func_new ("execute-extended-command", F_execute_extended_command, true,
		       "Read function name, then read its arguments and call it.");
}
