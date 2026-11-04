/* Macro facility functions

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

static KeyVec *cur_mp = NULL;	/* The macro being defined, or the last one. */
static KeyVec *cmd_mp = NULL;	/* Keystrokes of the command being read. */

static void
macro_free (KeyVec *mp)
{
	if (mp != NULL) {
		kv_free (mp);
		free (mp);
	}
}

void
add_cmd_to_macro (void)
{
	if (cmd_mp == NULL)
		return;
	if (cur_mp != NULL)
		for (size_t i = 0; i < cmd_mp->n; i++)
			kv_push (cur_mp, cmd_mp->v[i]);
	macro_free (cmd_mp);
	cmd_mp = NULL;
}

void
add_key_to_cmd (Keystroke key)
{
	if (cmd_mp == NULL)
		cmd_mp = xzalloc (sizeof (KeyVec));
	kv_push (cmd_mp, key);
}

void
remove_key_from_cmd (void)
{
	if (cmd_mp != NULL && cmd_mp->n > 0)
		cmd_mp->n--;
}

void
cancel_kbd_macro (void)
{
	macro_free (cmd_mp);
	macro_free (cur_mp);
	cmd_mp = cur_mp = NULL;
	thisflag &= ~FLAG_DEFINING_MACRO;
}

static KeyVec macro_keys;

static bool
call_macro (void)
{
	size_t cur = term_buf_len ();
	for (size_t i = macro_keys.n; i > 0; i--)
		pushkey (macro_keys.v[i - 1]);

	while (term_buf_len () > cur)
		get_and_run_command ();

	return true;
}

static void
execute_macro (const KeyVec *keys, long uniarg)
{
	/* Take a copy, as the macro may redefine itself. */
	KeyVec saved = macro_keys;
	kv_copy (&macro_keys, keys);
	execute_with_uniarg (uniarg, call_macro, NULL);
	kv_free (&macro_keys);
	macro_keys = saved;
}

static bool
F_start_kbd_macro (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (thisflag & FLAG_DEFINING_MACRO) {
		minibuf_error ("Already defining a keyboard macro");
		return false;
	}

	if (cur_mp != NULL)
		cancel_kbd_macro ();

	minibuf_write ("Defining keyboard macro...");

	thisflag |= FLAG_DEFINING_MACRO;
	cur_mp = xzalloc (sizeof (KeyVec));

	return true;
}

static bool
F_end_kbd_macro (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!(thisflag & FLAG_DEFINING_MACRO)) {
		minibuf_error ("Not defining a keyboard macro");
		return false;
	}

	thisflag &= ~FLAG_DEFINING_MACRO;
	macro_free (cmd_mp);
	cmd_mp = NULL;
	return true;
}

static bool
F_call_last_kbd_macro (long uniarg, Args *args)
{
	(void) args;
	if (cur_mp == NULL) {
		minibuf_error ("No kbd macro has been defined");
		return false;
	}

	execute_macro (cur_mp, uniarg);
	return true;
}

static bool
F_execute_kbd_macro (long uniarg, Args *args)
{
	KeyVec keys;
	if (!keystrtovec (args_poll (args), &keys))
		return false;

	execute_macro (&keys, uniarg);
	kv_free (&keys);
	return true;
}

void
macro_init (void)
{
	lisp_func_new ("start-kbd-macro", F_start_kbd_macro, true,
		       "Record subsequent keyboard input, defining a keyboard macro.\n"
		       "The commands are recorded even as they are executed.\n"
		       "Use \\[end-kbd-macro] to finish recording and make the macro available.");

	lisp_func_new ("end-kbd-macro", F_end_kbd_macro, true,
		       "Finish defining a keyboard macro.\n"
		       "The definition was started by \\[start-kbd-macro].\n"
		       "The macro is now available for use via \\[call-last-kbd-macro].");

	lisp_func_new ("call-last-kbd-macro", F_call_last_kbd_macro, true,
		       "Call the last keyboard macro that you defined with \\[start-kbd-macro].\n"
		       "A prefix argument serves as a repeat count.");

	lisp_func_new ("execute-kbd-macro", F_execute_kbd_macro, true,
		       "Execute macro as string of editor command characters.");
}
