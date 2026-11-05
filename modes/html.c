/* HTML mode, and its preview in a browser engine

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

/* HTML is edited as text. html-preview (C-c C-v in html-mode) shows the
   file in a browser engine in a window of its own: the WebKitGTK helper
   xzile-html-preview on Linux (html_preview.c), or the system browser.
   The helper reloads when the file changes; SIGUSR1 makes it reload
   and come to the front. It is a background process of the buffer,
   ended with it or with the editor. */

#include "../src/xzile.h"

#include <ctype.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* The preview window of a buffer. */
struct Html {
	Process *proc;
	bool ready;		/* It can take signals. */
	StrBuf line;		/* A partial line of its output. */
	char *last;		/* Its last complete line. */
};

static Binding *html_map;

static Html *
html_get (Buffer *bp)
{
	if (bp->html == NULL) {
		bp->html = xzalloc (sizeof (Html));
		sb_init (&bp->html->line);
	}
	return bp->html;
}

void
html_free (Buffer *bp)
{
	Html *h = bp->html;
	if (h == NULL)
		return;
	process_free (h->proc);
	sb_free (&h->line);
	free (h->last);
	free (h);
	bp->html = NULL;
}

static void
enter_html_mode (Buffer *bp)
{
	if (html_map == NULL) {
		html_map = keymap_new ();
		keymap_bind (html_map, "\\C-c\\C-v", "html-preview");
		keymap_bind (html_map, "\\C-c\\C-k", "html-preview-quit");
	}
	bp->keymap = html_map;
	free (bp->mode_label);
	bp->mode_label = xstrdup ("HTML");
}

static bool
has_suffix_nocase (const char *s, const char *suffix)
{
	size_t n = strlen (s), m = strlen (suffix);
	if (n < m)
		return false;
	for (size_t i = 0; i < m; i++)
		if (tolower ((unsigned char) s[n - m + i]) != suffix[i])
			return false;
	return true;
}

void
html_after_visit (Buffer *bp)
{
	if (bp->filename != NULL && bp->keymap == NULL &&
	    (has_suffix_nocase (bp->filename, ".html") || has_suffix_nocase (bp->filename, ".htm") ||
	     has_suffix_nocase (bp->filename, ".xhtml")))
		enter_html_mode (bp);
}

static bool
F_html_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (preview_showing (cur_bp) || cur_bp->shell != NULL || cur_bp->dired != NULL ||
	    cur_bp->magit != NULL || cur_bp->compilation != NULL) {
		minibuf_error ("Cannot use html-mode in this buffer");
		return false;
	}
	enter_html_mode (cur_bp);
	return true;
}


/* Programs started. */

/* Note the line before each newline in what a program printed. */
static void
note_output (StrBuf *line, char **last, const char *data, size_t len,
	     void (*on_line) (const char *line, void *data), void *owner)
{
	for (size_t i = 0; i < len; i++) {
		if (data[i] != '\n') {
			/* Keep only the start of an endless line. */
			if (line->len < 4096)
				sb_addc (line, data[i]);
			continue;
		}
		if (line->len > 0 && line->s[line->len - 1] == '\r')
			line->s[--line->len] = '\0';
		if (line->len > 0) {
			free (*last);
			*last = xstrdup (line->s);
			if (on_line != NULL)
				on_line (line->s, owner);
		}
		sb_free (line);
		sb_init (line);
	}
}

static void
report_exit (const char *what, int status, const char *last)
{
	if (WIFEXITED (status) && WEXITSTATUS (status) == 0)
		return;
	/* Closed by us, or with the editor. */
	if (WIFSIGNALED (status) && (WTERMSIG (status) == SIGHUP || WTERMSIG (status) == SIGTERM))
		return;
	char *how = WIFEXITED (status) ? xasprintf ("exited with code %d", WEXITSTATUS (status))
		: xasprintf ("was killed by signal %d", WIFSIGNALED (status) ? WTERMSIG (status) : 0);
	minibuf_error ("%s %s%s%s", what, how, last != NULL ? ": " : "", last != NULL ? last : "");
	free (how);
}

static void
helper_line (const char *line, void *owner)
{
	Html *h = owner;
	if (strcmp (line, "ready") == 0)
		h->ready = true;
	else if (str_has_prefix (line, "error: "))
		minibuf_error ("HTML preview: %s", line + 7);
}

static void
helper_output (Process *p, const char *data, size_t len, void *owner)
{
	(void) p;
	Html *h = owner;
	note_output (&h->line, &h->last, data, len, helper_line, h);
}

static void
helper_exit (Process *p, int status, void *owner)
{
	Html *h = owner;
	report_exit ("The HTML preview", status, h->last);
	h->ready = false;
	h->proc = NULL;
	process_free (p);
}

/* A program that hands a file to the browser, and exits. */
typedef struct {
	char *what;
	StrBuf line;
	char *last;
} Launch;

static void
launch_output (Process *p, const char *data, size_t len, void *owner)
{
	(void) p;
	Launch *l = owner;
	note_output (&l->line, &l->last, data, len, NULL, NULL);
}

static void
launch_exit (Process *p, int status, void *owner)
{
	Launch *l = owner;
	report_exit (l->what, status, l->last);
	free (l->what);
	sb_free (&l->line);
	free (l->last);
	free (l);
	process_free (p);
}

static bool
open_in_browser (const char *program, const char *path)
{
	Launch *l = xzalloc (sizeof (Launch));
	l->what = xstrdup (program);
	sb_init (&l->line);
	/* `path' is absolute, so cannot be taken for an option. */
	const char *argv[] = { program, path, NULL };
	Process *p = process_start (argv, NULL, NULL, false, 0, 0, NULL, launch_output, launch_exit, l);
	if (p == NULL) {
		minibuf_error ("Cannot run %s", program);
		free (l->what);
		free (l);
		return false;
	}
	process_set_background (p);
	return true;
}

#ifndef __APPLE__
static bool
is_executable (const char *path)
{
	return file_is_regular (path) && access (path, X_OK) == 0;
}

/* The helper: html-preview-program, found next to this program (so
   that it works from the build directory) or on PATH. */
static char *
find_helper (void)
{
	const char *name = get_variable ("html-preview-program");
	if (name == NULL || *name == '\0' || strcmp (name, "nil") == 0)
		return NULL;
	if (strchr (name, '/') != NULL) {
		char *path = expand_path (name);
		if (path != NULL && is_executable (path))
			return path;
		free (path);
		return NULL;
	}

	char self[4096];
	ssize_t n = readlink ("/proc/self/exe", self, sizeof self - 1);
	if (n > 0) {
		self[n] = '\0';
		char *dir = path_dirname (self);
		char *path = path_join (dir, name);
		free (dir);
		if (is_executable (path))
			return path;
		free (path);
	}

	const char *env = getenv ("PATH");
	char *dirs = xstrdup (env != NULL ? env : "/usr/local/bin:/usr/bin:/bin");
	char *found = NULL;
	for (char *dir = dirs, *end; found == NULL && dir != NULL; dir = end) {
		end = strchr (dir, ':');
		if (end != NULL)
			*end++ = '\0';
		char *path = path_join (*dir != '\0' ? dir : ".", name);
		if (is_executable (path))
			found = path;
		else
			free (path);
	}
	free (dirs);
	return found;
}
#endif

static bool
start_html_preview (Buffer *bp)
{
#ifdef __APPLE__
	/* No native engine here: the file's default application, normally
	   the browser. */
	if (!open_in_browser ("open", bp->filename))
		return false;
	minibuf_write ("Opened %s in the browser", bp->name);
	return true;
#else
	char *helper = find_helper ();
	if (helper == NULL) {
		if (!open_in_browser ("xdg-open", bp->filename))
			return false;
		minibuf_write ("xzile-html-preview is not installed; opened %s with xdg-open", bp->name);
		return true;
	}

	Html *h = html_get (bp);
	const char *argv[5];
	size_t n = 0;
	argv[n++] = helper;
	if (!get_variable_bool ("html-preview-gpu"))
		argv[n++] = "--software";
	argv[n++] = "--";
	argv[n++] = bp->filename;
	argv[n] = NULL;
	h->ready = false;
	free (h->last);
	h->last = NULL;
	sb_free (&h->line);
	sb_init (&h->line);
	h->proc = process_start (argv, NULL, bp->dir, false, 0, 0, NULL, helper_output, helper_exit, h);
	free (helper);
	if (h->proc == NULL) {
		minibuf_error ("Cannot start the HTML preview");
		return false;
	}
	process_set_background (h->proc);
	minibuf_write ("Opening the HTML preview of %s", bp->name);
	return true;
#endif
}

static bool
F_html_preview (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Buffer *bp = cur_bp;
	if (remote_refuse (bp, "html-preview"))
		return false;
	if (bp->filename == NULL || bp->needname) {
		minibuf_error ("Buffer is not visiting a file; save it first");
		return false;
	}
	if (bp->modified) {
		int ans = minibuf_read_y_or_n ("Save %s first? ", bp->name);
		if (ans < 0)
			return false;
		if (ans == 1 && !funcall ("save-buffer"))
			return false;
	}
	if (!file_is_regular (bp->filename)) {
		minibuf_error ("%s does not exist; save it first", bp->filename);
		return false;
	}

	Html *h = bp->html;
	if (h != NULL && process_alive (h->proc)) {
		if (!h->ready) {
			minibuf_write ("The HTML preview is starting");
			return true;
		}
		/* Not process_signal: that signals the whole process group,
		   the browser engine's own processes included. */
		kill (process_pid (h->proc), SIGUSR1);
		minibuf_write ("Reloaded the HTML preview");
		return true;
	}
	return start_html_preview (bp);
}

static bool
F_html_preview_quit (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Html *h = cur_bp->html;
	if (h == NULL || !process_alive (h->proc)) {
		minibuf_error ("No HTML preview is open for this buffer");
		return false;
	}
	process_free (h->proc);
	h->proc = NULL;
	h->ready = false;
	minibuf_write ("Closed the HTML preview");
	return true;
}

void
html_mode_init (void)
{
	lisp_func_new ("html-mode", F_html_mode, true,
		       "Major mode for editing HTML. Visiting an .html file selects it.\n"
		       "C-c C-v (html-preview) shows the file in a browser engine, which\n"
		       "follows changes when the file is saved; C-c C-k closes that window.");
	lisp_func_new ("html-preview", F_html_preview, true,
		       "Show the visited HTML file, rendered by a browser engine, in a window\n"
		       "of its own; if it is open already, reload it and bring it forward.\n"
		       "On Linux this runs `html-preview-program' (xzile-html-preview, built\n"
		       "with `make gui') or else xdg-open; on macOS, the browser.");
	lisp_func_new ("html-preview-quit", F_html_preview_quit, true,
		       "Close the HTML preview window of the current buffer.");
}
