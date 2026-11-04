/* Pickers for buffers, files, commands and other names

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

/* `switch-to-buffer' (C-x b) offers the open buffers, recently visited
   files and the files of the current Git project, previewing each in
   the window as it is selected. `kill-buffer' (C-x k) and
   `insert-buffer' offer buffers. The recent files list is kept in
   ~/.xzile-recentf unless the init file is not loaded. */

#include "xzile.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_RECENT 200
/* Larger files are not loaded just to preview them. */
#define MAX_PREVIEW_SIZE (1024 * 1024)

enum { G_BUFFERS, G_RECENT, G_PROJECT };

static const char *const switch_groups[] = { "Buffers", "Recent files", "Project files", NULL };


/*--------------------------------------------------------------------------
 * Recent files.
 *--------------------------------------------------------------------------*/

static StrVec recent;		/* Most recent last. */
static bool recent_enabled;

void
recentf_add (const char *path)
{
	for (size_t i = 0; i < recent.n; i++)
		if (strcmp (recent.v[i], path) == 0) {
			free (recent.v[i]);
			memmove (recent.v + i, recent.v + i + 1, (recent.n - i - 1) * sizeof (char *));
			recent.n--;
			break;
		}
	if (recent.n == MAX_RECENT) {
		free (recent.v[0]);
		memmove (recent.v, recent.v + 1, (recent.n - 1) * sizeof (char *));
		recent.n--;
	}
	sv_push (&recent, path);
}

static char *
recentf_file (void)
{
	char *home = get_home_dir ();
	char *file = path_join (home, "." PACKAGE "-recentf");
	free (home);
	return file;
}

void
recentf_load (void)
{
	recent_enabled = true;
	char *file = recentf_file ();
	char *s;
	size_t len;
	if (read_file (file, &s, &len)) {
		for (char *p = s, *line; (line = strsep (&p, "\n")) != NULL;)
			if (line[0] == '/')
				recentf_add (line);
		free (s);
	}
	free (file);
}

void
recentf_save (void)
{
	if (!recent_enabled)
		return;
	char *file = recentf_file ();
	char *tmp = xstrcat3 (file, ".tmp", "");
	int fd = open (tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0) {
		bool ok = true;
		for (size_t i = 0; ok && i < recent.n; i++) {
			char *line = xstrcat3 (recent.v[i], "\n", "");
			ok = write (fd, line, strlen (line)) == (ssize_t) strlen (line);
			free (line);
		}
		if (close (fd) == 0 && ok)
			rename (tmp, file);
		else
			unlink (tmp);
	}
	free (tmp);
	free (file);
}


/*--------------------------------------------------------------------------
 * Generic names and files.
 *--------------------------------------------------------------------------*/

/* Read one of `names'; returns NULL if cancelled (after keyboard-quit).
   Without `require_match', the input may be anything. */
char *
picker_read_name (const char *prompt, StrVec *names, StrVec *annotations,
		  History *hp, bool require_match, int default_item)
{
	PickerSpec spec;
	memset (&spec, 0, sizeof spec);
	spec.prompt = prompt;
	spec.require_match = require_match;
	spec.preselect = true;
	spec.default_item = default_item;
	spec.history = hp;
	Picker *p = picker_new (&spec);
	for (size_t i = 0; i < names->n; i++)
		picker_add (p, names->v[i], annotations != NULL && i < annotations->n ? annotations->v[i] : NULL, 0, NULL);

	PickerChoice choice;
	picker_run (p, &choice);
	char *result = NULL;
	if (choice.kind == PICK_ITEM)
		result = xstrdup (picker_item (p, choice.item)->text);
	else if (choice.kind == PICK_INPUT)
		result = xstrdup (choice.input);
	free (choice.input);
	picker_free (p);
	if (choice.kind == PICK_CANCEL)
		funcall ("keyboard-quit");
	return result;
}

/* Read a file name, starting from `initial' (a directory ending in /,
   or a path). With `visit', matches are preselected, for opening
   existing files; otherwise only exact matches are, as the name may be
   new. Returns the expanded path, or NULL if cancelled. */
char *
picker_read_file (const char *prompt, const char *initial, History *hp, bool visit)
{
	PickerSpec spec;
	memset (&spec, 0, sizeof spec);
	spec.prompt = prompt;
	spec.initial = initial;
	spec.file_mode = true;
	spec.preselect = visit;
	spec.default_item = -1;
	spec.history = hp;
	static const char *const file_groups[] = { "Directories", "Files", NULL };
	(void) file_groups;
	Picker *p = picker_new (&spec);

	PickerChoice choice;
	picker_run (p, &choice);
	char *result = choice.kind != PICK_CANCEL ? expand_path (choice.input) : NULL;
	free (choice.input);
	picker_free (p);
	if (choice.kind == PICK_CANCEL)
		funcall ("keyboard-quit");
	return result;
}


/*--------------------------------------------------------------------------
 * Buffers.
 *--------------------------------------------------------------------------*/

static bool
hidden_buffer (const Buffer *bp)
{
	return bp->name == NULL || bp->name[0] == ' ' || strcmp (bp->name, "*Completions*") == 0;
}

static char *
buffer_annotation (Buffer *bp)
{
	StrBuf sb;
	sb_init (&sb);
	sb_addf (&sb, "%-2s  %-16s  ", bp->modified ? "**" : bp->readonly ? "%%" : "",
		 buffer_mode_name (bp));
	if (bp->filename != NULL) {
		char *p = compact_path (bp->filename);
		sb_add (&sb, p);
		free (p);
	} else if (bp->dired != NULL || bp->shell != NULL || bp->magit != NULL || bp->compilation != NULL) {
		char *p = compact_path (bp->dir);
		sb_add (&sb, p);
		free (p);
	}
	return sb_take (&sb);
}

/* Add the visible buffers, with `first' first and `last' last. */
static void
add_buffers (Picker *p, Buffer *first, Buffer *last)
{
	Buffer *order[3] = { first, NULL, last };
	for (int pass = 0; pass < 3; pass++)
		for (Buffer *bp = head_bp; bp != NULL; bp = bp->next) {
			bool special = bp == first || bp == last;
			if ((pass == 0 && bp != order[0]) || (pass == 1 && special) || (pass == 2 && bp != order[2]))
				continue;
			if (hidden_buffer (bp))
				continue;
			char *ann = buffer_annotation (bp);
			picker_add (p, bp->name, ann, G_BUFFERS, bp);
			free (ann);
		}
}

typedef struct {
	Buffer *orig;
	size_t orig_topdelta, orig_lastpointn;
	char *project_dir;	/* Where to look for a Git project. */
	char *project_root;	/* Found by the asynchronous source. */
	Map *known;		/* Absolute paths of buffers and recent files. */
	Buffer **previews;	/* Buffers created to preview files. */
	size_t npreviews;
} SwitchState;

static char *
item_path (SwitchState *st, const PickerItem *it)
{
	if (it->group == G_RECENT)
		return expand_path (it->text);
	if (it->group == G_PROJECT && st->project_root != NULL)
		return path_join (st->project_root, it->text);
	return NULL;
}

/* A buffer for previewing `path', loading it if need be. */
static Buffer *
preview_buffer (SwitchState *st, const char *path)
{
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->filename != NULL && strcmp (bp->filename, path) == 0)
			return bp;
	struct stat sb;
	if (stat (path, &sb) != 0 || !S_ISREG (sb.st_mode) || sb.st_size > MAX_PREVIEW_SIZE)
		return NULL;
	Estr *es = estr_from_file (path);
	if (es == NULL)
		return NULL;
	Buffer *bp = buffer_new (es);
	buffer_set_names (bp, path);
	free (bp->dir);
	bp->dir = path_dirname (path);
	bp->modified = false;
	/* Keep it at the end of the buffer list while previewing. */
	buffer_set_temporary (bp);
	bp->temporary = false;
	st->previews = xrealloc (st->previews, (st->npreviews + 1) * sizeof (Buffer *));
	st->previews[st->npreviews++] = bp;
	return bp;
}

static void
show_in_window (Buffer *bp)
{
	if (bp == NULL || cur_bp == bp)
		return;
	buffer_switch_to (bp, false);
	recenter (cur_wp);
}

static void
switch_preview (PickerItem *it, void *data)
{
	SwitchState *st = data;
	if (it->group == G_BUFFERS) {
		show_in_window (it->data);
		return;
	}
	char *path = item_path (st, it);
	if (path != NULL)
		show_in_window (preview_buffer (st, path));
	free (path);
}

static void
restore_window (SwitchState *st)
{
	if (buffer_is_live (st->orig) && cur_bp != st->orig) {
		buffer_switch_to (st->orig, false);
		cur_wp->topdelta = st->orig_topdelta;
		cur_wp->lastpointn = st->orig_lastpointn;
	}
}

static bool
switch_kill (PickerItem *it, void *data)
{
	SwitchState *st = data;
	if (it->group != G_BUFFERS) {
		ding ();
		return false;
	}
	Buffer *bp = it->data;
	if (!buffer_is_live (bp) || !buffer_check_modified (bp))
		return false;
	if ((shell_process_alive (bp) || compilation_process_alive (bp)) &&
	    minibuf_read_yesno ("Buffer %s has a running process; kill it? (yes or no) ", bp->name) != 1)
		return false;
	for (size_t i = 0; i < st->npreviews; i++)
		if (st->previews[i] == bp)
			st->previews[i] = NULL;
	bool was_orig = bp == st->orig;
	buffer_kill (bp);
	if (was_orig)
		st->orig = cur_bp;
	return true;
}

/* List the files of the Git project containing the directory. */
static void
project_files (Picker *p, void *data)
{
	SwitchState *st = data;
	const char *argv1[] = { "git", "-C", st->project_dir, "rev-parse", "--show-toplevel", NULL };
	const char *const env[] = { "GIT_OPTIONAL_LOCKS=0", NULL };
	StrBuf out;
	sb_init (&out);
	if (process_run (argv1, env, NULL, 0, &out, NULL) != 0 || out.len == 0) {
		sb_free (&out);
		return;
	}
	while (out.len > 0 && out.s[out.len - 1] == '\n')
		out.s[--out.len] = '\0';
	char *root = sb_take (&out);
	st->project_root = root;

	const char *argv2[] = { "git", "-C", root, "-c", "core.quotepath=false", "ls-files", "-z",
				"--cached", "--others", "--exclude-standard", NULL };
	sb_init (&out);
	if (process_run (argv2, env, NULL, 0, &out, NULL) == 0)
		for (size_t i = 0; i < out.len;) {
			size_t n = strnlen (out.s + i, out.len - i);
			if (n > 0)
				picker_add_async (p, out.s + i, NULL, G_PROJECT);
			i += n + 1;
		}
	sb_free (&out);
}

/* Skip project files that are open or recent. */
static bool
accept_project_file (const char *text, void *data)
{
	SwitchState *st = data;
	if (st->project_root == NULL)
		return true;
	char *path = path_join (st->project_root, text);
	bool known = map_get (st->known, path) != NULL;
	free (path);
	return !known;
}

static bool
switch_to_name (const char *name, bool other_window)
{
	if (other_window)
		window_set_current (popup_window ());
	Buffer *bp = buffer_find (name);
	if (bp == NULL) {
		bp = buffer_new (NULL);
		bp->name = xstrdup (name);
		bp->needname = true;
		bp->nosave = true;
	}
	buffer_switch_to (bp, true);
	return true;
}

static bool
pick_buffer_to_switch (void)
{
	SwitchState st;
	memset (&st, 0, sizeof st);
	st.orig = cur_bp;
	st.orig_topdelta = cur_wp->topdelta;
	st.orig_lastpointn = cur_wp->lastpointn;
	st.project_dir = xstrdup (cur_bp->dir);
	st.known = map_new ();

	PickerSpec spec;
	memset (&spec, 0, sizeof spec);
	spec.prompt = "Switch to: ";
	spec.groups = switch_groups;
	spec.preselect = true;
	spec.fuzzy_groups = 1u << G_BUFFERS;
	spec.other_window = true;
	spec.default_item = 0;
	spec.preview = switch_preview;
	spec.kill = switch_kill;
	spec.async = project_files;
	spec.accept_async = accept_project_file;
	spec.data = &st;
	Picker *p = picker_new (&spec);

	/* The previous buffer comes first, the current one last. */
	Buffer *other = NULL;
	for (Buffer *bp = head_bp; bp != NULL && other == NULL; bp = bp->next)
		if (bp != cur_bp && !hidden_buffer (bp))
			other = bp;
	add_buffers (p, other, cur_bp);
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->filename != NULL)
			map_set (st.known, bp->filename, (void *) 1);
	for (size_t i = recent.n; i > 0; i--) {
		const char *path = recent.v[i - 1];
		if (map_get (st.known, path) != NULL)
			continue;
		map_set (st.known, path, (void *) 1);
		char *shown = compact_path (path);
		picker_add (p, shown, NULL, G_RECENT, NULL);
		free (shown);
	}

	PickerChoice choice;
	picker_run (p, &choice);

	bool ok = false;
	Buffer *chosen = NULL;
	char *path = NULL;
	if (choice.kind == PICK_ITEM) {
		PickerItem *it = picker_item (p, choice.item);
		if (it->group == G_BUFFERS)
			chosen = it->data;
		else
			path = item_path (&st, it);
	}

	restore_window (&st);
	if (choice.kind == PICK_ITEM && chosen != NULL && buffer_is_live (chosen)) {
		if (choice.other_window)
			window_set_current (popup_window ());
		buffer_switch_to (chosen, true);
		ok = true;
	} else if (choice.kind == PICK_ITEM && path != NULL) {
		if (choice.other_window)
			window_set_current (popup_window ());
		ok = find_file (path);
	} else if (choice.kind == PICK_INPUT) {
		if (choice.input[0] != '\0')
			ok = switch_to_name (choice.input, false);
		else if (other != NULL) {
			buffer_switch_to (other, true);
			ok = true;
		}
	}

	/* Drop preview buffers that were not chosen. */
	for (size_t i = 0; i < st.npreviews; i++) {
		Buffer *bp = st.previews[i];
		if (bp != NULL && buffer_is_live (bp) && bp != cur_bp && window_showing (bp) == NULL)
			buffer_kill (bp);
		else if (bp != NULL && buffer_is_live (bp))
			recentf_add (bp->filename);
	}
	free (st.previews);

	if (choice.kind == PICK_CANCEL)
		funcall ("keyboard-quit");
	free (choice.input);
	free (path);
	picker_free (p);
	map_free (st.known, NULL);
	free (st.project_dir);
	free (st.project_root);
	return ok;
}

static bool
kill_one (PickerItem *it, void *data)
{
	(void) data;
	Buffer *bp = it->data;
	if (!buffer_is_live (bp) || !buffer_check_modified (bp))
		return false;
	buffer_kill (bp);
	return true;
}

/* Read a buffer, the current one first; returns NULL if cancelled. */
static Buffer *
pick_buffer (const char *prompt, Buffer *def, bool *cancelled, char **typed)
{
	PickerSpec spec;
	memset (&spec, 0, sizeof spec);
	spec.prompt = prompt;
	spec.preselect = true;
	spec.fuzzy_groups = 1u << G_BUFFERS;
	spec.default_item = 0;
	spec.kill = kill_one;
	Picker *p = picker_new (&spec);
	add_buffers (p, def, NULL);

	PickerChoice choice;
	picker_run (p, &choice);
	Buffer *bp = NULL;
	*cancelled = choice.kind == PICK_CANCEL;
	*typed = NULL;
	if (choice.kind == PICK_ITEM)
		bp = picker_item (p, choice.item)->data;
	else if (choice.kind == PICK_INPUT) {
		if (choice.input[0] == '\0')
			bp = def;
		else if ((bp = buffer_find (choice.input)) == NULL)
			*typed = xstrdup (choice.input);
	}
	free (choice.input);
	picker_free (p);
	return bp != NULL && buffer_is_live (bp) ? bp : NULL;
}


/*--------------------------------------------------------------------------
 * Commands.
 *--------------------------------------------------------------------------*/

static bool
F_switch_to_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	const char *name = args_poll (args);
	if (name != NULL) {
		if (name[0] != '\0')
			return switch_to_name (name, false);
		Buffer *bp = cur_bp->next != NULL ? cur_bp->next : head_bp;
		buffer_switch_to (bp, true);
		return true;
	}
	return pick_buffer_to_switch ();
}

static bool
kill_buffer_checked (Buffer *bp)
{
	if (!buffer_check_modified (bp))
		return false;
	if ((shell_process_alive (bp) || compilation_process_alive (bp)) &&
	    minibuf_read_yesno ("Buffer %s has a running process; kill it? (yes or no) ", bp->name) != 1)
		return false;
	buffer_kill (bp);
	return true;
}

static bool
F_kill_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	const char *name = args_poll (args);
	Buffer *bp;
	if (name != NULL) {
		bp = name[0] != '\0' ? buffer_find (name) : cur_bp;
		if (bp == NULL) {
			minibuf_error ("Buffer `%s' not found", name);
			return false;
		}
	} else {
		bool cancelled;
		char *typed;
		char *prompt = xasprintf ("Kill buffer (default %s): ", cur_bp->name);
		bp = pick_buffer (prompt, cur_bp, &cancelled, &typed);
		free (prompt);
		if (cancelled)
			return funcall ("keyboard-quit");
		if (bp == NULL) {
			minibuf_error ("Buffer `%s' not found", typed != NULL ? typed : "");
			free (typed);
			return false;
		}
	}
	return kill_buffer_checked (bp);
}

static bool
F_insert_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	if (buffer_warn_if_readonly (cur_bp))
		return false;
	Buffer *def = cur_bp->next != NULL ? cur_bp->next : head_bp;
	const char *name = args_poll (args);
	Buffer *bp;
	if (name != NULL)
		bp = name[0] != '\0' ? buffer_find (name) : def;
	else {
		bool cancelled;
		char *typed;
		char *prompt = xasprintf ("Insert buffer (default %s): ", def->name);
		bp = pick_buffer (prompt, def, &cancelled, &typed);
		free (prompt);
		free (typed);
		if (cancelled)
			return funcall ("keyboard-quit");
	}
	if (bp == NULL) {
		minibuf_error ("Buffer `%s' not found", name != NULL ? name : "");
		return false;
	}
	buffer_insert_buffer (cur_bp, bp);
	funcall ("set-mark-command");
	return true;
}

void
selectors_init (void)
{
	lisp_func_new ("switch-to-buffer", F_switch_to_buffer, true,
		       "Select a buffer, a recently visited file or a file of the current\n"
		       "Git project, in the current window.\n"
		       "\n"
		       "Type to narrow the list (words may match anywhere, in any order);\n"
		       "C-n and C-p move, and the selection is previewed in the window. RET\n"
		       "chooses it (or, with nothing selected, switches to or creates a buffer\n"
		       "named by the input, as C-j always does), C-o chooses it for the other\n"
		       "window, C-k kills the selected buffer, and C-g cancels.");
	lisp_func_new ("kill-buffer", F_kill_buffer, true,
		       "Kill buffer BUFFER.\n"
		       "With a nil argument, kill the current buffer.");
	lisp_func_new ("insert-buffer", F_insert_buffer, true,
		       "Insert after point the contents of BUFFER.\n"
		       "Puts mark after the inserted text.");
}
