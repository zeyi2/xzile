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

struct Compilation {
	Buffer *bp;
	Process *proc;
	char *command;
	char *dir;
	int64_t start_ms;
	size_t error_o;		/* Start of the current error's line, or NOPOS. */
	int escape;		/* ANSI escape state: 0 none, 1 after ESC, 2 in CSI, 3 in OSC. */
	bool cr;		/* A carriage return is pending. */
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

/* Directories in which to look for files named by the message at `o',
   most likely first: make's current directory, then directories given
   to `make -C', then the compilation's directory. */
static void
directories_at (Buffer *bp, size_t o, StrVec *out)
{
	const char *base = bp->compilation != NULL ? bp->compilation->dir : bp->dir;
	StrVec stack = { NULL, 0, 0 }, dash_c = { NULL, 0, 0 };
	for (size_t lo = 0; lo != NOPOS && lo < o; lo = buffer_next_line (bp, lo)) {
		size_t len;
		char *s = line_text (bp, lo, &len);
		char *dir;
		if ((dir = make_directory (s, "Entering directory ")) != NULL)
			sv_push (&stack, dir);
		else if ((dir = make_directory (s, "Leaving directory ")) != NULL) {
			if (stack.n > 0)
				free (stack.v[--stack.n]);
		} else if ((dir = dash_c_directory (s)) != NULL)
			sv_push (&dash_c, dir);
		free (dir);
		free (s);
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
	bp->readonly = false;
	buffer_goto_offset (bp, old_len);
	buffer_insert_estr (bp, ev_of (s, len, eol_lf));
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
	free (bp->dir);
	bp->dir = xstrdup (dir);

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

static bool
visit_error (Buffer *bp, size_t lo)
{
	ErrorInfo e;
	if (!parse_error_at (bp, lo, &e))
		return false;

	char *path = NULL;
	if (e.file[0] == '/')
		path = xstrdup (e.file);
	else {
		StrVec dirs = { NULL, 0, 0 };
		directories_at (bp, lo, &dirs);
		for (size_t i = 0; i < dirs.n && path == NULL; i++) {
			char *p = path_join (dirs.v[i], e.file);
			if (file_is_regular (p))
				path = p;
			else
				free (p);
		}
		sv_clear (&dirs);
	}
	if (path == NULL || !file_is_regular (path)) {
		minibuf_error ("Cannot find file `%s'", e.file);
		free (path);
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
	if (ok) {
		buffer_goto_offset (cur_bp, 0);
		buffer_move_line (cur_bp, e.line - 1);
		buffer_goto_offset (cur_bp, buffer_line_o (cur_bp));
		for (long i = 1; i < e.col && !buffer_eolp (cur_bp); i++)
			buffer_move_char (cur_bp, 1);
		thisflag |= FLAG_NEED_RESYNC;
	}
	free (path);
	free (e.file);
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
