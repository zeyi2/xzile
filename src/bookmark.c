/* Bookmarks

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

/* A bookmark names a place to come back to: a file, or a directory
   listed by Dired, here or on a machine reached by SSH, with the
   position of point there (a byte offset, moved back into the text
   if the text has since become shorter).

   Bookmarks are kept in ~/.xzile.d/bookmarks unless the init file is
   not loaded. The first line is the format version; each other line
   is a record of tab-separated fields, with backslash, tab, newline
   and carriage return written as \\, \t, \n and \r:

     file NAME LOCATION PATH POINT
     dir  NAME LOCATION PATH POINT

   LOCATION is `local', or `ssh:' and the machine as remote_label
   gives it. Records are sorted by name. Records that do not parse are
   ignored; a file of another version, or one that cannot be read, is
   left alone and not replaced. Each change is made holding an
   fcntl(2) lock on bookmarks.lock: the file is read again, changes
   not yet saved and then this one are made to what it holds, and the
   file is replaced by renaming a new one over it, so that sessions
   sharing it neither lose each other's bookmarks nor see a partial
   file. */

#include "xzile.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define STATE_DIR "." PACKAGE ".d"
#define STATE_FILE "bookmarks"
#define STATE_PATH "~/" STATE_DIR "/" STATE_FILE
#define STATE_HEADER PACKAGE "-bookmarks 1"
#define MAX_STATE_SIZE (4u * 1024 * 1024)
#define MAX_STATE_FIELD 4096
#define MAX_NAME 512
/* Larger points are not believed. */
#define MAX_POINT_DIGITS 15
/* How long to wait for another session to finish saving, in ms. */
#define STATE_LOCK_WAIT 2000

#define LIST_NAME "*Bookmark List*"
#define LIST_HEADER_LINES 3
#define NAME_COL 2
#define MAX_NAME_WIDTH 40

typedef struct {
	char *name;
	bool dir;		/* A directory listed by Dired, not a file. */
	Remote *remote;		/* The machine, or NULL for this one. */
	char *path;		/* Absolute. */
	size_t pt;		/* Point, as a byte offset. */
} Bookmark;

typedef struct {
	Bookmark *v;
	size_t n;
} BookmarkList;			/* Sorted by name. */

enum OpKind { OP_NONE, OP_SET, OP_DELETE, OP_RENAME };

/* A change to the bookmarks. */
typedef struct {
	enum OpKind kind;
	Bookmark bm;		/* OP_SET: the bookmark. */
	char *name;		/* OP_DELETE, OP_RENAME: the bookmark. */
	char *new_name;		/* OP_RENAME. */
} Op;

static BookmarkList marks;
static bool state_enabled;
/* Changes made since the file was last saved, oldest first, while it
   cannot be; and why. */
static Op *pending;
static size_t npending;
static char *unsaved_why;

static History *bookmark_history;

/* Reports an error in the echo area, and in the debug log. */
static void PRINTF_LIKE (1, 2)
bookmark_error (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	char *msg = xvasprintf (fmt, ap);
	va_end (ap);
	minibuf_error ("%s", msg);
	debug_log ("bookmark", "%s", msg);
	free (msg);
}


/*--------------------------------------------------------------------------
 * Lists of bookmarks.
 *--------------------------------------------------------------------------*/

static void
bookmark_clear (Bookmark *b)
{
	free (b->name);
	free (b->path);
	b->name = b->path = NULL;
}

static Bookmark
bookmark_dup (const Bookmark *b)
{
	Bookmark c = *b;
	c.name = xstrdup (b->name);
	c.path = xstrdup (b->path);
	return c;
}

static void
list_clear (BookmarkList *l)
{
	for (size_t i = 0; i < l->n; i++)
		bookmark_clear (&l->v[i]);
	free (l->v);
	l->v = NULL;
	l->n = 0;
}

static long
list_find (const BookmarkList *l, const char *name)
{
	for (size_t i = 0; i < l->n; i++)
		if (strcmp (l->v[i].name, name) == 0)
			return (long) i;
	return -1;
}

/* Add `b', which the list then owns, in name order, replacing a
   bookmark of the same name; returns true if it replaced one. */
static bool
list_put (BookmarkList *l, Bookmark b)
{
	size_t i = 0;
	while (i < l->n && strcmp (l->v[i].name, b.name) < 0)
		i++;
	if (i < l->n && strcmp (l->v[i].name, b.name) == 0) {
		bookmark_clear (&l->v[i]);
		l->v[i] = b;
		return true;
	}
	l->v = xrealloc (l->v, (l->n + 1) * sizeof (Bookmark));
	memmove (l->v + i + 1, l->v + i, (l->n - i) * sizeof (Bookmark));
	l->v[i] = b;
	l->n++;
	return false;
}

/* Remove the `i'th bookmark, returning it to the caller to free. */
static Bookmark
list_take (BookmarkList *l, size_t i)
{
	Bookmark b = l->v[i];
	memmove (l->v + i, l->v + i + 1, (l->n - i - 1) * sizeof (Bookmark));
	l->n--;
	return b;
}

/* Why `name' cannot name a bookmark, or NULL if it can. */
static const char *
name_problem (const char *name)
{
	if (name[0] == '\0')
		return "the name is empty";
	if (strlen (name) > MAX_NAME)
		return "the name is too long";
	for (const unsigned char *p = (const unsigned char *) name; *p != '\0'; p++)
		if (*p < 0x20 || *p == 0x7f)
			return "the name contains a control character";
	return NULL;
}

static void
op_clear (Op *op)
{
	bookmark_clear (&op->bm);
	free (op->name);
	free (op->new_name);
}

/* Make `op' in `l'. Returns false, with `*err' set, if it cannot be
   made there; sets `*replaced' (if not NULL) if a bookmark was set
   over another of the same name. */
static bool
apply_op (BookmarkList *l, const Op *op, char **err, bool *replaced)
{
	long i;
	switch (op->kind) {
	case OP_NONE:
		return true;
	case OP_SET: {
		bool r = list_put (l, bookmark_dup (&op->bm));
		if (replaced != NULL)
			*replaced = r;
		return true;
	}
	case OP_DELETE:
		if ((i = list_find (l, op->name)) >= 0) {
			Bookmark b = list_take (l, (size_t) i);
			bookmark_clear (&b);
			return true;
		}
		break;
	case OP_RENAME:
		if ((i = list_find (l, op->name)) < 0)
			break;
		if (strcmp (op->name, op->new_name) == 0)
			return true;
		if (list_find (l, op->new_name) >= 0) {
			*err = xasprintf ("A bookmark named `%s' already exists", op->new_name);
			return false;
		}
		Bookmark b = list_take (l, (size_t) i);
		free (b.name);
		b.name = xstrdup (op->new_name);
		list_put (l, b);
		return true;
	}
	*err = xasprintf ("No bookmark named `%s'", op->name);
	return false;
}


/*--------------------------------------------------------------------------
 * The bookmarks file.
 *--------------------------------------------------------------------------*/

static void
escape_field (StrBuf *sb, const char *s)
{
	for (; *s != '\0'; s++)
		switch (*s) {
		case '\\': sb_add (sb, "\\\\"); break;
		case '\t': sb_add (sb, "\\t"); break;
		case '\n': sb_add (sb, "\\n"); break;
		case '\r': sb_add (sb, "\\r"); break;
		default: sb_addc (sb, *s);
		}
}

/* Decode the field [s, end); NULL if it is malformed. */
static char *
unescape_field (const char *s, const char *end)
{
	if (end - s > MAX_STATE_FIELD * 2)
		return NULL;
	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, "");
	for (; s < end; s++) {
		char c = *s;
		if (c == '\0' || c == '\r') {
			sb_free (&sb);
			return NULL;
		}
		if (c == '\\') {
			if (++s == end) {
				sb_free (&sb);
				return NULL;
			}
			c = *s == '\\' ? '\\' : *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : '\0';
			if (c == '\0') {
				sb_free (&sb);
				return NULL;
			}
		}
		sb_addc (&sb, c);
	}
	if (sb.len > MAX_STATE_FIELD) {
		sb_free (&sb);
		return NULL;
	}
	return sb_take (&sb);
}

static bool
parse_point (const char *s, size_t *pt)
{
	size_t n = strlen (s);
	if (n == 0 || n > MAX_POINT_DIGITS || strspn (s, "0123456789") != n)
		return false;
	unsigned long long v = strtoull (s, NULL, 10);
	if (v > SIZE_MAX)
		return false;
	*pt = (size_t) v;
	return true;
}

/* LOCATION as written in the file: `local', or `ssh:' and the label
   of a machine, which must be what remote_label gives for it, so that
   it reads back as the same machine. */
static bool
parse_location (const char *s, Remote **r)
{
	*r = NULL;
	if (strcmp (s, "local") == 0)
		return true;
	if (!str_has_prefix (s, "ssh:"))
		return false;
	*r = remote_parse (s + 4);
	return *r != NULL && strcmp (remote_label (*r), s + 4) == 0;
}

/* The record [line, end) added to `l'; false if it is malformed. The
   first of two records of the same name is kept. */
static bool
parse_record (BookmarkList *l, const char *line, const char *end)
{
	char *f[5];
	size_t n = 0;
	bool ok = true;
	for (const char *p = line; ok;) {
		const char *tab = memchr (p, '\t', (size_t) (end - p));
		const char *fe = tab != NULL ? tab : end;
		if (n == 5 || (f[n] = unescape_field (p, fe)) == NULL)
			ok = false;
		else
			n++;
		if (tab == NULL)
			break;
		p = tab + 1;
	}

	Bookmark b;
	ok = ok && n == 5 && (strcmp (f[0], "file") == 0 || strcmp (f[0], "dir") == 0) &&
		name_problem (f[1]) == NULL && parse_location (f[2], &b.remote) && f[3][0] == '/' &&
		parse_point (f[4], &b.pt);
	if (ok && list_find (l, f[1]) < 0) {
		b.dir = strcmp (f[0], "dir") == 0;
		b.name = f[1];
		b.path = f[3];
		f[1] = f[3] = NULL;
		list_put (l, b);
	}
	for (size_t i = 0; i < n; i++)
		free (f[i]);
	return ok;
}

static char *
state_dir (void)
{
	char *home = get_home_dir ();
	char *dir = path_join (home, STATE_DIR);
	free (home);
	return dir;
}

/* The state directory, created if need be; NULL, with `*why' set,
   if it cannot be, or is not private: a symbolic link, someone
   else's, or writable by others. */
static char *
private_state_dir (char **why)
{
	char *dir = state_dir ();
	struct stat st;
	if (mkdir (dir, 0700) != 0 && errno != EEXIST)
		*why = xasprintf ("cannot create ~/" STATE_DIR ": %s", strerror (errno));
	else if (lstat (dir, &st) != 0 || !S_ISDIR (st.st_mode) || st.st_uid != geteuid () ||
		 (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
		*why = xstrdup ("~/" STATE_DIR " is not a private directory");
	else
		return dir;
	free (dir);
	return NULL;
}

/* Lock the bookmarks in `dir' for writing, waiting at most
   STATE_LOCK_WAIT ms. The lock is on a file of its own, as the
   bookmarks file is replaced, never written in place. Returns the
   descriptor to close to unlock, or -1 with `*why' set. */
static int
lock_state (const char *dir, char **why)
{
	char *path = path_join (dir, STATE_FILE ".lock");
	int fd = open (path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
	free (path);
	if (fd < 0) {
		*why = xasprintf ("cannot open " STATE_PATH ".lock: %s", strerror (errno));
		return -1;
	}
	struct flock fl;
	memset (&fl, 0, sizeof fl);
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	int64_t until = monotonic_ms () + STATE_LOCK_WAIT;
	while (fcntl (fd, F_SETLK, &fl) != 0) {
		int e = errno;
		bool busy = e == EACCES || e == EAGAIN || e == EINTR;
		if (!busy || monotonic_ms () >= until) {
			*why = busy ? xstrdup ("another session kept " STATE_PATH " locked")
				: xasprintf ("cannot lock " STATE_PATH ".lock: %s", strerror (e));
			close (fd);
			return -1;
		}
		struct timespec ts = { 0, 20L * 1000 * 1000 };
		nanosleep (&ts, NULL);
	}
	return fd;
}

/* Read the bookmarks file into `l' (empty if there is none), with
   `*text' its contents. Returns false, with `*why' set, if the file
   is there but must not be replaced: not accessible, not a regular
   file, too large, unreadable, or of another version. */
static bool
read_state (BookmarkList *l, char **text, char **why)
{
	*text = NULL;
	char *dir = state_dir ();
	char *file = path_join (dir, STATE_FILE);
	free (dir);
	const char *reason = NULL;
	struct stat st;
	char *s = NULL;
	size_t len = 0;
	if (lstat (file, &st) != 0) {
		if (errno != ENOENT)
			reason = "not accessible";
	} else if (!S_ISREG (st.st_mode))
		reason = "not a regular file";
	else if (st.st_size > (off_t) MAX_STATE_SIZE)
		reason = "too large";
	else if (!read_file (file, &s, &len))
		reason = "unreadable";
	else if (len > 0) {
		const char *end = s + len, *nl = memchr (s, '\n', len);
		size_t hlen = nl != NULL ? (size_t) (nl - s) : len;
		if (hlen != strlen (STATE_HEADER) || memcmp (s, STATE_HEADER, hlen) != 0)
			reason = "of another version of the format";
		else {
			size_t lineno = 1;
			for (const char *p = nl != NULL ? nl + 1 : end; p < end; lineno++) {
				const char *e = memchr (p, '\n', (size_t) (end - p));
				if (e == NULL)
					e = end;
				if (e > p && !parse_record (l, p, e))
					debug_log ("bookmark", "ignored line %zu of %s", lineno + 1, file);
				p = e + 1;
			}
		}
	}
	if (reason != NULL) {
		debug_log ("bookmark", "not replacing %s: it is %s", file, reason);
		free (*why);
		*why = xasprintf (STATE_PATH " is %s", reason);
	}
	*text = s;
	free (file);
	return reason == NULL;
}

static char *
state_text (const BookmarkList *l)
{
	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, STATE_HEADER "\n");
	for (size_t i = 0; i < l->n; i++) {
		const Bookmark *b = &l->v[i];
		sb_add (&sb, b->dir ? "dir\t" : "file\t");
		escape_field (&sb, b->name);
		sb_add (&sb, b->remote != NULL ? "\tssh:" : "\tlocal");
		if (b->remote != NULL)
			escape_field (&sb, remote_label (b->remote));
		sb_addc (&sb, '\t');
		escape_field (&sb, b->path);
		sb_addf (&sb, "\t%zu\n", b->pt);
	}
	return sb_take (&sb);
}

/* Replace the bookmarks file in `dir' with `text', through a private
   temporary file renamed over it; false, with `*why' set, if that
   fails. */
static bool
write_state (const char *dir, const char *text, char **why)
{
	char *file = path_join (dir, STATE_FILE);
	char *tmp = path_join (dir, STATE_FILE ".XXXXXX");
	bool ok = false;
	int fd = mkstemp (tmp);
	if (fd >= 0) {
		size_t len = strlen (text), done = 0;
		bool wrote = fchmod (fd, S_IRUSR | S_IWUSR) == 0;
		while (wrote && done < len) {
			ssize_t w = write (fd, text + done, len - done);
			if (w < 0 && errno == EINTR)
				continue;
			wrote = w > 0;
			if (wrote)
				done += (size_t) w;
		}
		wrote = wrote && fsync (fd) == 0;
		if (close (fd) == 0 && wrote && rename (tmp, file) == 0)
			ok = true;
		else {
			int e = errno;
			unlink (tmp);
			errno = e;
		}
	}
	if (!ok)
		*why = xasprintf ("cannot write " STATE_PATH ": %s", strerror (errno));
	free (tmp);
	free (file);
	return ok;
}

/* Make the changes not yet saved in `l', which holds what the file
   does. A change that no longer applies there (such as deleting what
   another session deleted already) is dropped. */
static void
replay_pending (BookmarkList *l)
{
	for (size_t i = 0; i < npending; i++) {
		char *err = NULL;
		if (!apply_op (l, &pending[i], &err, NULL))
			debug_log ("bookmark", "unsaved change dropped: %s", err);
		free (err);
	}
}

static void
pending_clear (void)
{
	for (size_t i = 0; i < npending; i++)
		op_clear (&pending[i]);
	free (pending);
	pending = NULL;
	npending = 0;
	free (unsaved_why);
	unsaved_why = NULL;
}

static void
pending_push (const Op *op, const char *why)
{
	pending = xrealloc (pending, (npending + 1) * sizeof (Op));
	Op *p = &pending[npending++];
	p->kind = op->kind;
	p->bm = op->kind == OP_SET ? bookmark_dup (&op->bm) : (Bookmark) { NULL, false, NULL, NULL, 0 };
	p->name = xstrdup_or_null (op->name);
	p->new_name = xstrdup_or_null (op->new_name);
	free (unsaved_why);
	unsaved_why = xstrdup (why);
}

/* Make `op'. If the bookmarks are kept across sessions, this is done
   under the lock, in what the file holds, which is then saved and
   becomes the bookmarks of this session. Returns false, with `*err'
   set, if `op' cannot be made. If it was made but could not be saved,
   `*unsaved' says why; it is then kept to save with the next change. */
static bool
commit (const Op *op, char **err, char **unsaved, bool *replaced)
{
	*err = *unsaved = NULL;
	if (replaced != NULL)
		*replaced = false;
	if (state_enabled) {
		char *why = NULL;
		char *dir = private_state_dir (&why);
		int lock = dir != NULL ? lock_state (dir, &why) : -1;
		bool done = false, refused = false;
		if (lock >= 0) {
			BookmarkList cur = { NULL, 0 };
			char *old;
			if (read_state (&cur, &old, &why)) {
				replay_pending (&cur);
				if (!apply_op (&cur, op, err, replaced))
					refused = true;
				else {
					char *text = state_text (&cur);
					bool same = old != NULL && strcmp (old, text) == 0;
					if (same || write_state (dir, text, &why)) {
						list_clear (&marks);
						marks = cur;
						cur.v = NULL;
						cur.n = 0;
						pending_clear ();
						done = true;
					}
					free (text);
				}
			}
			free (old);
			list_clear (&cur);
			close (lock);
		}
		free (dir);
		if (done || refused) {
			free (why);
			return done;
		}
		*unsaved = why;
		debug_log ("bookmark", "not saved: %s", why);
	}
	if (!apply_op (&marks, op, err, replaced)) {
		free (*unsaved);
		*unsaved = NULL;
		return false;
	}
	if (*unsaved != NULL && op->kind != OP_NONE)
		pending_push (op, *unsaved);
	return true;
}

/* Load the bookmarks, and keep them across sessions, as `-q' does not. */
void
bookmarks_load (void)
{
	state_enabled = true;
	BookmarkList l = { NULL, 0 };
	char *text, *why = NULL;
	if (read_state (&l, &text, &why)) {
		list_clear (&marks);
		marks = l;
	} else {
		list_clear (&l);
		minibuf_write ("Warning: bookmarks not loaded: %s", why);
		debug_log ("bookmark", "Warning: bookmarks not loaded: %s", why);
		free (why);
	}
	free (text);
	debug_log ("bookmark", "%zu bookmarks loaded", marks.n);
}


/*--------------------------------------------------------------------------
 * Places.
 *--------------------------------------------------------------------------*/

/* Where `b' is, for people: `~/dir/file', `ssh:host:~/dir/', with
   control characters escaped. */
static char *
place_text (const Bookmark *b)
{
	char *shown = b->remote != NULL ? remote_compact_path (b->remote, b->path) : compact_path (b->path);
	StrBuf sb;
	sb_init (&sb);
	if (b->remote != NULL)
		sb_addf (&sb, "ssh:%s:", remote_label (b->remote));
	for (const char *p = shown; *p != '\0'; p++)
		switch (*p) {
		case '\t': sb_add (&sb, "\\t"); break;
		case '\n': sb_add (&sb, "\\n"); break;
		case '\r': sb_add (&sb, "\\r"); break;
		default:
			if ((unsigned char) *p < 0x20 || *p == 0x7f)
				sb_addf (&sb, "\\%03o", (unsigned char) *p);
			else
				sb_addc (&sb, *p);
		}
	if (b->dir && !str_has_suffix (shown, "/"))
		sb_addc (&sb, '/');
	free (shown);
	return sb_take (&sb);
}

/* The place `bp' shows, with point, in `*b' (without a name). Says
   why and returns false for buffers that are not of a file or
   directory, or that have no position to keep. */
static bool
buffer_place (Buffer *bp, Bookmark *b)
{
	memset (b, 0, sizeof *b);
	if (preview_showing (bp)) {
		bookmark_error ("Cannot bookmark `%s': image and PDF previews have no position to keep", bp->name);
		return false;
	}
	const char *dir = dired_directory (bp);
	const char *path = dir != NULL ? dir : bp->remote != NULL ? bp->remote_path : bp->filename;
	if (path == NULL || path[0] != '/') {
		bookmark_error ("Cannot bookmark `%s': it is not visiting a file or directory", bp->name);
		return false;
	}
	if (bp->remote != NULL && remote_parse (remote_label (bp->remote)) != bp->remote) {
		bookmark_error ("Cannot bookmark `%s': ssh:%s cannot be written in the bookmarks file",
			       bp->name, remote_label (bp->remote));
		return false;
	}
	b->dir = dir != NULL;
	b->remote = bp->remote;
	b->path = xstrdup (path);
	b->pt = buffer_pt (bp);
	return true;
}

/* `o', moved back into the text of `bp' and onto the start of a
   character. */
static size_t
clamp_point (Buffer *bp, size_t o)
{
	size_t len = buffer_length (bp);
	if (o >= len)
		return len;
	size_t eol = buffer_end_of_line (bp, buffer_start_of_line (bp, o));
	if (o > eol)
		return eol;
	while (o > 0 && ((unsigned char) buffer_get_char (bp, o) & 0xc0) == 0x80)
		o--;
	return o;
}

static bool
local_file_known (const char *path)
{
	if (file_exists (path))
		return true;
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->filename != NULL && bp->remote == NULL && strcmp (bp->filename, path) == 0)
			return true;
	return false;
}

static bool
jump_to (const char *name)
{
	long i = list_find (&marks, name);
	if (i < 0) {
		bookmark_error ("No bookmark named `%s'", name);
		return false;
	}
	Bookmark b = bookmark_dup (&marks.v[i]);
	bool ok;
	if (b.remote != NULL)
		ok = b.dir ? dired_remote (b.remote, b.path) : find_file_remote (b.remote, b.path);
	else if (b.dir)
		ok = dired (b.path);
	else if (!local_file_known (b.path)) {
		char *shown = place_text (&b);
		bookmark_error ("Bookmark `%s': %s no longer exists", name, shown);
		free (shown);
		ok = false;
	} else
		ok = find_file (b.path);

	if (ok) {
		size_t o = clamp_point (cur_bp, b.pt);
		buffer_goto_offset (cur_bp, o);
		cur_bp->mark_active = false;
		thisflag |= FLAG_NEED_RESYNC;
		debug_log ("bookmark", "jumped to `%s' at %zu (saved at %zu)", name, o, b.pt);
		if (b.pt > buffer_length (cur_bp))
			minibuf_write ("Bookmark `%s' was past the end of `%s': moved to the end", name, cur_bp->name);
	}
	bookmark_clear (&b);
	return ok;
}


/*--------------------------------------------------------------------------
 * The bookmark list.
 *--------------------------------------------------------------------------*/

/* What each row of the list buffer shows, as last written: rows are
   only acted on while their text is still that. */
typedef struct {
	char *name;
	char *line;		/* The line, after the flag column. */
	size_t loc_col;		/* Where the location starts, in bytes. */
} Row;

static Row *rows;
static size_t nrows;
static Binding *bmenu_map;

static void
rows_clear (void)
{
	for (size_t i = 0; i < nrows; i++) {
		free (rows[i].name);
		free (rows[i].line);
	}
	free (rows);
	rows = NULL;
	nrows = 0;
}

static Buffer *
list_buffer (void)
{
	Buffer *bp = buffer_find (LIST_NAME);
	return bp != NULL && bmenu_map != NULL && bp->keymap == bmenu_map ? bp : NULL;
}

static char *
line_text (Buffer *bp, size_t lo)
{
	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, "");
	for (size_t o = lo, end = buffer_end_of_line (bp, lo); o < end; o++)
		sb_addc (&sb, buffer_get_char (bp, o));
	return sb_take (&sb);
}

/* The offset of line `n' of `bp', or NOPOS. */
static size_t
line_offset (Buffer *bp, size_t n)
{
	size_t o = 0;
	while (n-- > 0 && o != NOPOS)
		o = buffer_next_line (bp, o);
	return o;
}

/* The row at offset `o' of the list buffer `bp', or -1. */
static long
row_at (Buffer *bp, size_t o)
{
	size_t line = buffer_offset_to_line (bp, o);
	if (line < LIST_HEADER_LINES || line - LIST_HEADER_LINES >= nrows)
		return -1;
	size_t i = line - LIST_HEADER_LINES;
	char *t = line_text (bp, buffer_start_of_line (bp, o));
	bool same = t[0] != '\0' && strcmp (t + 1, rows[i].line) == 0;
	free (t);
	return same ? (long) i : -1;
}

static char
row_flag (Buffer *bp, size_t i)
{
	size_t lo = line_offset (bp, LIST_HEADER_LINES + i);
	return lo != NOPOS && buffer_end_of_line (bp, lo) > lo ? buffer_get_char (bp, lo) : ' ';
}

static void
set_row_flag (Buffer *bp, size_t i, char c)
{
	size_t lo = line_offset (bp, LIST_HEADER_LINES + i);
	if (lo == NOPOS || row_flag (bp, i) == c)
		return;
	size_t pt = buffer_pt (bp);
	bool readonly = bp->readonly;
	bp->readonly = false;
	buffer_goto_offset (bp, lo);
	buffer_replace_estr (bp, 1, ev_of (&c, 1, eol_lf));
	buffer_goto_offset (bp, pt);
	bp->readonly = readonly;
	bp->modified = false;
}

static void
goto_row (Buffer *bp, size_t i)
{
	size_t lo = line_offset (bp, LIST_HEADER_LINES + i);
	if (lo != NOPOS)
		buffer_goto_offset (bp, MIN (lo + NAME_COL, buffer_end_of_line (bp, lo)));
}

static size_t
display_width (const char *s)
{
	size_t len = strlen (s), w = 0;
	for (size_t i = 0; i < len;) {
		size_t n;
		uint32_t c = utf8_decode (s, len, i, &n);
		w += (size_t) utf8_char_display_width (c);
		i += n > 0 ? n : 1;
	}
	return w;
}

static char *
status_text (void)
{
	if (!state_enabled)
		return xasprintf ("Bookmarks: %zu, kept for this session only (-q)", marks.n);
	if (unsaved_why != NULL)
		return xasprintf ("Bookmarks: %zu, changes not saved: %s (M-x bookmark-save retries)", marks.n, unsaved_why);
	return xasprintf ("Bookmarks: %zu, saved in " STATE_PATH, marks.n);
}

/* Write the list in `bp' again, keeping the flags of bookmarks still
   there and point on the same bookmark (or else line). */
static void
list_fill (Buffer *bp)
{
	StrVec flagged = { NULL, 0, 0 };
	for (size_t i = 0; i < nrows; i++)
		if (row_flag (bp, i) == 'D' && row_at (bp, line_offset (bp, LIST_HEADER_LINES + i)) == (long) i)
			sv_add_sorted (&flagged, rows[i].name);
	long at = row_at (bp, buffer_pt (bp));
	char *at_name = at >= 0 ? xstrdup (rows[at].name) : NULL;
	size_t at_line = buffer_offset_to_line (bp, buffer_pt (bp));
	rows_clear ();

	size_t width = strlen ("Bookmark");
	for (size_t i = 0; i < marks.n; i++)
		width = MAX (width, display_width (marks.v[i].name));
	width = MIN (width, MAX_NAME_WIDTH);

	StrBuf sb;
	sb_init (&sb);
	char *status = status_text ();
	sb_addf (&sb, "%s\n", status);
	free (status);
	sb_add (&sb, "RET: jump  n/p: move  d: flag  u: unflag  x: delete flagged  r: rename  g: reread  q: quit\n");
	sb_addf (&sb, "  %-*s  Location\n", (int) width, "Bookmark");
	if (marks.n == 0)
		sb_add (&sb, "  (none: C-x r m in a file or Dired buffer sets one)\n");
	rows = xmalloc ((marks.n > 0 ? marks.n : 1) * sizeof (Row));
	for (size_t i = 0; i < marks.n; i++) {
		const Bookmark *b = &marks.v[i];
		StrBuf line;
		sb_init (&line);
		sb_addf (&line, " %s", b->name);
		for (size_t w = display_width (b->name); w < width; w++)
			sb_addc (&line, ' ');
		sb_add (&line, "  ");
		Row *r = &rows[nrows++];
		r->loc_col = line.len + 1;
		char *place = place_text (b);
		sb_add (&line, place);
		free (place);
		r->name = xstrdup (b->name);
		r->line = sb_take (&line);
		sb_addf (&sb, " %s\n", r->line);
	}

	bool readonly = bp->readonly;
	bp->readonly = false;
	buffer_goto_offset (bp, 0);
	buffer_replace_estr (bp, buffer_length (bp), ev_cstr (sb.s));
	sb_free (&sb);
	for (size_t i = 0; i < nrows; i++)
		if (sv_contains (&flagged, rows[i].name))
			set_row_flag (bp, i, 'D');
	sv_clear (&flagged);
	bp->readonly = readonly;
	bp->modified = false;

	long found = -1;
	for (size_t i = 0; at_name != NULL && i < nrows && found < 0; i++)
		if (strcmp (rows[i].name, at_name) == 0)
			found = (long) i;
	free (at_name);
	if (found >= 0)
		goto_row (bp, (size_t) found);
	else if (nrows > 0)
		goto_row (bp, at_line < LIST_HEADER_LINES ? 0 : MIN (at_line - LIST_HEADER_LINES, nrows - 1));
	else
		buffer_goto_offset (bp, 0);
	thisflag |= FLAG_NEED_RESYNC;
}

/* Show the bookmarks as they now are in the list, if it is open. */
static void
list_update (void)
{
	Buffer *bp = list_buffer ();
	if (bp != NULL)
		list_fill (bp);
}

static int
bmenu_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		 size_t len, int start_state, const char **face_names)
{
	(void) mode;
	(void) start_state;
	for (size_t i = 0; i < len; i++)
		face_names[i] = NULL;
	if (line_idx < LIST_HEADER_LINES) {
		const char *face = line_idx == 0 ? FACE_FONT_LOCK_TYPE :
			line_idx == 1 ? FACE_FONT_LOCK_COMMENT : FACE_FONT_LOCK_KEYWORD;
		for (size_t i = 0; i < len; i++)
			face_names[i] = face;
		return SYNTAX_STATE_NORMAL;
	}
	size_t r = line_idx - LIST_HEADER_LINES;
	if (r >= nrows || len <= NAME_COL)
		return SYNTAX_STATE_NORMAL;
	bool flagged = buffer_get_char (bp, start_o) == 'D';
	if (flagged)
		face_names[0] = FACE_ERROR;
	size_t name_end = MIN (len, NAME_COL + strlen (rows[r].name));
	for (size_t i = NAME_COL; i < name_end; i++)
		face_names[i] = flagged ? FACE_ERROR : FACE_FONT_LOCK_FUNCTION_NAME;
	for (size_t i = rows[r].loc_col; i < len; i++)
		face_names[i] = FACE_FONT_LOCK_STRING;
	return SYNTAX_STATE_NORMAL;
}

static const Mode bmenu_mode = { "Bookmark Menu", bmenu_scan_line, NULL, NULL, 0 };

static void
init_bmenu_map (void)
{
	if (bmenu_map != NULL)
		return;
	static const struct {
		const char *keys;
		const char *func;
	} bindings[] = {
		{ "n", "bookmark-bmenu-next-line" },
		{ " ", "bookmark-bmenu-next-line" },
		{ "\\C-n", "bookmark-bmenu-next-line" },
		{ "\\DOWN", "bookmark-bmenu-next-line" },
		{ "p", "bookmark-bmenu-previous-line" },
		{ "\\C-p", "bookmark-bmenu-previous-line" },
		{ "\\UP", "bookmark-bmenu-previous-line" },
		{ "\\RET", "bookmark-bmenu-this-window" },
		{ "f", "bookmark-bmenu-this-window" },
		{ "j", "bookmark-bmenu-this-window" },
		{ "d", "bookmark-bmenu-delete" },
		{ "k", "bookmark-bmenu-delete" },
		{ "\\C-d", "bookmark-bmenu-delete" },
		{ "u", "bookmark-bmenu-unmark" },
		{ "\\BACKSPACE", "bookmark-bmenu-backup-unmark" },
		{ "x", "bookmark-bmenu-execute-deletions" },
		{ "r", "bookmark-bmenu-rename" },
		{ "g", "bookmark-bmenu-revert" },
		{ "q", "quit-window" },
	};
	bmenu_map = keymap_new ();
	for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++)
		keymap_bind (bmenu_map, bindings[i].keys, bindings[i].func);
}

static bool
check_bmenu (void)
{
	if (bmenu_map == NULL || cur_bp->keymap != bmenu_map) {
		bookmark_error ("Not in the bookmark list");
		return false;
	}
	return true;
}

/* The row at point, or -1 after saying there is none. */
static long
current_row (void)
{
	long r = row_at (cur_bp, buffer_pt (cur_bp));
	if (r < 0)
		bookmark_error (nrows == 0 ? "No bookmarks" : "No bookmark on this line");
	return r;
}


/*--------------------------------------------------------------------------
 * Commands.
 *--------------------------------------------------------------------------*/

/* Say what became of a change, and show the list as it now is. */
static bool
report (bool ok, char *err, char *unsaved, const char *done)
{
	if (!ok) {
		bookmark_error ("%s", err);
	} else if (unsaved != NULL) {
		bookmark_error ("%s, but not saved: %s", done, unsaved);
	} else {
		minibuf_write ("%s", done);
		debug_log ("bookmark", "%s", done);
	}
	free (err);
	free (unsaved);
	if (ok)
		list_update ();
	return ok;
}

static bool
check_name (const char *name)
{
	const char *why = name_problem (name);
	if (why != NULL) {
		bookmark_error ("Invalid bookmark name: %s", why);
	}
	return why == NULL;
}

/* Read the name of a bookmark, choosing from those there are; NULL if
   cancelled or there are none. */
static char *
read_bookmark (const char *prompt)
{
	if (marks.n == 0) {
		bookmark_error ("No bookmarks: C-x r m sets one");
		return NULL;
	}
	StrVec names = { NULL, 0, 0 }, places = { NULL, 0, 0 };
	for (size_t i = 0; i < marks.n; i++) {
		sv_push (&names, marks.v[i].name);
		char *place = place_text (&marks.v[i]);
		sv_push (&places, place);
		free (place);
	}
	char *name = picker_read_name (prompt, &names, &places, bookmark_history, true, -1);
	sv_clear (&names);
	sv_clear (&places);
	if (name != NULL)
		history_add_element (bookmark_history, name);
	return name;
}

static bool
set_bookmark (Bookmark *b)
{
	Op op = { OP_SET, *b, NULL, NULL };
	char *err, *unsaved;
	bool replaced;
	bool ok = commit (&op, &err, &unsaved, &replaced);
	char *done = xasprintf ("Bookmark `%s' %s", b->name, replaced ? "replaced" : "set");
	ok = report (ok, err, unsaved, done);
	free (done);
	return ok;
}

static bool
F_bookmark_set (long uniarg, Args *args)
{
	(void) uniarg;
	Bookmark b;
	char *name = xstrdup_or_null (args_poll (args));
	if (!buffer_place (cur_bp, &b)) {
		free (name);
		return false;
	}
	if (name == NULL) {
		char *def = path_basename (b.path);
		name = minibuf_read ("Set bookmark (default %s): ", "", def);
		if (name != NULL && name[0] == '\0') {
			free (name);
			name = def;
		} else
			free (def);
		if (name == NULL) {
			bookmark_clear (&b);
			return funcall ("keyboard-quit");
		}
	}
	bool ok = check_name (name);
	b.name = name;
	if (ok)
		ok = set_bookmark (&b);
	bookmark_clear (&b);
	return ok;
}

static bool
F_bookmark_jump (long uniarg, Args *args)
{
	(void) uniarg;
	char *name = xstrdup_or_null (args_poll (args));
	if (name == NULL && (name = read_bookmark ("Jump to bookmark: ")) == NULL)
		return false;
	bool ok = jump_to (name);
	free (name);
	return ok;
}

static bool
delete_bookmark (const char *name, char **err, char **unsaved)
{
	Op op = { OP_DELETE, { NULL, false, NULL, NULL, 0 }, (char *) name, NULL };
	return commit (&op, err, unsaved, NULL);
}

static bool
F_bookmark_delete (long uniarg, Args *args)
{
	(void) uniarg;
	char *name = xstrdup_or_null (args_poll (args));
	if (name == NULL && (name = read_bookmark ("Delete bookmark: ")) == NULL)
		return false;
	char *err, *unsaved;
	bool ok = delete_bookmark (name, &err, &unsaved);
	char *done = xasprintf ("Deleted bookmark `%s'", name);
	ok = report (ok, err, unsaved, done);
	free (done);
	free (name);
	return ok;
}

/* Rename `old' to `new_name', or to a name read if that is NULL;
   returns the new name, or NULL if it was not renamed. */
static char *
rename_bookmark (const char *old, const char *new_name)
{
	char *name = new_name != NULL ? xstrdup (new_name) : minibuf_read ("Rename bookmark `%s' to: ", "", old);
	if (name == NULL) {
		funcall ("keyboard-quit");
		return NULL;
	}
	bool ok = check_name (name);
	if (ok) {
		Op op = { OP_RENAME, { NULL, false, NULL, NULL, 0 }, (char *) old, name };
		char *err, *unsaved;
		ok = commit (&op, &err, &unsaved, NULL);
		char *done = xasprintf ("Renamed bookmark `%s' to `%s'", old, name);
		ok = report (ok, err, unsaved, done);
		free (done);
	}
	if (!ok) {
		free (name);
		name = NULL;
	}
	return name;
}

static bool
F_bookmark_rename (long uniarg, Args *args)
{
	(void) uniarg;
	char *old = xstrdup_or_null (args_poll (args));
	if (old == NULL && (old = read_bookmark ("Rename bookmark: ")) == NULL)
		return false;
	char *name = rename_bookmark (old, args_poll (args));
	bool ok = name != NULL;
	free (old);
	free (name);
	return ok;
}

static bool
F_bookmark_save (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!state_enabled) {
		bookmark_error ("Bookmarks are not saved across sessions with -q (--no-init-file)");
		return false;
	}
	Op op = { OP_NONE, { NULL, false, NULL, NULL, 0 }, NULL, NULL };
	char *err, *unsaved;
	bool ok = commit (&op, &err, &unsaved, NULL);
	if (unsaved != NULL) {
		char *msg = xasprintf ("Bookmarks not saved: %s", unsaved);
		free (unsaved);
		free (err);
		report (false, msg, NULL, NULL);
		list_update ();
		return false;
	}
	char *done = xasprintf ("Saved %zu bookmark%s in " STATE_PATH, marks.n, marks.n == 1 ? "" : "s");
	ok = report (ok, err, NULL, done);
	free (done);
	return ok;
}

static bool
F_bookmark_bmenu_list (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	init_bmenu_map ();
	Buffer *bp = list_buffer ();
	bool fresh = bp == NULL;
	if (fresh) {
		if (buffer_find (LIST_NAME) != NULL) {
			bookmark_error ("Cannot list bookmarks: a buffer named `%s' already exists", LIST_NAME);
			return false;
		}
		bp = buffer_new (NULL);
		bp->name = xstrdup (LIST_NAME);
		bp->needname = true;
		bp->nosave = true;
		bp->noundo = true;
		bp->readonly = true;
		bp->keymap = bmenu_map;
		buffer_set_highlighter (bp, &bmenu_mode);
	}
	list_fill (bp);
	buffer_switch_to (bp, true);
	if (fresh && nrows > 0)
		goto_row (bp, 0);
	return true;
}

static bool
F_bookmark_bmenu_next_line (long uniarg, Args *args)
{
	(void) args;
	if (!check_bmenu ())
		return false;
	if (nrows == 0)
		return true;
	long line = (long) buffer_offset_to_line (cur_bp, buffer_pt (cur_bp)) - LIST_HEADER_LINES;
	long target = line < 0 && uniarg > 0 ? uniarg - 1 : line + uniarg;
	goto_row (cur_bp, (size_t) MAX (0, MIN (target, (long) nrows - 1)));
	return true;
}

static bool
F_bookmark_bmenu_previous_line (long uniarg, Args *args)
{
	return F_bookmark_bmenu_next_line (-uniarg, args);
}

/* Flag (or unflag) `n' rows from point, moving down over them. */
static bool
flag_rows (long n, char c)
{
	if (!check_bmenu ())
		return false;
	for (long k = 0; k < (n < 0 ? -n : n); k++) {
		long r = current_row ();
		if (r < 0)
			return false;
		set_row_flag (cur_bp, (size_t) r, c);
		if ((size_t) r + 1 < nrows)
			goto_row (cur_bp, (size_t) r + 1);
		else
			break;
	}
	return true;
}

static bool
F_bookmark_bmenu_delete (long uniarg, Args *args)
{
	(void) args;
	return flag_rows (uniarg, 'D');
}

static bool
F_bookmark_bmenu_unmark (long uniarg, Args *args)
{
	(void) args;
	return flag_rows (uniarg, ' ');
}

static bool
F_bookmark_bmenu_backup_unmark (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_bmenu ())
		return false;
	long r = row_at (cur_bp, buffer_pt (cur_bp));
	long line = (long) buffer_offset_to_line (cur_bp, buffer_pt (cur_bp)) - LIST_HEADER_LINES;
	long prev = r >= 0 ? r - 1 : MIN (line, (long) nrows) - 1;
	if (prev < 0)
		return true;
	goto_row (cur_bp, (size_t) prev);
	if (row_at (cur_bp, buffer_pt (cur_bp)) == prev)
		set_row_flag (cur_bp, (size_t) prev, ' ');
	return true;
}

static bool
F_bookmark_bmenu_execute_deletions (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_bmenu ())
		return false;
	StrVec names = { NULL, 0, 0 };
	for (size_t i = 0; i < nrows; i++)
		if (row_flag (cur_bp, i) == 'D' && row_at (cur_bp, line_offset (cur_bp, LIST_HEADER_LINES + i)) == (long) i)
			sv_push (&names, rows[i].name);
	if (names.n == 0) {
		minibuf_write ("No bookmarks flagged for deletion");
		return true;
	}
	size_t deleted = 0;
	char *last_err = NULL, *last_unsaved = NULL;
	for (size_t i = 0; i < names.n; i++) {
		char *err, *unsaved;
		if (delete_bookmark (names.v[i], &err, &unsaved))
			deleted++;
		if (err != NULL) {
			free (last_err);
			last_err = err;
		}
		if (unsaved != NULL) {
			free (last_unsaved);
			last_unsaved = unsaved;
		}
	}
	sv_clear (&names);
	char *done = xasprintf ("Deleted %zu bookmark%s", deleted, deleted == 1 ? "" : "s");
	bool ok = report (last_err == NULL, last_err, last_unsaved, done);
	free (done);
	if (!ok)
		list_update ();
	return ok;
}

static bool
F_bookmark_bmenu_rename (long uniarg, Args *args)
{
	(void) uniarg;
	if (!check_bmenu ())
		return false;
	long r = current_row ();
	if (r < 0)
		return false;
	char *old = xstrdup (rows[r].name);
	char *name = rename_bookmark (old, args_poll (args));
	bool ok = name != NULL;
	free (old);
	/* Stay on the bookmark, wherever its new name puts it. */
	for (size_t i = 0; name != NULL && i < nrows && cur_bp == list_buffer (); i++)
		if (strcmp (rows[i].name, name) == 0) {
			goto_row (cur_bp, i);
			break;
		}
	free (name);
	return ok;
}

static bool
F_bookmark_bmenu_revert (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_bmenu ())
		return false;
	bool ok = true;
	if (state_enabled) {
		BookmarkList l = { NULL, 0 };
		char *text, *why = NULL;
		if (read_state (&l, &text, &why)) {
			replay_pending (&l);
			list_clear (&marks);
			marks = l;
			minibuf_write ("Read " STATE_PATH);
		} else {
			list_clear (&l);
			bookmark_error ("Cannot reread bookmarks: %s", why);
			free (why);
			ok = false;
		}
		free (text);
	}
	list_fill (cur_bp);
	return ok;
}

static bool
F_bookmark_bmenu_this_window (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_bmenu ())
		return false;
	long r = current_row ();
	if (r < 0)
		return false;
	char *name = xstrdup (rows[r].name);
	bool ok = jump_to (name);
	free (name);
	return ok;
}

void
bookmark_init (void)
{
	bookmark_history = history_new ();

	lisp_func_new ("bookmark-set", F_bookmark_set, true,
		       "Set a bookmark named NAME at point in a file or Dired buffer.\n"
		       "The name read defaults to the name of the file or directory. A\n"
		       "bookmark of the same name is replaced. Unless the init file is not\n"
		       "loaded, bookmarks are saved in ~/.xzile.d/bookmarks at once.");
	lisp_func_new ("bookmark-jump", F_bookmark_jump, true,
		       "Jump to bookmark NAME: visit its file or directory, at its position.\n"
		       "A position past the end of the text goes to the end.");
	lisp_func_new ("bookmark-delete", F_bookmark_delete, true,
		       "Delete the bookmark named NAME.");
	lisp_func_new ("bookmark-rename", F_bookmark_rename, true,
		       "Rename the bookmark named OLD to NEW.");
	lisp_func_new ("bookmark-save", F_bookmark_save, true,
		       "Save the bookmarks in ~/.xzile.d/bookmarks.\n"
		       "Changes are saved as they are made; this saves those that could not\n"
		       "be, merged with what other sessions saved.");
	lisp_func_new ("bookmark-bmenu-list", F_bookmark_bmenu_list, true,
		       "Show a list of all bookmarks, in the buffer `*Bookmark List*'.\n"
		       "There, RET jumps to the bookmark on the line, `n' and `p' move, `d'\n"
		       "flags it for deletion, `u' unflags it and `x' deletes the flagged\n"
		       "bookmarks, `r' renames it, `g' rereads the bookmarks file and `q'\n"
		       "buries the list.");
	lisp_func_new ("list-bookmarks", F_bookmark_bmenu_list, true,
		       "Show a list of all bookmarks, as bookmark-bmenu-list does.");
	lisp_func_new ("bookmark-bmenu-next-line", F_bookmark_bmenu_next_line, true,
		       "In the bookmark list, move down lines to a bookmark.");
	lisp_func_new ("bookmark-bmenu-previous-line", F_bookmark_bmenu_previous_line, true,
		       "In the bookmark list, move up lines to a bookmark.");
	lisp_func_new ("bookmark-bmenu-this-window", F_bookmark_bmenu_this_window, true,
		       "In the bookmark list, jump to the bookmark on this line.");
	lisp_func_new ("bookmark-bmenu-delete", F_bookmark_bmenu_delete, true,
		       "In the bookmark list, flag the bookmark on this line for deletion.\n"
		       "With a prefix argument ARG, flag that many lines.");
	lisp_func_new ("bookmark-bmenu-unmark", F_bookmark_bmenu_unmark, true,
		       "In the bookmark list, remove the deletion flag of this line.\n"
		       "With a prefix argument ARG, unflag that many lines.");
	lisp_func_new ("bookmark-bmenu-backup-unmark", F_bookmark_bmenu_backup_unmark, true,
		       "In the bookmark list, move up a line and remove its deletion flag.");
	lisp_func_new ("bookmark-bmenu-execute-deletions", F_bookmark_bmenu_execute_deletions, true,
		       "In the bookmark list, delete the bookmarks flagged for deletion.");
	lisp_func_new ("bookmark-bmenu-rename", F_bookmark_bmenu_rename, true,
		       "In the bookmark list, rename the bookmark on this line.");
	lisp_func_new ("bookmark-bmenu-revert", F_bookmark_bmenu_revert, true,
		       "In the bookmark list, read the bookmarks file again and show it.");
}
