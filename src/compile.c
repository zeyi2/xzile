/* Compilation mode

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

/* `compile' runs a command asynchronously, with its output in the
   *compilation* buffer. Error messages there, like those of GCC and
   Clang ("FILE:LINE:COL: ..."), Rust ("--> FILE:LINE:COL") and Python
   tracebacks, can be visited with `next-error' and `previous-error',
   or RET in the buffer. Make's "Entering directory" messages are
   followed to find relative file names. */

#include "xzile.h"

#include <ctype.h>
#include <signal.h>
#include <strings.h>
#include <time.h>
#include <sys/wait.h>

#define COMPILATION_BUFFER "*compilation*"
#define DEFAULT_COMPILE_COMMAND "make -k "

enum DirChange { DIR_NONE, DIR_ENTER, DIR_LEAVE, DIR_DASH_C };

/* A line of output that changes the directories of later messages. */
typedef struct {
	size_t o;
	enum DirChange change;
	char *dir;
} DirLine;

struct Compilation {
	Buffer *bp;
	Process *proc;
	char *command;
	char *dir;
	int64_t start_ms;
	size_t error_o;		/* Start of the current error's line, or NOPOS. */
	int escape;		/* ANSI escape state: 0 none, 1 after ESC, 2 in CSI, 3 in OSC. */
	bool cr;		/* A carriage return is pending. */
	DirLine *dirs;		/* The directory changes of the lines */
	size_t ndirs, dirs_cap;	/* before dirs_upto, read when */
	size_t dirs_upto;	/* bp->changes was dirs_changes. */
	size_t dirs_changes;
};

enum ErrorType { ERROR_ERROR, ERROR_WARNING, ERROR_INFO };

typedef struct {
	char *file;
	long line, col;
	enum ErrorType type;
	size_t file_start, file_end;	/* The location within the line. */
	size_t loc_end;
} ErrorInfo;

static Binding *compilation_map;
static Buffer *last_compilation;
static char *last_compile_command;

static int compilation_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
				  size_t len, int start_state, const char **face_names);
static const Mode compilation_mode = { "Compilation", compilation_scan_line, NULL, NULL, 0 };

bool
compilation_process_alive (const Buffer *bp)
{
	return bp->compilation != NULL && process_alive (bp->compilation->proc);
}

void
compilation_free (Buffer *bp)
{
	Compilation *c = bp->compilation;
	if (c == NULL)
		return;
	process_free (c->proc);
	free (c->command);
	free (c->dir);
	for (size_t i = 0; i < c->ndirs; i++)
		free (c->dirs[i].dir);
	free (c->dirs);
	free (c);
	bp->compilation = NULL;
	if (last_compilation == bp)
		last_compilation = NULL;
}


/* Parsing error messages. */

static bool
parse_digits (const char *s, size_t len, size_t *i, long *n)
{
	size_t start = *i;
	long v = 0;
	while (*i < len && isdigit ((unsigned char) s[*i]) && *i - start < 9)
		v = v * 10 + (s[(*i)++] - '0');
	*n = v;
	return *i > start;
}

static enum ErrorType
error_type (const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (strncasecmp (s + i, "warning", 7) == 0)
			return ERROR_WARNING;
		if (strncmp (s + i, "note:", 5) == 0 || strncmp (s + i, "info:", 5) == 0)
			return ERROR_INFO;
	}
	return ERROR_ERROR;
}

/* Parse "FILE:LINE[:COL]:" at `i'. */
static bool
parse_gnu (const char *s, size_t len, size_t i, ErrorInfo *e)
{
	size_t start = i;
	while (i < len && s[i] != ':' && s[i] != ' ' && s[i] != '\t' && s[i] != '(' && s[i] != '"')
		i++;
	if (i == start || i >= len || s[i] != ':')
		return false;
	size_t file_end = i++;
	if (!parse_digits (s, len, &i, &e->line))
		return false;
	e->col = 0;
	if (i + 1 < len && s[i] == ':' && isdigit ((unsigned char) s[i + 1])) {
		i++;
		parse_digits (s, len, &i, &e->col);
	}
	/* The location ends with a colon, or the line. */
	if (i < len && s[i] != ':' && s[i] != ',' && s[i] != ' ')
		return false;
	e->file_start = start;
	e->file_end = file_end;
	e->loc_end = i;
	return true;
}

static bool
parse_error_line (const char *s, size_t len, ErrorInfo *e)
{
	memset (e, 0, sizeof *e);
	size_t i = 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;

	bool ok = false;
	if (len - i > 4 && strncmp (s + i, "--> ", 4) == 0)
		/* Rust. */
		ok = parse_gnu (s, len, i + 4, e);
	else if (len - i > 6 && strncmp (s + i, "File \"", 6) == 0) {
		/* Python: File "FILE", line LINE */
		size_t start = i + 6, end = start;
		while (end < len && s[end] != '"')
			end++;
		size_t j = end + 1;
		if (end < len && j + 7 <= len && strncmp (s + j, ", line ", 7) == 0) {
			j += 7;
			if (parse_digits (s, len, &j, &e->line)) {
				e->file_start = start;
				e->file_end = end;
				e->loc_end = j;
				ok = true;
			}
		}
	} else {
		/* GCC's include stack: "In file included from F:L:", "from F:L:". */
		if (len - i > 22 && strncmp (s + i, "In file included from ", 22) == 0)
			i += 22;
		else if (i > 0 && len - i > 5 && strncmp (s + i, "from ", 5) == 0)
			i += 5;
		ok = parse_gnu (s, len, i, e);
	}
	if (!ok || e->line <= 0)
		return false;

	e->file = xstrndup (s + e->file_start, e->file_end - e->file_start);
	e->type = error_type (s + e->loc_end, len - e->loc_end);
	return true;
}

static char *
line_text (Buffer *bp, size_t lo, size_t *len)
{
	size_t eol = buffer_end_of_line (bp, lo);
	char *s = xmalloc (eol - lo + 1);
	for (size_t o = lo; o < eol; o++)
		s[o - lo] = buffer_get_char (bp, o);
	s[eol - lo] = '\0';
	*len = eol - lo;
	return s;
}

static bool
parse_error_at (Buffer *bp, size_t lo, ErrorInfo *e)
{
	size_t len;
	char *s = line_text (bp, lo, &len);
	bool ok = parse_error_line (s, len, e);
	free (s);
	return ok;
}

/* Extract DIR from make's "Entering directory 'DIR'" message. */
static char *
make_directory (const char *s, const char *what)
{
	const char *p = strstr (s, what);
	if (p == NULL)
		return NULL;
	p += strlen (what);
	if (*p != '\'' && *p != '`' && *p != '"')
		return NULL;
	const char *start = ++p;
	while (*p != '\0' && *p != '\'' && *p != '"')
		p++;
	return *p != '\0' ? xstrndup (start, (size_t) (p - start)) : NULL;
}

/* Extract DIR from "-C DIR" in a command line echoed by make. */
static char *
dash_c_directory (const char *s)
{
	const char *p = strstr (s, " -C ");
	if (p == NULL)
		return NULL;
	p += 4;
	while (*p == ' ')
		p++;
	size_t n = strcspn (p, " \t;&|");
	return n > 0 ? xstrndup (p, n) : NULL;
}

/* What the line `s' says of make's directories, in `*dir' (to free). */
static enum DirChange
directory_change (const char *s, char **dir)
{
	if ((*dir = make_directory (s, "Entering directory ")) != NULL)
		return DIR_ENTER;
	if ((*dir = make_directory (s, "Leaving directory ")) != NULL)
		return DIR_LEAVE;
	if ((*dir = dash_c_directory (s)) != NULL)
		return DIR_DASH_C;
	return DIR_NONE;
}

/* Note the directory changes of the lines of `c' before `o', reading
   them all again if the text has changed since but by output appended
   (see append): the buffer may have been edited. */
static void
scan_directories (Buffer *bp, Compilation *c, size_t o)
{
	if (c->dirs_changes != bp->changes) {
		for (size_t i = 0; i < c->ndirs; i++)
			free (c->dirs[i].dir);
		c->ndirs = 0;
		c->dirs_upto = 0;
		c->dirs_changes = bp->changes;
	}
	while (c->dirs_upto < o) {
		size_t lo = c->dirs_upto, next = buffer_next_line (bp, lo);
		if (next == NOPOS)
			break;
		size_t len;
		char *s = line_text (bp, lo, &len), *dir;
		enum DirChange change = directory_change (s, &dir);
		free (s);
		if (change != DIR_NONE) {
			if (c->ndirs == c->dirs_cap) {
				c->dirs_cap = c->dirs_cap > 0 ? 2 * c->dirs_cap : 16;
				c->dirs = xrealloc (c->dirs, c->dirs_cap * sizeof *c->dirs);
			}
			c->dirs[c->ndirs++] = (DirLine) { lo, change, dir };
		}
		c->dirs_upto = next;
	}
}

/* Directories in which to look for files named by the message at `o',
   most likely first: make's current directory, then directories given
   to `make -C', then the compilation's directory. */
static void
directories_at (Buffer *bp, size_t o, StrVec *out)
{
	Compilation *c = bp->compilation;
	const char *base = c != NULL ? c->dir : bp->dir;
	StrVec stack = { NULL, 0, 0 }, dash_c = { NULL, 0, 0 };
	if (c != NULL)
		scan_directories (bp, c, o);
	size_t i = 0;
	for (size_t lo = c != NULL ? c->dirs_upto : 0; ; i++) {
		/* The lines read already, then any others. */
		enum DirChange change;
		char *dir;
		if (c != NULL && i < c->ndirs) {
			if (c->dirs[i].o >= o)
				break;
			change = c->dirs[i].change;
			dir = xstrdup (c->dirs[i].dir);
		} else {
			if (lo == NOPOS || lo >= o)
				break;
			size_t len;
			char *s = line_text (bp, lo, &len);
			change = directory_change (s, &dir);
			free (s);
			lo = buffer_next_line (bp, lo);
		}
		if (change == DIR_ENTER)
			sv_push (&stack, dir);
		else if (change == DIR_LEAVE && stack.n > 0)
			free (stack.v[--stack.n]);
		else if (change == DIR_DASH_C)
			sv_push (&dash_c, dir);
		free (dir);
	}

	if (stack.n > 0)
		sv_push (out, stack.v[stack.n - 1]);
	for (size_t i = dash_c.n; i > 0; i--)
		sv_push (out, dash_c.v[i - 1]);
	sv_push (out, base);
	/* Make relative directories absolute. */
	for (size_t i = 0; i < out->n; i++)
		if (out->v[i][0] != '/') {
			char *abs = path_join (base, out->v[i]);
			free (out->v[i]);
			out->v[i] = abs;
		}
	sv_clear (&stack);
	sv_clear (&dash_c);
}


/* Output. */

static void
append (Buffer *bp, const char *s, size_t len)
{
	size_t old_len = buffer_length (bp), pt = buffer_pt (bp);
	bool readonly = bp->readonly;
	bool dirs_current = bp->compilation != NULL && bp->compilation->dirs_changes == bp->changes;
	bp->readonly = false;
	buffer_goto_offset (bp, old_len);
	buffer_insert_estr (bp, ev_of (s, len, eol_lf));
	/* The lines noted all end before `old_len', so are unchanged. */
	if (dirs_current)
		bp->compilation->dirs_changes = bp->changes;
	/* Follow the output if point was at the end. */
	if (pt != old_len)
		buffer_goto_offset (bp, pt);
	bp->readonly = readonly;
	bp->modified = false;

	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (wp != cur_wp && wp->bp == bp && wp->saved_pt != NULL && wp->saved_pt->o == old_len)
			wp->saved_pt->o = buffer_length (bp);
}

static void
on_output (Process *p, const char *data, size_t len, void *owner)
{
	(void) p;
	Compilation *c = owner;
	StrBuf sb;
	sb_init (&sb);
	for (size_t i = 0; i < len; i++) {
		char ch = data[i];
		switch (c->escape) {
		case 1:
			c->escape = ch == '[' ? 2 : ch == ']' ? 3 : 0;
			continue;
		case 2:
			if ((unsigned char) ch >= 0x40 && (unsigned char) ch <= 0x7e)
				c->escape = 0;
			continue;
		case 3:
			if (ch == '\a')
				c->escape = 0;
			else if (ch == '\033')
				c->escape = 1;
			continue;
		default:
			break;
		}
		if (ch == '\033') {
			c->escape = 1;
			continue;
		}
		if (c->cr && ch != '\n') {
			/* A lone carriage return, e.g. from a progress meter. */
			c->cr = false;
		}
		if (ch == '\r') {
			c->cr = true;
			continue;
		}
		c->cr = false;
		sb_addc (&sb, ch);
	}
	if (sb.len > 0)
		append (c->bp, sb.s, sb.len);
	sb_free (&sb);
}

static void
format_now (char *buf, size_t size)
{
	static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static const char *const months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
	};
	time_t t = time (NULL);
	struct tm tm;
	localtime_r (&t, &tm);
	snprintf (buf, size, "%s %s %2d %02d:%02d:%02d", days[tm.tm_wday % 7], months[tm.tm_mon % 12],
		  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void
on_process_exit (Process *p, int status, void *owner)
{
	(void) p;
	Compilation *c = owner;
	char when[64], *how, *label;
	format_now (when, sizeof when);
	double secs = (double) (monotonic_ms () - c->start_ms) / 1000.0;

	if (WIFEXITED (status) && WEXITSTATUS (status) == 0) {
		how = xstrdup ("finished");
		label = xstrdup ("Compilation:exit [0]");
	} else if (WIFEXITED (status)) {
		how = xasprintf ("exited abnormally with code %d", WEXITSTATUS (status));
		label = xasprintf ("Compilation:exit [%d]", WEXITSTATUS (status));
	} else {
		int sig = WIFSIGNALED (status) ? WTERMSIG (status) : 0;
		how = xstrdup (sig == SIGINT ? "interrupt" : "killed");
		label = xasprintf ("Compilation:signal [%s]", how);
	}

	char *msg = xasprintf ("\nCompilation %s at %s, duration %.2f s\n", how, when, secs);
	append (c->bp, msg, strlen (msg));
	free (msg);
	free (c->bp->mode_label);
	c->bp->mode_label = label;
	minibuf_write ("Compilation %s", how);
	free (how);
}

static void init_compilation_map (void);

static bool
run_compile (const char *command, const char *dir)
{
	Buffer *bp = buffer_find (COMPILATION_BUFFER);
	if (bp != NULL && compilation_process_alive (bp)) {
		int ans = minibuf_read_yesno ("A compilation process is running; kill it? (yes or no) ");
		if (ans != 1)
			return ans < 0 ? funcall ("keyboard-quit") : false;
		compilation_free (bp);
	}

	if (bp == NULL) {
		bp = buffer_new (NULL);
		bp->name = xstrdup (COMPILATION_BUFFER);
		bp->nosave = true;
		bp->noundo = true;
		init_compilation_map ();
		bp->keymap = compilation_map;
		buffer_set_highlighter (bp, &compilation_mode);
	} else
		compilation_free (bp);

	Compilation *c = xzalloc (sizeof (Compilation));
	c->bp = bp;
	c->command = xstrdup (command);
	c->dir = xstrdup (dir);
	c->start_ms = monotonic_ms ();
	c->error_o = NOPOS;
	bp->compilation = c;
	last_compilation = bp;
	/* `dir' may be bp->dir itself, when compiling again. */
	char *new_dir = xstrdup (dir);
	free (bp->dir);
	bp->dir = new_dir;
	dir = new_dir;

	char when[64];
	format_now (when, sizeof when);
	char *shown_dir = compact_path (dir);
	char *header = xasprintf ("-*- mode: compilation; default-directory: \"%s%s\" -*-\n"
				  "Compilation started at %s\n\n%s\n",
				  shown_dir, str_has_suffix (shown_dir, "/") ? "" : "/", when, command);
	free (shown_dir);
	bp->readonly = false;
	buffer_goto_offset (bp, 0);
	buffer_replace_estr (bp, buffer_length (bp), ev_cstr (header));
	free (header);
	bp->readonly = true;
	bp->modified = false;
	buffer_goto_offset (bp, 0);

	free (bp->mode_label);
	bp->mode_label = xstrdup ("Compilation:run");

	/* Show the buffer without selecting it. */
	if (window_showing (bp) == NULL)
		window_show_buffer (cur_bp == bp ? cur_wp : popup_window (), bp, 0);
	else
		for (Window *wp = head_wp; wp != NULL; wp = wp->next)
			if (wp->bp == bp)
				window_show_buffer (wp, bp, 0);

	static const char *const env[] = { "TERM=dumb", "INSIDE_XZILE=1", NULL };
	const char *argv[] = { "/bin/sh", "-c", command, NULL };
	c->proc = process_start (argv, NULL, dir, false, 0, 0, env, on_output, on_process_exit, c);
	if (c->proc == NULL) {
		minibuf_error ("Cannot run `%s'", command);
		return false;
	}
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}


/* Visiting errors. */

static Buffer *
error_buffer (void)
{
	if (last_compilation != NULL && buffer_is_live (last_compilation))
		return last_compilation;
	return buffer_find (COMPILATION_BUFFER);
}

/* Find the start of the next (or previous) error line after (before) `o'. */
static size_t
find_error (Buffer *bp, size_t o, bool forward)
{
	size_t lo;
	if (forward)
		lo = o == NOPOS ? 0 : buffer_next_line (bp, o);
	else
		lo = o == NOPOS ? NOPOS : buffer_prev_line (bp, o);
	for (; lo != NOPOS; lo = forward ? buffer_next_line (bp, lo) : buffer_prev_line (bp, lo)) {
		ErrorInfo e;
		if (parse_error_at (bp, lo, &e)) {
			bool info = e.type == ERROR_INFO;
			free (e.file);
			if (!info)
				return lo;
		}
	}
	return NOPOS;
}

/* The file named `file' by the message at `lo' in `bp', or NULL if
   there is no such file. */
static char *
error_path (Buffer *bp, size_t lo, const char *file)
{
	if (file[0] == '/')
		return file_is_regular (file) ? xstrdup (file) : NULL;
	char *path = NULL;
	StrVec dirs = { NULL, 0, 0 };
	directories_at (bp, lo, &dirs);
	for (size_t i = 0; i < dirs.n && path == NULL; i++) {
		char *p = path_join (dirs.v[i], file);
		if (file_is_regular (p))
			path = p;
		else
			free (p);
	}
	sv_clear (&dirs);
	return path;
}

/* Move point to line `line', column `col' (both from 1; a column of 0
   is the start of the line). */
static void
goto_line_col (Buffer *bp, long line, long col)
{
	buffer_goto_offset (bp, 0);
	buffer_move_line (bp, line - 1);
	buffer_goto_offset (bp, buffer_line_o (bp));
	for (long i = 1; i < col && !buffer_eolp (bp); i++)
		buffer_move_char (bp, 1);
	thisflag |= FLAG_NEED_RESYNC;
}

static bool
visit_error (Buffer *bp, size_t lo)
{
	ErrorInfo e;
	if (!parse_error_at (bp, lo, &e))
		return false;

	char *path = error_path (bp, lo, e.file);
	if (path == NULL) {
		minibuf_error ("Cannot find file `%s'", e.file);
		free (e.file);
		return false;
	}

	if (bp->compilation != NULL)
		bp->compilation->error_o = lo;

	/* Visit the file in a window other than the compilation buffer's,
	   and show the error there. */
	Window *cwp = window_showing (bp), *target;
	if (cur_wp->bp == bp) {
		cwp = cur_wp;
		target = popup_window ();
	} else {
		target = cur_wp;
		if (cwp == NULL)
			cwp = popup_window ();
	}
	window_show_buffer (cwp, bp, lo);

	window_set_current (target);
	bool ok = find_file (path);
	if (ok)
		goto_line_col (cur_bp, e.line, e.col);
	free (path);
	free (e.file);
	return ok;
}

/* Whether a reference may start at byte `i' of `s': at the start of
   the line, or after a space, an opening bracket or a quote. */
static bool
reference_start (const char *s, size_t i)
{
	return i == 0 || strchr (" \t([{<\"'`=,;|", s[i - 1]) != NULL;
}

/* The references to places in files in the line `s', in `*refs' (to
   free with free_references), in the order in which a click tries them:
   the location of an error message, as `next-error' visits, then each
   "FILE:LINE[:COL]" from the left, as grep -n prints. A reference is
   from the start of FILE to the end of LINE or COL. */
static void
add_reference (ErrorInfo **v, size_t *n, size_t *cap, const ErrorInfo *e)
{
	if (*n == *cap) {
		*cap = *cap > 0 ? 2 * *cap : 4;
		*v = xrealloc (*v, *cap * sizeof **v);
	}
	(*v)[(*n)++] = *e;
}

static size_t
references (const char *s, size_t len, ErrorInfo **refs)
{
	ErrorInfo *v = NULL, e;
	size_t n = 0, cap = 0, error_start = NOPOS;
	if (parse_error_line (s, len, &e)) {
		error_start = e.file_start;
		add_reference (&v, &n, &cap, &e);
	}
	for (size_t j = 0; j < len; j++) {
		memset (&e, 0, sizeof e);
		if (j != error_start && reference_start (s, j) && parse_gnu (s, len, j, &e) && e.line > 0) {
			e.file = xstrndup (s + e.file_start, e.file_end - e.file_start);
			add_reference (&v, &n, &cap, &e);
		}
	}
	*refs = v;
	return n;
}
static void
free_references (ErrorInfo *refs, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free (refs[i].file);
	free (refs);
}

/* The name `file' in the terminal `bp' joined to its directory, which
   is on bp->remote if that is not NULL. */
static char *
terminal_name (Buffer *bp, const char *file)
{
	if (file[0] == '/' || file[0] == '~')
		return xstrdup (file);
	char *dir = bp->remote != NULL ? remote_buffer_dir (bp) : xstrdup (bp->dir);
	char *joined = path_join (dir, file);
	free (dir);
	return joined;
}

/* The file to which the reference `e', in the line at `lo' of `bp',
   leads, in `*path' (to free): in a terminal on another machine (on
   bp->remote), it is not looked for, and is NULL. Returns false if
   there is no such file. */
static bool
reference_path (Buffer *bp, size_t lo, const ErrorInfo *e, char **path)
{
	*path = NULL;
	if (bp->compilation != NULL)
		*path = error_path (bp, lo, e->file);
	else if (bp->remote != NULL)
		return true;
	else {
		char *joined = terminal_name (bp, e->file);
		*path = expand_path (joined);
		free (joined);
		if (*path != NULL && !file_is_regular (*path)) {
			free (*path);
			*path = NULL;
		}
	}
	return *path != NULL;
}

/* A reference that leads to a file. */
typedef struct {
	LinkSpan span;
	ErrorInfo e;
	char *path;	/* As for reference_path. */
} Link;

static void
free_links (Link *links, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free (links[i].e.file);
		free (links[i].path);
	}
	free (links);
}

/* The references that lead to files in the line `s' of `bp', sorted
   and apart, in `*out' (to free with free_links): each byte belongs to
   the first reference of those that `references' gives over it that
   leads to a file, which is what a click there follows. Only names of
   local files are looked for: in *compilation*, in the directories of
   the message at `lo' (see directories_at). */
static size_t
find_links (Buffer *bp, const char *s, size_t len, size_t lo, Link **out)
{
	*out = NULL;
	if (len == 0)
		return 0;
	ErrorInfo *refs;
	size_t nrefs = references (s, len, &refs);
	size_t *owner = xmalloc (len * sizeof *owner);
	char **paths = xzalloc ((nrefs + 1) * sizeof *paths);
	for (size_t i = 0; i < len; i++)
		owner[i] = NOPOS;
	for (size_t k = 0; k < nrefs; k++) {
		size_t from = refs[k].file_start, to = MIN (refs[k].loc_end, len), i = from;
		while (i < to && owner[i] != NOPOS)
			i++;
		if (i == to || !reference_path (bp, lo, &refs[k], &paths[k]))
			continue;
		for (; i < to; i++)
			if (owner[i] == NOPOS)
				owner[i] = k;
	}

	Link *links = NULL;
	size_t n = 0, cap = 0;
	for (size_t i = 0; i < len;) {
		size_t k = owner[i], j = i + 1;
		while (j < len && owner[j] == k)
			j++;
		if (k != NOPOS) {
			if (n == cap) {
				cap = cap > 0 ? 2 * cap : 4;
				links = xrealloc (links, cap * sizeof *links);
			}
			Link *l = &links[n++];
			l->span = (LinkSpan) { i, j };
			l->e = refs[k];
			l->e.file = xstrdup (refs[k].file);
			l->path = xstrdup_or_null (paths[k]);
		}
		i = j;
	}
	for (size_t k = 0; k < nrefs; k++)
		free (paths[k]);
	free (paths);
	free (owner);
	free_references (refs, nrefs);
	*out = links;
	return n;
}

/* The link of `links' over byte `i', or NULL. */
static Link *
link_over (Link *links, size_t n, size_t i)
{
	for (size_t k = 0; k < n; k++)
		if (i >= links[k].span.start && i < links[k].span.end)
			return &links[k];
	return NULL;
}

bool
references_followable (const Buffer *bp)
{
	if (bp->compilation != NULL)
		return true;
	if (bp->shell == NULL)
		return false;
	char *where = shell_elsewhere (bp);
	bool here = where == NULL;
	free (where);
	return here;
}

size_t
reference_links (Buffer *bp, const char *s, size_t len, size_t lo, LinkSpan **spans)
{
	Link *links;
	size_t n = find_links (bp, s, len, lo, &links);
	*spans = n > 0 ? xmalloc (n * sizeof **spans) : NULL;
	for (size_t k = 0; k < n; k++)
		(*spans)[k] = links[k].span;
	free_links (links, n);
	return n;
}

size_t
reference_line_links (Buffer *bp, size_t lo, LinkSpan **spans)
{
	size_t len;
	char *s = line_text (bp, lo, &len);
	size_t n = reference_links (bp, s, len, lo, spans);
	free (s);
	return n;
}

bool
reference_link_at (Buffer *bp, const char *s, size_t len, size_t i, size_t lo, LinkSpan *span,
		   char **target)
{
	Link *links;
	size_t n = find_links (bp, s, len, lo, &links);
	Link *l = link_over (links, n, i);
	bool found = l != NULL;
	if (found) {
		*span = l->span;
		char *name;
		if (l->path != NULL)
			name = compact_path (l->path);
		else {
			char *joined = terminal_name (bp, l->e.file);
			name = xasprintf ("%s on %s", joined, remote_label (bp->remote));
			free (joined);
		}
		if (l->e.col > 0)
			*target = xasprintf ("%s\nLine %ld, column %ld", name, l->e.line, l->e.col);
		else
			*target = xasprintf ("%s\nLine %ld", name, l->e.line);
		free (name);
	}
	free_links (links, n);
	return found;
}

bool
reference_line_link_at (Buffer *bp, size_t lo, size_t i, LinkSpan *span, char **target)
{
	size_t len;
	char *s = line_text (bp, lo, &len);
	bool ok = reference_link_at (bp, s, len, i, lo, span, target);
	free (s);
	return ok;
}

bool
follow_reference (Window *wp, const char *s, size_t len, size_t i, size_t lo)
{
	Buffer *bp = wp->bp;
	if (bp->compilation == NULL && bp->shell == NULL)
		return false;

	char *where = shell_elsewhere (bp);
	if (where != NULL) {
		/* The file would be looked for on the wrong machine. */
		ErrorInfo *refs;
		size_t n = references (s, len, &refs), k = 0;
		while (k < n && (i < refs[k].file_start || i >= refs[k].loc_end))
			k++;
		if (k < n) {
			minibuf_error ("Cannot visit `%s': the terminal is %s", refs[k].file, where);
			debug_log ("remote", "%s: not visiting %s: the terminal is %s", bp->name, refs[k].file, where);
		}
		free_references (refs, n);
		free (where);
		return k < n;
	}

	Link *links;
	size_t n = find_links (bp, s, len, lo, &links);
	Link *l = link_over (links, n, i);
	if (l == NULL) {
		/* Not a file here: the text is only text. */
		free_links (links, n);
		return false;
	}
	Remote *r = bp->compilation != NULL ? NULL : bp->remote;
	char *path = l->path;
	l->path = NULL;
	if (r != NULL) {
		char *joined = terminal_name (bp, l->e.file);
		path = remote_expand_path (r, joined);
		free (joined);
		if (path == NULL) {
			minibuf_error ("%s", remote_last_error ());
			free_links (links, n);
			return true;
		}
	}

	if (bp->compilation != NULL && lo != NOPOS) {
		ErrorInfo whole;
		if (parse_error_at (bp, lo, &whole)) {
			bp->compilation->error_o = lo;
			free (whole.file);
		}
	}
	bool is_dir;
	if (find_file_for_link (wp, r, path, &is_dir))
		goto_line_col (cur_bp, l->e.line, l->e.col);
	else if (is_dir)
		minibuf_error ("`%s' is a directory", l->e.file);
	free (path);
	free_links (links, n);
	return true;
}

bool
follow_reference_at (Window *wp, size_t o)
{
	size_t lo = buffer_start_of_line (wp->bp, o), len;
	char *s = line_text (wp->bp, lo, &len);
	bool ok = follow_reference (wp, s, len, o - lo, lo);
	free (s);
	return ok;
}
static bool
next_error (long n)
{
	Buffer *bp = error_buffer ();
	if (bp == NULL || bp->compilation == NULL) {
		minibuf_error ("No buffers contain error message locations");
		return false;
	}

	bool forward = n >= 0;
	size_t o = bp->compilation->error_o;
	for (long i = 0; i < (n < 0 ? -n : n); i++) {
		size_t found = find_error (bp, o, forward);
		if (found == NOPOS) {
			minibuf_error (forward ? "Moved past last error" : "Moved back before first error");
			return false;
		}
		o = found;
	}
	return visit_error (bp, o);
}

static bool
F_next_error (long uniarg, Args *args)
{
	(void) args;
	return next_error (uniarg);
}

static bool
F_previous_error (long uniarg, Args *args)
{
	(void) args;
	return next_error (-uniarg);
}

static bool
F_compile_goto_error (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	ErrorInfo e;
	size_t lo = buffer_line_o (cur_bp);
	if (!parse_error_at (cur_bp, lo, &e)) {
		minibuf_error ("No error message on this line");
		return false;
	}
	free (e.file);
	return visit_error (cur_bp, lo);
}

static bool
move_to_error (long n)
{
	bool forward = n >= 0;
	size_t o = buffer_line_o (cur_bp);
	for (long i = 0; i < (n < 0 ? -n : n); i++) {
		size_t found = find_error (cur_bp, o, forward);
		if (found == NOPOS) {
			minibuf_error (forward ? "Moved past last error" : "Moved back before first error");
			return false;
		}
		o = found;
	}
	buffer_goto_offset (cur_bp, o);
	return true;
}

static bool
F_compilation_next_error (long uniarg, Args *args)
{
	(void) args;
	return move_to_error (uniarg);
}

static bool
F_compilation_previous_error (long uniarg, Args *args)
{
	(void) args;
	return move_to_error (-uniarg);
}


/* Commands. */

static bool
F_compile (long uniarg, Args *args)
{
	(void) uniarg;
	if (remote_refuse (cur_bp, "compile"))
		return false;
	char *command = xstrdup_or_null (args_poll (args));
	if (command == NULL) {
		command = minibuf_read ("Compile command: ", last_compile_command != NULL ? last_compile_command : DEFAULT_COMPILE_COMMAND);
		if (command == NULL)
			return funcall ("keyboard-quit");
	}
	if (command[0] == '\0') {
		free (command);
		return false;
	}
	free (last_compile_command);
	last_compile_command = xstrdup (command);
	bool ok = run_compile (command, cur_bp->dir);
	free (command);
	return ok;
}

static bool
F_recompile (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (remote_refuse (cur_bp, "recompile"))
		return false;
	Buffer *bp = error_buffer ();
	if (bp != NULL && bp->compilation != NULL) {
		char *command = xstrdup (bp->compilation->command);
		char *dir = xstrdup (bp->compilation->dir);
		bool ok = run_compile (command, dir);
		free (command);
		free (dir);
		return ok;
	}
	if (last_compile_command == NULL) {
		minibuf_error ("No previous compilation");
		return false;
	}
	return run_compile (last_compile_command, cur_bp->dir);
}

static bool
F_kill_compilation (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Buffer *bp = error_buffer ();
	if (bp == NULL || !compilation_process_alive (bp)) {
		minibuf_error ("No compilation process");
		return false;
	}
	process_signal (bp->compilation->proc, SIGINT);
	return true;
}


/* Highlighting. */

static int
compilation_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		       size_t len, int start_state, const char **face_names)
{
	(void) mode;
	(void) line_idx;
	(void) start_state;
	for (size_t i = 0; i < len; i++)
		face_names[i] = NULL;

	size_t n;
	char *s = line_text (bp, start_o, &n);
	ErrorInfo e;
	if (parse_error_line (s, n, &e)) {
		bool current = bp->compilation != NULL && bp->compilation->error_o == start_o;
		const char *face = current ? FACE_MATCH : e.type == ERROR_WARNING ? FACE_WARNING :
			e.type == ERROR_INFO ? FACE_SUCCESS : FACE_ERROR;
		for (size_t i = e.file_start; i < e.file_end && i < len; i++)
			face_names[i] = face;
		for (size_t i = e.file_end + 1; i < e.loc_end && i < len; i++)
			face_names[i] = FACE_FONT_LOCK_CONSTANT;
		free (e.file);
	} else if (str_has_prefix (s, "Compilation ") && !str_has_prefix (s, "Compilation started")) {
		/* "Compilation finished at ..." or "Compilation exited abnormally ...". */
		const char *at = strstr (s, " at ");
		size_t to = at != NULL ? (size_t) (at - s) : n;
		const char *face = str_has_prefix (s, "Compilation finished") ? FACE_SUCCESS : FACE_ERROR;
		for (size_t i = 12; i < to && i < len; i++)
			face_names[i] = face;
	}
	free (s);
	return SYNTAX_STATE_NORMAL;
}

static void
init_compilation_map (void)
{
	if (compilation_map != NULL)
		return;
	compilation_map = keymap_new ();
	keymap_bind (compilation_map, "\\RET", "compile-goto-error");
	keymap_bind (compilation_map, "\\C-c\\C-c", "compile-goto-error");
	keymap_bind (compilation_map, "n", "compilation-next-error");
	keymap_bind (compilation_map, "\\M-n", "compilation-next-error");
	keymap_bind (compilation_map, "p", "compilation-previous-error");
	keymap_bind (compilation_map, "\\M-p", "compilation-previous-error");
	keymap_bind (compilation_map, "g", "recompile");
	keymap_bind (compilation_map, "\\C-c\\C-k", "kill-compilation");
	keymap_bind (compilation_map, "q", "quit-window");
}

void
compile_init (void)
{
	lisp_func_new ("compile", F_compile, true,
		       "Compile the program, running COMMAND asynchronously.\n"
		       "The output goes to the *compilation* buffer, where RET visits the\n"
		       "error message at point, n and p move between messages, g reruns\n"
		       "the command and C-c C-k interrupts it.\n"
		       "Use \\[next-error] to visit each error message in turn.");
	lisp_func_new ("recompile", F_recompile, true,
		       "Rerun the last compilation command, in the same directory.");
	lisp_func_new ("kill-compilation", F_kill_compilation, true,
		       "Interrupt the running compilation.");
	lisp_func_new ("next-error", F_next_error, true,
		       "Visit the next error message and the source code it refers to.\n"
		       "With argument N, move N messages; negative N moves backwards.");
	lisp_func_new ("previous-error", F_previous_error, true,
		       "Visit the previous error message and the source code it refers to.");
	lisp_func_new ("compile-goto-error", F_compile_goto_error, true,
		       "Visit the source for the error message at point.");
	lisp_func_new ("compilation-next-error", F_compilation_next_error, true,
		       "Move point to the next error message, without visiting it.");
	lisp_func_new ("compilation-previous-error", F_compilation_previous_error, true,
		       "Move point to the previous error message, without visiting it.");
}
