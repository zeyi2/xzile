/* Key bindings and extended commands

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

#include <assert.h>
#include <ctype.h>

/*--------------------------------------------------------------------------
 * Key binding.
 *--------------------------------------------------------------------------*/

typedef struct {
	Keystroke key;
	Binding *binding;
} BindEntry;

struct Binding {
	LispFunc *func;		/* (Leaf node): the function. */
	BindEntry *map;		/* (Other node): key codes to bindings, sorted by key. */
	size_t n, cap;
	bool has_map;
	LispFunc *default_func;	/* (Keymap root): for otherwise unbound keys. */
	/* Note: the root is always a non-leaf node. */
};

static Binding *root_bindings;

static Binding *
binding_new (void)
{
	Binding *b = xzalloc (sizeof (Binding));
	b->has_map = true;
	return b;
}

static void
binding_free (Binding *b)
{
	for (size_t i = 0; i < b->n; i++)
		binding_free (b->map[i].binding);
	free (b->map);
	free (b);
}

static void
binding_clear_map (Binding *b)
{
	for (size_t i = 0; i < b->n; i++)
		binding_free (b->map[i].binding);
	free (b->map);
	b->map = NULL;
	b->n = b->cap = 0;
	b->has_map = false;
}

static size_t
binding_lower_bound (const Binding *b, Keystroke key)
{
	size_t lo = 0, hi = b->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (b->map[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static Binding *
binding_get (const Binding *b, Keystroke key)
{
	size_t i = binding_lower_bound (b, key);
	return i < b->n && b->map[i].key == key ? b->map[i].binding : NULL;
}

static void
binding_put (Binding *b, Keystroke key, Binding *child)
{
	size_t i = binding_lower_bound (b, key);
	if (b->n == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 8;
		b->map = xrealloc (b->map, b->cap * sizeof (BindEntry));
	}
	memmove (b->map + i + 1, b->map + i, (b->n - i) * sizeof (BindEntry));
	b->map[i].key = key;
	b->map[i].binding = child;
	b->n++;
	b->has_map = true;
}

static Binding *
binding_find (Binding *b, const Keystroke *keys, size_t n)
{
	if (n == 0)
		/* No more keys: either we found a binding, or we're part-way
		 * through a prefix. */
		return b;
	Binding *p = binding_get (b, keys[0]);
	return p == NULL ? NULL : binding_find (p, keys + 1, n - 1);
}

static void
binding_bind (Binding *b, const Keystroke *keys, size_t n, LispFunc *func)
{
	/* If we are on the last keystroke, insert the function. */
	if (n == 0) {
		binding_clear_map (b); /* Erase any previous suffixes. */
		b->func = func;
		return;
	}

	Binding *branch = binding_get (b, keys[0]);

	/* If this is part of a new prefix, add a new branch. */
	if (branch == NULL) {
		branch = binding_new ();
		binding_put (b, keys[0], branch);
	} else {
		/* Erase any previous binding the current key might have had. */
		branch->func = NULL;
		branch->has_map = true;
	}

	/* We have more keystrokes: recurse. */
	binding_bind (branch, keys + 1, n - 1, func);
}

enum Lookup { LOOKUP_UNDEFINED, LOOKUP_FUNCTION, LOOKUP_PREFIX };

/* Look up a key sequence in the current buffer's keymap, then in the
   global one; a local prefix shadows a global binding. */
static enum Lookup
lookup_in (Binding *map, const KeyVec *keys, LispFunc **f)
{
	Binding *p = map != NULL ? binding_find (map, keys->v, keys->n) : NULL;
	if (p == NULL)
		return LOOKUP_UNDEFINED;
	if (p->func != NULL) {
		*f = p->func;
		return LOOKUP_FUNCTION;
	}
	return p->has_map && p->n > 0 ? LOOKUP_PREFIX : LOOKUP_UNDEFINED;
}

static enum Lookup
lookup_keys (const KeyVec *keys, LispFunc **f)
{
	*f = NULL;
	Binding *local_map = cur_bp != NULL ? cur_bp->keymap : NULL;
	enum Lookup local = lookup_in (local_map, keys, f);
	if (local != LOOKUP_UNDEFINED)
		return local;
	/* A local default takes every single key it does not bind. */
	if (local_map != NULL && local_map->default_func != NULL && keys->n == 1) {
		*f = local_map->default_func;
		return LOOKUP_FUNCTION;
	}
	return lookup_in (root_bindings, keys, f);
}

Keystroke
binding_completion (const char *a)
{
	StrBuf b;
	sb_init (&b);

	if (lastflag & FLAG_SET_UNIARG) {
		long arg = last_uniarg < 0 ? -last_uniarg : last_uniarg;
		do {
			char *s = xasprintf ("%c %s", (int) (arg % 10 + '0'), b.s);
			sb_free (&b);
			sb_init (&b);
			sb_add (&b, s);
			free (s);
			arg /= 10;
		} while (arg != 0);

		if (last_uniarg < 0) {
			char *s = xstrcat3 ("- ", b.s, "");
			sb_free (&b);
			sb_init (&b);
			sb_add (&b, s);
			free (s);
		}
	}

	bool cu = (lastflag & (FLAG_SET_UNIARG | FLAG_UNIARG_EMPTY)) ==
		(FLAG_SET_UNIARG | FLAG_UNIARG_EMPTY);
	minibuf_write ("%s%s%s-", cu ? "C-u " : "", b.s, a);
	sb_free (&b);
	Keystroke key = getkey (GETKEY_DEFAULT);
	minibuf_clear ();

	return key;
}

/* Get a key sequence from the keyboard; the sequence returned
   has at most the last stroke unbound. */
void
get_key_sequence (KeyVec *keys)
{
	keys->v = NULL;
	keys->n = keys->cap = 0;

	/* Follow files changed on disk while waiting for a command. */
	Keystroke key;
	for (bool all = false;; all = true) {
		if (buffers_check_disk (all) && (thisflag & FLAG_NEED_RESYNC))
			window_resync (cur_wp);
		key = getkey (term_input_eof () ? GETKEY_DEFAULT : 1000);
		if (key != KBD_NOKEY)
			break;
	}
	kv_push (keys, key);
	LispFunc *f;
	while (lookup_keys (keys, &f) == LOOKUP_PREFIX) {
		char *desc = keyvectodesc (keys);
		kv_push (keys, binding_completion (desc));
		free (desc);
	}
}

LispFunc *
get_function_by_keys (const KeyVec *keys)
{
	/* Detect Meta-digit */
	bool local_default = cur_bp != NULL && cur_bp->keymap != NULL && cur_bp->keymap->default_func != NULL;
	if (keys->n == 1 && !local_default) {
		Keystroke key = keys->v[0];
		if ((key & KBD_META) &&
		    (isdigit ((int) (key & 0xff)) || (key & 0xff) == '-'))
			return lisp_func_find ("universal-argument");
	}

	/* See if we've got a valid key sequence */
	LispFunc *f;
	return lookup_keys (keys, &f) == LOOKUP_FUNCTION ? f : NULL;
}

Binding *
keymap_new (void)
{
	return binding_new ();
}

void
keymap_set_default (Binding *map, const char *func_name)
{
	map->default_func = lisp_func_find (func_name);
	assert (map->default_func != NULL);
}

void
keymap_bind (Binding *map, const char *keystr, const char *func_name)
{
	KeyVec keys;
	LispFunc *f = lisp_func_find (func_name);
	assert (f != NULL);
	if (keystrtovec (keystr, &keys)) {
		binding_bind (map, keys.v, keys.n, f);
		kv_free (&keys);
	}
}

static bool
self_insert_command (void)
{
	bool ret = true;
	/* Mask out ~KBD_CTRL to allow control sequences to be themselves. */
	Keystroke key = lastkey () & ~KBD_CTRL;
	cur_bp->mark_active = false;
	if (key <= 0xff) {
		if (isspace ((int) key) && cur_bp->autofill)
			ret = fill_break_line () >= 0;
		buffer_insert_char (cur_bp, (char) key);
	} else {
		ding ();
		ret = false;
	}

	return ret;
}

static bool
bracketed_paste_command (void)
{
	char *text = term_take_pending_paste ();
	if (text == NULL || text[0] == '\0') {
		free (text);
		return true;
	}
	if (shell_char_mode (cur_bp)) {
		shell_send_paste (text);
		free (text);
		return true;
	}

	bool ok = false;
	if (!buffer_warn_if_readonly (cur_bp)) {
		cur_bp->mark_active = false;
		const char *eol = eol_lf;
		if (strstr (text, "\r\n") != NULL)
			eol = eol_crlf;
		else if (strchr (text, '\r') != NULL)
			eol = eol_cr;
		ok = buffer_insert_estr (cur_bp, ev_of (text, strlen (text), eol));
	}
	free (text);
	return ok;
}

static LispFunc *last_command_fn;
static LispFunc *this_command_fn;

LispFunc *
last_command (void)
{
	return last_command_fn;
}

void
set_this_command (LispFunc *cmd)
{
	this_command_fn = cmd;
}

bool
call_command (LispFunc *f, long uniarg, Args *args)
{
	thisflag = lastflag & FLAG_DEFINING_MACRO;
	undo_start_sequence ();

	/* Reset last_uniarg before function call, so recursion (e.g. in
	   macros) works. */
	if (!(thisflag & FLAG_SET_UNIARG))
		last_uniarg = 1;

	/* Execute the command. */
	this_command_fn = f;
	bool ok = f->func (uniarg, args);
	last_command_fn = this_command_fn;

	/* Only add keystrokes if we were already in macro defining mode
	   before the function call, to cope with start-kbd-macro. */
	if (lastflag & thisflag & FLAG_DEFINING_MACRO)
		add_cmd_to_macro ();

	undo_end_sequence ();
	lastflag = thisflag;

	return ok;
}

void
get_and_run_command (void)
{
	KeyVec keys;
	get_key_sequence (&keys);
	if (keys.n == 1 && keys.v[0] == KBD_BRACKETED_PASTE) {
		thisflag = lastflag & FLAG_DEFINING_MACRO;
		undo_start_sequence ();
		if (!(thisflag & FLAG_SET_UNIARG))
			last_uniarg = 1;
		bracketed_paste_command ();
		undo_end_sequence ();
		lastflag = thisflag;
		kv_free (&keys);
		return;
	}

	LispFunc *f = get_function_by_keys (&keys);

	minibuf_clear ();
	if (f != NULL) {
		if (lastflag & FLAG_SET_UNIARG)
			call_command (f, last_uniarg, NULL);
		else {
			Args *args = args_new ();
			call_command (f, last_uniarg, args);
			args_free (args);
		}
	} else {
		char *desc = keyvectodesc (&keys);
		minibuf_error ("%s is undefined", desc);
		free (desc);
	}
	kv_free (&keys);
}

static const struct {
	const char *keys;
	const char *func;
} default_bindings[] = {
	{ "\\M-m", "back-to-indentation" },
	{ "\\LEFT", "backward-char" },
	{ "\\C-b", "backward-char" },
	{ "\\BACKSPACE", "backward-delete-char" },
	{ "\\M-\\BACKSPACE", "backward-kill-word" },
	{ "\\M-{", "backward-paragraph" },
	{ "\\C-\\UP", "backward-paragraph" },
	{ "\\C-\\M-b", "backward-sexp" },
	{ "\\M-b", "backward-word" },
	{ "\\M-<", "beginning-of-buffer" },
	{ "\\HOME", "beginning-of-line" },
	{ "\\C-a", "beginning-of-line" },
	{ "\\C-xe", "call-last-kbd-macro" },
	{ "\\M-c", "capitalize-word" },
	{ "\\M-w", "copy-region-as-kill" },
	{ "\\C-xrs", "copy-to-register" },
	{ "\\C-xrx", "copy-to-register" },
	{ "\\C-x\\C-o", "delete-blank-lines" },
	{ "\\DELETE", "delete-char" },
	{ "\\C-d", "delete-char" },
	{ "\\M-\\\\", "delete-horizontal-space" },
	{ "\\C-x1", "delete-other-windows" },
	{ "\\C-x0", "delete-window" },
	{ "\\C-xd", "dired" },
	{ "\\C-xg", "magit-status" },
	{ "\\C-x\\M-g", "magit-dispatch" },
	{ "\\C-x\\C-j", "dired-jump" },
	{ "\\C-x`", "next-error" },
	{ "\\M-gn", "next-error" },
	{ "\\M-g\\M-n", "next-error" },
	{ "\\M-gp", "previous-error" },
	{ "\\M-g\\M-p", "previous-error" },
	{ "\\C-hb", "describe-bindings" },
	{ "\\F1b", "describe-bindings" },
	{ "\\C-hf", "describe-function" },
	{ "\\F1f", "describe-function" },
	{ "\\C-hk", "describe-key" },
	{ "\\F1k", "describe-key" },
	{ "\\C-hv", "describe-variable" },
	{ "\\F1v", "describe-variable" },
	{ "\\C-x\\C-l", "downcase-region" },
	{ "\\M-l", "downcase-word" },
	{ "\\C-x,", "duplicate-line" },
	{ "\\C-x)", "end-kbd-macro" },
	{ "\\M->", "end-of-buffer" },
	{ "\\END", "end-of-line" },
	{ "\\C-e", "end-of-line" },
	{ "\\C-x^", "enlarge-window" },
	{ "\\C-x}", "enlarge-window-horizontally" },
	{ "\\C-x\\C-x", "exchange-point-and-mark" },
	{ "\\M-x", "execute-extended-command" },
	{ "\\M-q", "fill-paragraph" },
	{ "\\C-x\\C-v", "find-alternate-file" },
	{ "\\C-x\\C-f", "find-file" },
	{ "\\C-x\\C-r", "find-file-read-only" },
	{ "\\RIGHT", "forward-char" },
	{ "\\C-f", "forward-char" },
	{ "\\M-}", "forward-paragraph" },
	{ "\\C-\\DOWN", "forward-paragraph" },
	{ "\\C-\\M-f", "forward-sexp" },
	{ "\\M-f", "forward-word" },
	{ "\\M-gg", "goto-line" },
	{ "\\M-g\\M-g", "goto-line" },
	{ "\\TAB", "indent-for-tab-command" },
	{ "\\C-xi", "insert-file" },
	{ "\\C-xri", "insert-register" },
	{ "\\C-xrg", "insert-register" },
	{ "\\C-r", "isearch-backward" },
	{ "\\C-\\M-r", "isearch-backward-regexp" },
	{ "\\C-s", "isearch-forward" },
	{ "\\C-\\M-s", "isearch-forward-regexp" },
	{ "\\M-\\SPC", "just-one-space" },
	{ "\\C-g", "keyboard-quit" },
	{ "\\C-xk", "kill-buffer" },
	{ "\\C-k", "kill-line" },
	{ "\\C-w", "kill-region" },
	{ "\\C-\\M-k", "kill-sexp" },
	{ "\\M-d", "kill-word" },
	{ "\\C-x\\C-b", "list-buffers" },
	{ "\\M-h", "mark-paragraph" },
	{ "\\C-\\M-@", "mark-sexp" },
	{ "\\C-xh", "mark-whole-buffer" },
	{ "\\M-@", "mark-word" },
	{ "\\M-\\DOWN", "move-text-down" },
	{ "\\M-\\UP", "move-text-up" },
	{ "\\RET", "newline" },
	{ "\\C-j", "newline-and-indent" },
	{ "\\C-x\\RIGHT", "next-buffer" },
	{ "\\DOWN", "next-line" },
	{ "\\C-n", "next-line" },
	{ "\\C-o", "open-line" },
	{ "\\C-xo", "other-window" },
	{ "\\C-x\\LEFT", "previous-buffer" },
	{ "\\UP", "previous-line" },
	{ "\\C-p", "previous-line" },
	{ "\\M-%", "query-replace" },
	{ "\\C-q", "quoted-insert" },
	{ "\\C-l", "recenter" },
	{ "\\C-x\\C-s", "save-buffer" },
	{ "\\C-x\\C-c", "save-buffers-kill-emacs" },
	{ "\\C-xs", "save-some-buffers" },
	{ "\\PRIOR", "scroll-down" },
	{ "\\M-v", "scroll-down" },
	{ "\\NEXT", "scroll-up" },
	{ "\\C-v", "scroll-up" },
	{ "\\C-xf", "set-fill-column" },
	{ "\\C-@", "set-mark-command" },
	{ "\\M-!", "shell-command" },
	{ "\\M-|", "shell-command-on-region" },
	{ "\\C-x{", "shrink-window-horizontally" },
	{ "\\C-x2", "split-window" },
	{ "\\C-x3", "split-window-right" },
	{ "\\C-x(", "start-kbd-macro" },
	{ "\\C-x\\C-z", "suspend-emacs" },
	{ "\\C-z", "suspend-emacs" },
	{ "\\C-xb", "switch-to-buffer" },
	{ "\\M-i", "tab-to-tab-stop" },
	{ "\\C-x\\C-q", "toggle-read-only" },
	{ "\\C-t", "transpose-chars" },
	{ "\\C-x\\C-t", "transpose-lines" },
	{ "\\C-\\M-t", "transpose-sexps" },
	{ "\\M-t", "transpose-words" },
	{ "\\C-xu", "undo" },
	{ "\\C-_", "undo" },
	{ "\\C-u", "universal-argument" },
	{ "\\C-x\\C-u", "upcase-region" },
	{ "\\M-u", "upcase-word" },
	{ "\\C-hw", "where-is" },
	{ "\\F1w", "where-is" },
	{ "\\C-x\\C-w", "write-file" },
	{ "\\C-y", "yank" },
};

void
init_default_bindings (void)
{
	root_bindings = binding_new ();

	/* Bind all printing keys to self_insert_command */
	LispFunc *self_insert = lisp_func_find ("self-insert-command");
	for (Keystroke i = 0; i <= 0xff; i++)
		if ((i >= 0x20 && i < 0x7f) || i >= 0x80)
			binding_bind (root_bindings, &i, 1, self_insert);

	for (size_t i = 0; i < sizeof default_bindings / sizeof default_bindings[0]; i++) {
		KeyVec keys;
		LispFunc *f = lisp_func_find (default_bindings[i].func);
		assert (f != NULL);
		if (keystrtovec (default_bindings[i].keys, &keys)) {
			binding_bind (root_bindings, keys.v, keys.n, f);
			kv_free (&keys);
		}
	}
}

typedef void (*BindingsProcessor) (const char *key, Binding *p, void *data);

static void
walk_bindings_tree (Binding *tree, StrVec *keys, BindingsProcessor process, void *data)
{
	for (size_t i = 0; i < tree->n; i++) {
		Binding *p = tree->map[i].binding;
		char *desc = chordtodesc (tree->map[i].key);
		sv_push (keys, desc);
		free (desc);
		if (p->func != NULL) {
			StrBuf sb;
			sb_init (&sb);
			for (size_t k = 0; k < keys->n; k++) {
				if (k > 0)
					sb_addc (&sb, ' ');
				sb_add (&sb, keys->v[k]);
			}
			process (sb.s, p, data);
			sb_free (&sb);
		} else
			walk_bindings_tree (p, keys, process, data);
		free (keys->v[--keys->n]);
	}
}

static void
walk_bindings (BindingsProcessor process, void *data)
{
	StrVec keys = { NULL, 0, 0 };
	walk_bindings_tree (root_bindings, &keys, process, data);
	sv_clear (&keys);
}

static void
first_binding_process (const char *key, Binding *p, void *data)
{
	Map *m = data;
	char *old = map_get (m, p->func->name);
	if (old == NULL || strlen (key) < strlen (old)) {
		free (old);
		map_set (m, p->func->name, xstrdup (key));
	}
}

/* Map each bound function's name to its shortest key (owned strings). */
void
bindings_by_function (Map *out)
{
	walk_bindings (first_binding_process, out);
}

static bool
F_global_set_key (long uniarg, Args *args)
{
	(void) uniarg;
	KeyVec keys;
	char *keystr = xstrdup_or_null (args_poll (args));
	if (keystr != NULL) {
		if (!keystrtovec (keystr, &keys)) {
			minibuf_error ("Key sequence %s is invalid", keystr);
			free (keystr);
			return false;
		}
	} else {
		minibuf_write ("Set key globally: ");
		get_key_sequence (&keys);
		keystr = keyvectodesc (&keys);
	}

	bool ok = false;
	char *name = xstrdup_or_null (args_poll (args));
	if (name == NULL)
		name = minibuf_read_function_name ("Set key %s to command: ", keystr);
	if (name != NULL) {
		LispFunc *func = lisp_func_find (name);
		if (func == NULL) /* Possible if called non-interactively */
			minibuf_error ("No such function `%s'", name);
		else {
			binding_bind (root_bindings, keys.v, keys.n, func);
			ok = true;
		}
	}
	free (name);
	free (keystr);
	kv_free (&keys);
	return ok;
}

static bool
F_self_insert_command (long uniarg, Args *args)
{
	(void) args;
	return execute_with_uniarg (uniarg, self_insert_command, NULL);
}

typedef struct {
	LispFunc *f;
	StrBuf sb;
} WhereIsData;

static void
where_is_process (const char *key, Binding *p, void *data)
{
	WhereIsData *d = data;
	if (p->func == d->f) {
		if (d->sb.len > 0)
			sb_add (&d->sb, ", ");
		sb_add (&d->sb, key);
	}
}

static bool
F_where_is (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	char *name = minibuf_read_function_name ("Where is command: ");
	bool ok = false;

	if (name != NULL) {
		LispFunc *f = lisp_func_find (name);
		if (f != NULL) {
			WhereIsData d;
			d.f = f;
			sb_init (&d.sb);
			walk_bindings (where_is_process, &d);

			if (d.sb.len == 0)
				minibuf_write ("%s is not on any key", name);
			else
				minibuf_write ("%s is on %s", name, d.sb.s);
			sb_free (&d.sb);
			ok = true;
		}
	}
	free (name);
	return ok;
}

static void
describe_bindings_process (const char *key, Binding *p, void *data)
{
	(void) data;
	bprintf ("%-15s %s\n", key, p->func->name);
}

static void
write_bindings_list (void *data)
{
	(void) data;
	bprintf ("Key translations:\n");
	bprintf ("%-15s %s\n", "key", "binding");
	bprintf ("%-15s %s\n", "---", "-------");
	walk_bindings (describe_bindings_process, NULL);
}

static bool
F_describe_bindings (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	write_temp_buffer ("*Help*", true, write_bindings_list, NULL);
	return true;
}

void
bind_init (void)
{
	lisp_func_new ("global-set-key", F_global_set_key, true,
		       "Bind a command to a key sequence.\n"
		       "Read key sequence and function name, and bind the function to the key\n"
		       "sequence.");

	lisp_func_new ("self-insert-command", F_self_insert_command, true,
		       "Insert the character you type.\n"
		       "Whichever character you type to run this command is inserted.");

	lisp_func_new ("where-is", F_where_is, true,
		       "Print message listing key sequences that invoke the command DEFINITION.\n"
		       "Argument is a command name.");

	lisp_func_new ("describe-bindings", F_describe_bindings, true,
		       "Show a list of all defined keys, and their definitions.");
}
