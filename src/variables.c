/* Zile variables handling functions

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

static Map *main_vars;

static void
init_builtin_var (const char *name, const char *defval, bool local, const char *doc)
{
	VarEntry *v = xzalloc (sizeof (VarEntry));
	v->name = xstrdup (name);
	v->defval = xstrdup (defval);
	v->val = xstrdup (defval);
	v->local = local;
	v->doc = xstrdup (doc);
	map_set (main_vars, name, v);
}

void
init_variables (void)
{
	main_vars = map_new ();
#define X(name, defval, local, doc) init_builtin_var (name, defval, local, doc);
#include "tbl_vars.h"
#undef X
}

/* A buffer-local variable shares its entry with the global table, so
   setting it in one buffer (e.g. from the init file) sets its value for
   all buffers, as Zile always has. */
void
set_variable (const char *name, const char *val)
{
	/* Find whether variable is buffer-local when set, and if needed
	   create a buffer-local variable list. */
	Map *var_list = main_vars;
	VarEntry *v = map_get (main_vars, name);
	if (v != NULL && v->local && cur_bp != NULL) {
		if (cur_bp->vars == NULL)
			cur_bp->vars = map_new ();
		var_list = cur_bp->vars;
	}

	/* Create variable if it doesn't already exist. */
	if (v == NULL) {
		v = xzalloc (sizeof (VarEntry));
		v->name = xstrdup (name);
		v->defval = xstrdup (val);
		v->local = false;
		v->doc = xstrdup ("");
	}

	/* Update value. */
	char *old = v->val;
	v->val = xstrdup (val);
	free (old);

	/* Set variable. */
	map_set (var_list, name, v);
}

VarEntry *
get_variable_entry (Buffer *bp, const char *name)
{
	if (bp != NULL && bp->vars != NULL) {
		VarEntry *v = map_get (bp->vars, name);
		if (v != NULL)
			return v;
	}
	return map_get (main_vars, name);
}

const char *
get_variable_doc (const char *name, const char **defval)
{
	VarEntry *v = get_variable_entry (NULL, name);
	if (v == NULL) {
		*defval = NULL;
		return NULL;
	}

	*defval = v->defval;
	return v->doc;
}

const char *
get_variable (const char *name)
{
	VarEntry *v = get_variable_entry (cur_bp, name);
	return v != NULL ? v->val : NULL;
}

bool
get_variable_bool (const char *name)
{
	const char *val = get_variable (name);
	if (val != NULL)
		return strcmp (val, "nil") != 0;
	return false;
}

char *
minibuf_read_variable_name (const char *fmt, ...)
{
	Completion *cp = completion_new (false);
	MapIter it;
	void *val;
	map_iter_init (&it);
	while (map_iter_next (main_vars, &it, NULL, &val))
		sv_add_sorted (&cp->completions, ((VarEntry *) val)->name);

	va_list ap;
	va_start (ap, fmt);
	char *ms = minibuf_vread_completion (fmt, "", cp, NULL,
					     "No variable name given",
					     "Undefined variable name `%s'", ap);
	va_end (ap);
	completion_free (cp);
	return ms;
}

static bool
F_set_variable (long uniarg, Args *args)
{
	(void) uniarg;
	bool ok = true;
	char *name = xstrdup_or_null (args_poll (args));
	if (name == NULL)
		name = minibuf_read_variable_name ("Set variable: ");
	if (name == NULL)
		return false;
	char *val = xstrdup_or_null (args_poll (args));
	if (val == NULL)
		val = minibuf_read ("Set %s to value: ", "", name);
	if (val == NULL)
		ok = funcall ("keyboard-quit");

	if (ok)
		set_variable (name, val);
	free (name);
	free (val);
	return ok;
}

void
variables_init (void)
{
	lisp_func_new ("set-variable", F_set_variable, true,
		       "Set a variable value to the user-specified value.");
}
