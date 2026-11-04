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
 * Read a function name from the minibuffer, showing each command's key
 * and documentation, with recently used commands first.
 */
static History *functions_history = NULL;

static int
compare_names (const void *a, const void *b)
{
	return strcmp (*(char *const *) a, *(char *const *) b);
}

char *
minibuf_read_function_name (const char *fmt, ...)
{
	StrVec names = { NULL, 0, 0 }, annotations = { NULL, 0, 0 };
	Map *seen = map_new ();
	for (size_t i = functions_history->elements.n; i > 0; i--) {
		const char *name = functions_history->elements.v[i - 1];
		if (map_get (seen, name) == NULL && lisp_func_find (name) != NULL) {
			map_set (seen, name, (void *) 1);
			sv_push (&names, name);
		}
	}
	size_t nrecent = names.n;
	MapIter it;
	const char *name;
	void *val;
	map_iter_init (&it);
	while (map_iter_next (lisp_funcs, &it, &name, &val))
		if (((LispFunc *) val)->interactive && map_get (seen, name) == NULL)
			sv_push (&names, name);
	qsort (names.v + nrecent, names.n - nrecent, sizeof (char *), compare_names);

	Map *keys = map_new ();
	bindings_by_function (keys);
	for (size_t i = 0; i < names.n; i++) {
		LispFunc *f = lisp_func_find (names.v[i]);
		const char *key = map_get (keys, names.v[i]);
		size_t doc_len = f->doc != NULL ? strcspn (f->doc, "\n") : 0;
		char *ann = xasprintf ("%-12s %.*s", key != NULL ? key : "", (int) doc_len, f->doc != NULL ? f->doc : "");
		sv_push (&annotations, ann);
		free (ann);
	}
	map_free (keys, free);
	map_free (seen, NULL);

	va_list ap;
	va_start (ap, fmt);
	char *prompt = xvasprintf (fmt, ap);
	va_end (ap);
	char *ms = picker_read_name (prompt, &names, &annotations, NULL, true, -1);
	free (prompt);
	if (ms != NULL)
		history_add_element (functions_history, ms);
	sv_clear (&names);
	sv_clear (&annotations);
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
