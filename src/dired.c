/* Directory editor

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

/* A Dired buffer lists a directory like `ls -l', after two header
   lines. Column 0 of each entry holds its mark: `*' for marked files
   and `D' for files flagged for deletion. All file names start at the
   same column, so the name of an entry is the rest of its line. */

#include "xzile.h"

#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define HEADER_LINES 2

struct Dired {
	char *dir;		/* The directory, without a trailing slash. */
	size_t name_col;	/* The column where file names start. */
	bool by_date;		/* Sort by modification time, not name. */
};

static Binding *dired_map;

void
dired_free (Buffer *bp)
{
	if (bp->dired != NULL) {
		free (bp->dired->dir);
		free (bp->dired);
		bp->dired = NULL;
	}
}


/*--------------------------------------------------------------------------
 * Listing.
 *--------------------------------------------------------------------------*/

typedef struct {
	char *name;
	struct stat st;
	char *link;		/* Symbolic link target, or NULL. */
} Entry;

static bool sort_by_date;

static int
compare_entries (const void *a, const void *b)
{
	const Entry *ea = a, *eb = b;
	/* `.' and `..' always come first. */
	bool da = strcmp (ea->name, ".") == 0 || strcmp (ea->name, "..") == 0;
	bool db = strcmp (eb->name, ".") == 0 || strcmp (eb->name, "..") == 0;
	if (da != db)
		return da ? -1 : 1;
	if (sort_by_date && !da && ea->st.st_mtime != eb->st.st_mtime)
		return ea->st.st_mtime > eb->st.st_mtime ? -1 : 1;
	return strcmp (ea->name, eb->name);
}

static void
mode_string (mode_t m, char *s)
{
	s[0] = S_ISDIR (m) ? 'd' : S_ISLNK (m) ? 'l' : S_ISCHR (m) ? 'c' :
		S_ISBLK (m) ? 'b' : S_ISFIFO (m) ? 'p' : S_ISSOCK (m) ? 's' : '-';
	s[1] = m & S_IRUSR ? 'r' : '-';
	s[2] = m & S_IWUSR ? 'w' : '-';
	s[3] = m & S_ISUID ? (m & S_IXUSR ? 's' : 'S') : (m & S_IXUSR ? 'x' : '-');
	s[4] = m & S_IRGRP ? 'r' : '-';
	s[5] = m & S_IWGRP ? 'w' : '-';
	s[6] = m & S_ISGID ? (m & S_IXGRP ? 's' : 'S') : (m & S_IXGRP ? 'x' : '-');
	s[7] = m & S_IROTH ? 'r' : '-';
	s[8] = m & S_IWOTH ? 'w' : '-';
	s[9] = m & S_ISVTX ? (m & S_IXOTH ? 't' : 'T') : (m & S_IXOTH ? 'x' : '-');
	s[10] = '\0';
}

/* Format a time like ls: the time of day for recent files, otherwise
   the year. Month names are not localized, to keep columns aligned. */
static void
format_time (time_t t, char *buf, size_t size)
{
	static const char *const months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
	};
	struct tm tm;
	time_t now = time (NULL);
	localtime_r (&t, &tm);
	const char *mon = tm.tm_mon >= 0 && tm.tm_mon < 12 ? months[tm.tm_mon] : "???";
	if (t <= now && now - t < 182 * 24 * 60 * 60)
		snprintf (buf, size, "%s %2d %02d:%02d", mon, tm.tm_mday, tm.tm_hour, tm.tm_min);
	else
		snprintf (buf, size, "%s %2d  %4d", mon, tm.tm_mday, tm.tm_year + 1900);
}

static void
format_size (unsigned long long n, char *buf, size_t size)
{
	static const char units[] = "KMGTP";
	if (n < 1024) {
		snprintf (buf, size, "%llu", n);
		return;
	}
	double v = (double) n;
	size_t u = 0;
	for (v /= 1024; v >= 1024 && u + 1 < sizeof units - 1; v /= 1024)
		u++;
	snprintf (buf, size, v < 10 ? "%.1f%c" : "%.0f%c", v, units[u]);
}

static char *
user_name (uid_t uid)
{
	struct passwd *pw = getpwuid (uid);
	return pw != NULL ? xstrdup (pw->pw_name) : xasprintf ("%lu", (unsigned long) uid);
}

static char *
group_name (gid_t gid)
{
	struct group *gr = getgrgid (gid);
	return gr != NULL ? xstrdup (gr->gr_name) : xasprintf ("%lu", (unsigned long) gid);
}

static size_t
num_width (unsigned long long n)
{
	size_t w = 1;
	while (n >= 10) {
		n /= 10;
		w++;
	}
	return w;
}

/* Produce the listing of `dir'; returns NULL with errno set if the
   directory cannot be read. Sets `*name_col'. */
static char *
make_listing (const char *dir, bool by_date, size_t *name_col)
{
	DIR *dp = opendir (dir);
	if (dp == NULL)
		return NULL;

	Entry *entries = NULL;
	size_t n = 0, cap = 0;
	struct dirent *d;
	while ((d = readdir (dp)) != NULL) {
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			entries = xrealloc (entries, cap * sizeof (Entry));
		}
		Entry *e = &entries[n];
		e->name = xstrdup (d->d_name);
		e->link = NULL;
		char *path = path_join (dir, d->d_name);
		if (lstat (path, &e->st) != 0) {
			free (path);
			free (e->name);
			continue;
		}
		if (S_ISLNK (e->st.st_mode)) {
			char buf[4096];
			ssize_t len = readlink (path, buf, sizeof buf - 1);
			if (len >= 0)
				e->link = xstrndup (buf, (size_t) len);
		}
		free (path);
		n++;
	}
	closedir (dp);

	sort_by_date = by_date;
	qsort (entries, n, sizeof (Entry), compare_entries);

	char **users = xmalloc ((n ? n : 1) * sizeof (char *));
	char **groups = xmalloc ((n ? n : 1) * sizeof (char *));
	size_t wl = 1, wu = 1, wg = 1, ws = 1;
	unsigned long long used = 0;
	for (size_t i = 0; i < n; i++) {
		users[i] = user_name (entries[i].st.st_uid);
		groups[i] = group_name (entries[i].st.st_gid);
		wl = MAX (wl, num_width ((unsigned long long) entries[i].st.st_nlink));
		wu = MAX (wu, strlen (users[i]));
		wg = MAX (wg, strlen (groups[i]));
		ws = MAX (ws, num_width ((unsigned long long) entries[i].st.st_size));
		used += (unsigned long long) entries[i].st.st_blocks * 512;
	}

	StrBuf sb;
	sb_init (&sb);
	char used_s[32], avail_s[32] = "?";
	format_size (used, used_s, sizeof used_s);
	struct statvfs vfs;
	if (statvfs (dir, &vfs) == 0)
		format_size ((unsigned long long) vfs.f_bavail * vfs.f_frsize, avail_s, sizeof avail_s);
	sb_addf (&sb, "  %s%s:\n", dir, strcmp (dir, "/") == 0 ? "" : "/");
	sb_addf (&sb, "  total used in directory %s available %s\n", used_s, avail_s);

	/* "  " mode " " nlink " " user " " group " " size " " date(12) " " */
	*name_col = 2 + 10 + 1 + wl + 1 + wu + 1 + wg + 1 + ws + 1 + 12 + 1;
	for (size_t i = 0; i < n; i++) {
		Entry *e = &entries[i];
		char mode[11], date[32];
		mode_string (e->st.st_mode, mode);
		format_time (e->st.st_mtime, date, sizeof date);
		sb_addf (&sb, "  %s %*llu %-*s %-*s %*llu %s %s",
			 mode, (int) wl, (unsigned long long) e->st.st_nlink,
			 (int) wu, users[i], (int) wg, groups[i],
			 (int) ws, (unsigned long long) e->st.st_size, date, e->name);
		if (e->link != NULL)
			sb_addf (&sb, " -> %s", e->link);
		sb_addc (&sb, '\n');
		free (e->name);
		free (e->link);
		free (users[i]);
		free (groups[i]);
	}
	free (entries);
	free (users);
	free (groups);
	return sb_take (&sb);
}

static int
compare_remote_entries (const void *a, const void *b)
{
	const RemoteEntry *ea = a, *eb = b;
	bool da = strcmp (ea->name, ".") == 0 || strcmp (ea->name, "..") == 0;
	bool db = strcmp (eb->name, ".") == 0 || strcmp (eb->name, "..") == 0;
	if (da != db)
		return da ? -1 : 1;
	return strcmp (ea->name, eb->name);
}

/* Produce the listing of `dir' on `r', in the same columns as
   make_listing; returns NULL with the remote error set. */
static char *
make_remote_listing (Remote *r, const char *dir, size_t *name_col)
{
	RemoteEntry *v;
	size_t n;
	if (!remote_list (r, dir, true, &v, &n))
		return NULL;
	qsort (v, n, sizeof (RemoteEntry), compare_remote_entries);

	size_t w[5] = { 10, 1, 1, 1, 1 };
	for (size_t i = 0; i < n; i++)
		for (size_t k = 1; k < 5; k++)
			if (v[i].info[k] != NULL)
				w[k] = MAX (w[k], strlen (v[i].info[k]));

	StrBuf sb;
	sb_init (&sb);
	sb_addf (&sb, "  ssh:%s:%s%s:\n", remote_label (r), dir, strcmp (dir, "/") == 0 ? "" : "/");
	sb_addf (&sb, "  remote directory: files can be opened, not renamed, copied or deleted here\n");
	*name_col = 2 + 10 + 1 + w[1] + 1 + w[2] + 1 + w[3] + 1 + w[4] + 1 + 12 + 1;
	for (size_t i = 0; i < n; i++) {
		RemoteEntry *e = &v[i];
		const char *mode = e->info[0] != NULL ? e->info[0] : e->dir ? "d?????????" : "-?????????";
		const char *f[5];
		for (size_t k = 1; k < 5; k++)
			f[k] = e->info[k] != NULL ? e->info[k] : "?";
		sb_addf (&sb, "  %-10.10s %*s %-*s %-*s %*s %-12.12s %s", mode,
			 (int) w[1], f[1], (int) w[2], f[2], (int) w[3], f[3], (int) w[4], f[4],
			 e->info[5] != NULL ? e->info[5] : "", e->name);
		if (e->link != NULL && mode[0] == 'l')
			sb_addf (&sb, " -> %s", e->link);
		sb_addc (&sb, '\n');
	}
	remote_entries_free (v, n);
	return sb_take (&sb);
}

/* The listing of a directory here or on `r'; NULL, having said why,
   if it cannot be read. */
static char *
dir_listing (Remote *r, const char *dir, bool by_date, size_t *name_col)
{
	char *text;
	if (r != NULL) {
		text = make_remote_listing (r, dir, name_col);
		if (text == NULL)
			minibuf_error ("%s", remote_last_error ());
	} else {
		text = make_listing (dir, by_date, name_col);
		if (text == NULL)
			minibuf_error ("Opening directory: %s: %s", dir, strerror (errno));
	}
	return text;
}

/*--------------------------------------------------------------------------
 * Reading and changing the buffer.
 *--------------------------------------------------------------------------*/

/* Return the name of the entry on the line starting at `lo', or NULL
   if it is not an entry. */
static char *
line_entry (Buffer *bp, size_t lo)
{
	size_t nc = bp->dired->name_col;
	size_t eol = buffer_end_of_line (bp, lo);
	if (eol - lo <= nc || buffer_offset_to_line (bp, lo) < HEADER_LINES)
		return NULL;

	StrBuf sb;
	sb_init (&sb);
	for (size_t o = lo + nc; o < eol; o++)
		sb_addc (&sb, buffer_get_char (bp, o));

	/* Remove a symbolic link's target. */
	if (buffer_get_char (bp, lo + 2) == 'l') {
		char *arrow = strstr (sb.s, " -> ");
		if (arrow != NULL)
			*arrow = '\0';
	}
	return sb_take (&sb);
}

static char
line_mark (Buffer *bp, size_t lo)
{
	return buffer_end_of_line (bp, lo) > lo ? buffer_get_char (bp, lo) : ' ';
}

static bool
is_dot_entry (const char *name)
{
	return strcmp (name, ".") == 0 || strcmp (name, "..") == 0;
}

static void
set_line_mark (Buffer *bp, size_t lo, char c)
{
	if (line_mark (bp, lo) == c)
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

/* Move point to the file name on the current line, if any. */
static void
goto_filename (Buffer *bp)
{
	size_t lo = buffer_line_o (bp);
	char *name = line_entry (bp, lo);
	buffer_goto_offset (bp, name != NULL ? lo + bp->dired->name_col : lo);
	free (name);
}

/* Move point to the entry named `name'; returns false if there is none. */
static bool
goto_entry (Buffer *bp, const char *name)
{
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (bp, lo)) {
		char *n = line_entry (bp, lo);
		bool found = n != NULL && strcmp (n, name) == 0;
		free (n);
		if (found) {
			buffer_goto_offset (bp, lo + bp->dired->name_col);
			return true;
		}
	}
	return false;
}

/* Re-read the directory, keeping marks and the entry at point. */
static bool
dired_refresh (Buffer *bp)
{
	Dired *d = bp->dired;
	size_t name_col;
	char *text = dir_listing (bp->remote, d->dir, d->by_date, &name_col);
	if (text == NULL)
		return false;

	Map *marks = map_new ();
	char *point_name = NULL;
	size_t point_line = buffer_offset_to_line (bp, buffer_pt (bp));
	if (buffer_length (bp) > 0) {
		point_name = line_entry (bp, buffer_line_o (bp));
		for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (bp, lo)) {
			char mark = line_mark (bp, lo);
			char *name = mark != ' ' ? line_entry (bp, lo) : NULL;
			if (name != NULL)
				map_set (marks, name, (void *) (intptr_t) mark);
			free (name);
		}
	}

	bool readonly = bp->readonly;
	bp->readonly = false;
	buffer_goto_offset (bp, 0);
	buffer_replace_estr (bp, buffer_length (bp), ev_cstr (text));
	free (text);
	d->name_col = name_col;

	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (bp, lo)) {
		char *name = line_entry (bp, lo);
		intptr_t mark = name != NULL ? (intptr_t) map_get (marks, name) : 0;
		if (mark != 0)
			set_line_mark (bp, lo, (char) mark);
		free (name);
	}
	map_free (marks, NULL);

	bp->readonly = readonly;
	bp->modified = false;

	if (point_name == NULL || !goto_entry (bp, point_name)) {
		buffer_goto_offset (bp, 0);
		buffer_move_line (bp, (long) point_line);
		goto_filename (bp);
	}
	free (point_name);
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}

static bool
check_dired (void)
{
	if (cur_bp->dired == NULL) {
		minibuf_error ("Buffer is not in Dired mode");
		return false;
	}
	return true;
}

/* Collect the names of entries with mark `mark'. */
static void
marked_entries (Buffer *bp, char mark, StrVec *out)
{
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (bp, lo))
		if (line_mark (bp, lo) == mark) {
			char *name = line_entry (bp, lo);
			if (name != NULL && !is_dot_entry (name))
				sv_push (out, name);
			free (name);
		}
}

/* The files to operate on: the marked ones, or else the current one. */
static bool
target_entries (Buffer *bp, StrVec *out)
{
	marked_entries (bp, '*', out);
	if (out->n > 0)
		return true;

	char *name = line_entry (bp, buffer_line_o (bp));
	if (name == NULL) {
		minibuf_error ("No file on this line");
		return false;
	}
	if (is_dot_entry (name)) {
		minibuf_error ("Cannot operate on `.' or `..'");
		free (name);
		return false;
	}
	sv_push (out, name);
	free (name);
	return true;
}

static char *
entry_path (Buffer *bp, const char *name)
{
	return path_join (bp->dired->dir, name);
}


/*--------------------------------------------------------------------------
 * File operations.
 *--------------------------------------------------------------------------*/

static bool
dir_is_empty (const char *path)
{
	DIR *dp = opendir (path);
	if (dp == NULL)
		return true;
	struct dirent *d;
	bool empty = true;
	while (empty && (d = readdir (dp)) != NULL)
		if (!is_dot_entry (d->d_name))
			empty = false;
	closedir (dp);
	return empty;
}

/* Delete `path', recursively for directories. */
static bool
delete_path (const char *path)
{
	struct stat st;
	if (lstat (path, &st) != 0)
		return false;
	if (!S_ISDIR (st.st_mode))
		return unlink (path) == 0;

	DIR *dp = opendir (path);
	if (dp == NULL)
		return false;
	bool ok = true;
	struct dirent *d;
	while (ok && (d = readdir (dp)) != NULL)
		if (!is_dot_entry (d->d_name)) {
			char *p = path_join (path, d->d_name);
			ok = delete_path (p);
			free (p);
		}
	int saved = errno;
	closedir (dp);
	errno = saved;
	return ok && rmdir (path) == 0;
}

/* Copy `from' to `to', recursively for directories. */
static bool
copy_path (const char *from, const char *to)
{
	struct stat st;
	if (lstat (from, &st) != 0)
		return false;

	if (S_ISLNK (st.st_mode)) {
		char buf[4096];
		ssize_t len = readlink (from, buf, sizeof buf - 1);
		if (len < 0)
			return false;
		buf[len] = '\0';
		return symlink (buf, to) == 0;
	}

	if (!S_ISDIR (st.st_mode)) {
		if (!S_ISREG (st.st_mode)) {
			errno = EINVAL;
			return false;
		}
		return file_copy (from, to);
	}

	if (mkdir (to, (st.st_mode & 07777) | S_IRWXU) != 0 && errno != EEXIST)
		return false;
	DIR *dp = opendir (from);
	if (dp == NULL)
		return false;
	bool ok = true;
	struct dirent *d;
	while (ok && (d = readdir (dp)) != NULL)
		if (!is_dot_entry (d->d_name)) {
			char *f = path_join (from, d->d_name);
			char *t = path_join (to, d->d_name);
			ok = copy_path (f, t);
			free (f);
			free (t);
		}
	int saved = errno;
	closedir (dp);
	if (ok)
		chmod (to, st.st_mode & 07777);
	errno = saved;
	return ok;
}

static bool
move_path (const char *from, const char *to)
{
	if (rename (from, to) == 0)
		return true;
	if (errno != EXDEV)
		return false;
	/* Across file systems: copy, then delete. */
	return copy_path (from, to) && delete_path (from);
}

/* Is `path' `dir' or inside it? */
static bool
path_within (const char *path, const char *dir)
{
	size_t n = strlen (dir);
	return strncmp (path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* After moving `from' to `to', make buffers visit the new names. */
static void
update_visiting_buffers (const char *from, const char *to)
{
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->filename != NULL && path_within (bp->filename, from)) {
			char *name = xstrcat3 (to, bp->filename + strlen (from), "");
			char *old_name = bp->name;
			bp->name = NULL;
			buffer_set_names (bp, name);
			free (old_name);
			free (bp->dir);
			bp->dir = path_dirname (bp->filename);
			free (name);
		}
}

enum Operation { OP_COPY, OP_RENAME };

static bool
do_copy_or_rename (enum Operation op)
{
	if (!check_dired () || remote_refuse (cur_bp, op == OP_COPY ? "dired-do-copy" : "dired-do-rename"))
		return false;

	StrVec names = { NULL, 0, 0 };
	if (!target_entries (cur_bp, &names))
		return false;

	const char *verb = op == OP_COPY ? "Copy" : "Rename";
	char *dest;
	if (names.n == 1)
		dest = minibuf_read_filename ("%s %s to: ", cur_bp->dired->dir, NULL, verb, names.v[0]);
	else
		dest = minibuf_read_filename ("%s %zu files to: ", cur_bp->dired->dir, NULL,
					      op == OP_COPY ? "Copy" : "Move", names.n);
	if (dest == NULL) {
		sv_clear (&names);
		return funcall ("keyboard-quit");
	}

	/* Remove a trailing slash, except from the root. */
	size_t dlen = strlen (dest);
	while (dlen > 1 && dest[dlen - 1] == '/')
		dest[--dlen] = '\0';

	bool into_dir = file_is_dir (dest);
	if (names.n > 1 && !into_dir) {
		minibuf_error ("Target `%s' is not a directory", dest);
		free (dest);
		sv_clear (&names);
		return false;
	}

	size_t done = 0;
	char *last_name = NULL;
	bool ok = true;
	for (size_t i = 0; ok && i < names.n; i++) {
		char *from = entry_path (cur_bp, names.v[i]);
		char *to = into_dir ? path_join (dest, names.v[i]) : xstrdup (dest);
		struct stat st;

		if (strcmp (from, to) == 0 || (file_is_dir (from) && path_within (to, from))) {
			minibuf_error ("Cannot %s `%s' to itself", op == OP_COPY ? "copy" : "move", names.v[i]);
			ok = false;
		} else if (lstat (to, &st) == 0 && !S_ISDIR (st.st_mode)) {
			int ans = minibuf_read_yesno ("File `%s' exists; overwrite? (yes or no) ", to);
			if (ans < 0)
				ok = false;
			else if (ans == 0) {
				free (from);
				free (to);
				continue;
			}
		}

		if (ok) {
			if (op == OP_COPY ? copy_path (from, to) : move_path (from, to)) {
				done++;
				if (op == OP_RENAME)
					update_visiting_buffers (from, to);
				free (last_name);
				last_name = path_basename (to);
			} else {
				minibuf_error ("%s of `%s' failed: %s", verb, names.v[i], strerror (errno));
				ok = false;
			}
		}
		free (from);
		free (to);
	}

	bool same_dir = !into_dir ? true : strcmp (dest, cur_bp->dired->dir) == 0;
	dired_refresh (cur_bp);
	if (last_name != NULL && same_dir && names.n == 1)
		goto_entry (cur_bp, last_name);
	if (ok)
		minibuf_write ("%s %zu file%s", op == OP_COPY ? "Copied" : "Moved", done, done == 1 ? "" : "s");

	free (last_name);
	free (dest);
	sv_clear (&names);
	return ok;
}

static bool
delete_entries (StrVec *names)
{
	int ans;
	if (names->n == 1)
		ans = minibuf_read_yesno ("Delete %s? (yes or no) ", names->v[0]);
	else
		ans = minibuf_read_yesno ("Delete %zu files? (yes or no) ", names->n);
	if (ans != 1)
		return false;

	size_t done = 0;
	bool ok = true;
	for (size_t i = 0; ok && i < names->n; i++) {
		char *path = entry_path (cur_bp, names->v[i]);
		struct stat st;
		if (lstat (path, &st) == 0 && S_ISDIR (st.st_mode) && !dir_is_empty (path)) {
			ans = minibuf_read_yesno ("Recursively delete %s? (yes or no) ", names->v[i]);
			if (ans < 0)
				ok = false;
			if (ans != 1) {
				free (path);
				continue;
			}
		}
		if (delete_path (path))
			done++;
		else {
			minibuf_error ("Deleting `%s' failed: %s", names->v[i], strerror (errno));
			ok = false;
		}
		free (path);
	}

	dired_refresh (cur_bp);
	if (ok)
		minibuf_write ("Deleted %zu file%s", done, done == 1 ? "" : "s");
	return ok;
}

/* Apply a chmod(1)-style mode: octal, or symbolic like "u+x,go-w". */
static bool
parse_mode (const char *spec, mode_t old, mode_t *out)
{
	long n;
	char *end;
	if (spec[0] >= '0' && spec[0] <= '7') {
		n = strtol (spec, &end, 8);
		if (*end != '\0' || n < 0 || n > 07777)
			return false;
		*out = (mode_t) n;
		return true;
	}

	mode_t mode = old & 07777;
	const char *p = spec;
	for (;;) {
		mode_t who = 0;
		for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
			who |= *p == 'u' ? (S_ISUID | S_IRWXU) : *p == 'g' ? (S_ISGID | S_IRWXG) :
				*p == 'o' ? S_IRWXO : (S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO);
		if (who == 0)
			who = S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO;
		do {
			char op = *p++;
			if (op != '+' && op != '-' && op != '=')
				return false;
			mode_t perm = 0;
			for (; *p != '\0' && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++)
				switch (*p) {
				case 'r': perm |= S_IRUSR | S_IRGRP | S_IROTH; break;
				case 'w': perm |= S_IWUSR | S_IWGRP | S_IWOTH; break;
				case 'x': perm |= S_IXUSR | S_IXGRP | S_IXOTH; break;
				case 'X':
					if (S_ISDIR (old) || (old & (S_IXUSR | S_IXGRP | S_IXOTH)))
						perm |= S_IXUSR | S_IXGRP | S_IXOTH;
					break;
				case 's': perm |= S_ISUID | S_ISGID; break;
				case 't': perm |= S_ISVTX; break;
				default: return false;
				}
			perm &= who;
			if (op == '+')
				mode |= perm;
			else if (op == '-')
				mode &= ~perm;
			else
				mode = (mode & ~who) | perm;
		} while (*p == '+' || *p == '-' || *p == '=');
		if (*p == '\0')
			break;
		if (*p++ != ',')
			return false;
	}
	*out = mode;
	return true;
}


/*--------------------------------------------------------------------------
 * Opening Dired buffers.
 *--------------------------------------------------------------------------*/

static int dired_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
			    size_t len, int start_state, const char **face_names);

static const Mode dired_name_mode = { "Dired by name", dired_scan_line, NULL, NULL, 0 };
static const Mode dired_date_mode = { "Dired by date", dired_scan_line, NULL, NULL, 0 };

static void init_dired_map (void);

/* Open a Dired buffer for `dir' (absolute, without a trailing slash,
   and owned by this function) here or on `r'. */
static bool
open_dired (Remote *r, char *dir)
{
	/* Reuse an existing buffer for the directory. */
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->dired != NULL && bp->remote == r && strcmp (bp->dired->dir, dir) == 0) {
			buffer_switch_to (bp, true);
			free (dir);
			return true;
		}

	size_t name_col;
	char *text = dir_listing (r, dir, false, &name_col);
	if (text == NULL) {
		free (dir);
		return false;
	}

	init_dired_map ();
	char *base = path_basename (dir);
	char *name = buffer_unique_name (base);
	free (base);

	Buffer *bp = buffer_new (NULL);
	bp->name = name;
	bp->dired = xzalloc (sizeof (Dired));
	bp->dired->dir = dir;
	bp->dired->name_col = name_col;
	bp->remote = r;
	if (r == NULL) {
		free (bp->dir);
		bp->dir = xstrdup (dir);
	}
	bp->nosave = true;
	bp->noundo = true;
	bp->keymap = dired_map;
	buffer_set_highlighter (bp, &dired_name_mode);

	buffer_insert_estr (bp, ev_cstr (text));
	free (text);
	bp->readonly = true;
	bp->modified = false;

	buffer_switch_to (bp, true);

	/* Start on the first file after `.' and `..'. */
	buffer_goto_offset (bp, 0);
	buffer_move_line (bp, HEADER_LINES);
	for (int i = 0; i < 2; i++) {
		char *n = line_entry (bp, buffer_line_o (bp));
		bool dot = n != NULL && is_dot_entry (n);
		free (n);
		if (!dot)
			break;
		buffer_move_line (bp, 1);
	}
	char *n = line_entry (bp, buffer_line_o (bp));
	if (n == NULL)	/* Only `.' and `..': stay on the last. */
		buffer_move_line (bp, -1);
	free (n);
	goto_filename (bp);
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}

static void
strip_slashes (char *dir)
{
	size_t len = strlen (dir);
	while (len > 1 && dir[len - 1] == '/')
		dir[--len] = '\0';
}

bool
dired (const char *dir_in)
{
	char *dir = expand_path (dir_in);
	if (dir == NULL || !file_is_dir (dir)) {
		minibuf_error ("%s: No such directory", dir_in);
		free (dir);
		return false;
	}
	strip_slashes (dir);
	return open_dired (NULL, dir);
}

/* Like dired, for the directory `dir_in' on `r'. */
bool
dired_remote (Remote *r, const char *dir_in)
{
	char *dir = remote_expand_path (r, dir_in);
	if (dir == NULL) {
		minibuf_error ("%s", remote_last_error ());
		return false;
	}
	strip_slashes (dir);
	char *noted = xstrdup (dir);
	bool ok = open_dired (r, dir);
	if (ok)
		location_note (r, noted);
	free (noted);
	return ok;
}

/* The directory a Dired buffer lists, or NULL. */
const char *
dired_directory (const Buffer *bp)
{
	return bp->dired != NULL ? bp->dired->dir : NULL;
}
static int
dired_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		 size_t len, int start_state, const char **face_names)
{
	(void) mode;
	(void) start_state;
	for (size_t i = 0; i < len; i++)
		face_names[i] = NULL;
	if (bp->dired == NULL)
		return SYNTAX_STATE_NORMAL;

	if (line_idx == 0) {
		for (size_t i = 2; i < len; i++)
			face_names[i] = FACE_FONT_LOCK_TYPE;
		return SYNTAX_STATE_NORMAL;
	}
	size_t nc = bp->dired->name_col;
	if (line_idx < HEADER_LINES || len <= nc)
		return SYNTAX_STATE_NORMAL;

	char mark = buffer_get_char (bp, start_o);
	char type = buffer_get_char (bp, start_o + 2);
	const char *name_face = mark == 'D' ? FACE_ERROR : mark == '*' ? FACE_WARNING :
		type == 'd' ? FACE_FONT_LOCK_FUNCTION_NAME : type == 'l' ? FACE_FONT_LOCK_KEYWORD : NULL;
	if (mark != ' ')
		face_names[0] = mark == 'D' ? FACE_ERROR : FACE_WARNING;

	size_t end = len;
	if (type == 'l')
		for (size_t i = nc; i + 3 < len; i++)
			if (buffer_get_char (bp, start_o + i) == ' ' && buffer_get_char (bp, start_o + i + 1) == '-' &&
			    buffer_get_char (bp, start_o + i + 2) == '>' && buffer_get_char (bp, start_o + i + 3) == ' ') {
				end = i;
				break;
			}
	for (size_t i = nc; i < end; i++)
		face_names[i] = name_face;
	return SYNTAX_STATE_NORMAL;
}


/*--------------------------------------------------------------------------
 * Commands.
 *--------------------------------------------------------------------------*/

static bool
F_dired (long uniarg, Args *args)
{
	(void) uniarg;
	Remote *loc = NULL;
	char *dir = xstrdup_or_null (args_poll (args));
	if (dir == NULL) {
		loc = cur_bp->remote;
		char *start = remote_buffer_dir (cur_bp);
		dir = minibuf_read_location_file ("Dired (directory): ", &loc, start != NULL ? start : cur_bp->dir, NULL);
		free (start);
		if (dir == NULL)
			return funcall ("keyboard-quit");
	}
	bool ok = dir[0] != '\0' && (loc != NULL ? dired_remote (loc, dir) : dired (dir));
	if (ok && loc == NULL && cur_bp->dired != NULL)
		location_note (NULL, cur_bp->dired->dir);
	free (dir);
	return ok;
}

static bool
F_dired_jump (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	char *dir, *file = NULL;
	const char *visited = cur_bp->remote != NULL ? cur_bp->remote_path : cur_bp->filename;
	if (visited != NULL) {
		dir = path_dirname (visited);
		file = path_basename (visited);
	} else if (cur_bp->dired != NULL) {
		dir = path_dirname (cur_bp->dired->dir);
		file = path_basename (cur_bp->dired->dir);
	} else
		dir = cur_bp->remote != NULL ? remote_buffer_dir (cur_bp) : xstrdup (cur_bp->dir);

	bool ok = cur_bp->remote != NULL ? dired_remote (cur_bp->remote, dir) : dired (dir);
	if (ok && file != NULL)
		goto_entry (cur_bp, file);
	free (dir);
	free (file);
	return ok;
}

static bool
move_lines (long n)
{
	if (!check_dired ())
		return false;
	bool ok = buffer_move_line (cur_bp, n);
	goto_filename (cur_bp);
	return ok;
}

static bool
F_dired_next_line (long uniarg, Args *args)
{
	(void) args;
	return move_lines (uniarg);
}

static bool
F_dired_previous_line (long uniarg, Args *args)
{
	(void) args;
	return move_lines (-uniarg);
}

/* Mark `n' lines (backwards if negative) with `c', moving over them. */
static bool
mark_lines (long n, char c)
{
	if (!check_dired ())
		return false;
	long count = n < 0 ? -n : n;
	for (long i = 0; i < count; i++) {
		if (n < 0 && !buffer_move_line (cur_bp, -1))
			break;
		size_t lo = buffer_line_o (cur_bp);
		char *name = line_entry (cur_bp, lo);
		if (name != NULL && !is_dot_entry (name))
			set_line_mark (cur_bp, lo, c);
		free (name);
		if (n > 0 && !buffer_move_line (cur_bp, 1))
			break;
	}
	goto_filename (cur_bp);
	return true;
}

static bool
F_dired_mark (long uniarg, Args *args)
{
	(void) args;
	return mark_lines (uniarg, '*');
}

static bool
F_dired_unmark (long uniarg, Args *args)
{
	(void) args;
	return mark_lines (uniarg, ' ');
}

static bool
F_dired_unmark_backward (long uniarg, Args *args)
{
	(void) args;
	return mark_lines (-uniarg, ' ');
}

static bool
F_dired_flag_file_deletion (long uniarg, Args *args)
{
	(void) args;
	return mark_lines (uniarg, 'D');
}

static bool
F_dired_unmark_all_marks (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired ())
		return false;
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (cur_bp, lo))
		if (line_mark (cur_bp, lo) != ' ' && buffer_offset_to_line (cur_bp, lo) >= HEADER_LINES)
			set_line_mark (cur_bp, lo, ' ');
	return true;
}

static bool
F_dired_toggle_marks (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired ())
		return false;
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (cur_bp, lo)) {
		char *name = line_entry (cur_bp, lo);
		if (name != NULL && !is_dot_entry (name)) {
			char mark = line_mark (cur_bp, lo);
			if (mark == '*')
				set_line_mark (cur_bp, lo, ' ');
			else if (mark == ' ')
				set_line_mark (cur_bp, lo, '*');
		}
		free (name);
	}
	return true;
}

static bool
mark_files_regexp (char mark)
{
	if (!check_dired ())
		return false;
	char *pattern = minibuf_read (mark == 'D' ? "Flag for deletion (regexp): " : "Mark files (regexp): ", "");
	if (pattern == NULL)
		return funcall ("keyboard-quit");

	const char *err;
	Regex *re = regex_compile (pattern, strlen (pattern), false, false, &err);
	free (pattern);
	if (re == NULL) {
		minibuf_error ("Invalid regexp: %s", err);
		return false;
	}

	size_t count = 0;
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (cur_bp, lo)) {
		char *name = line_entry (cur_bp, lo);
		size_t end;
		if (name != NULL && !is_dot_entry (name) &&
		    regex_search (re, name, strlen (name), true, -1, -1, &end) >= 0) {
			set_line_mark (cur_bp, lo, mark);
			count++;
		}
		free (name);
	}
	regex_free (re);
	minibuf_write ("%zu matching file%s %s", count, count == 1 ? "" : "s",
		       mark == 'D' ? "flagged" : "marked");
	return true;
}

static bool
F_dired_mark_files_regexp (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return mark_files_regexp ('*');
}

static bool
F_dired_flag_files_regexp (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return mark_files_regexp ('D');
}

static bool
F_dired_flag_backup_files (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired ())
		return false;
	size_t count = 0;
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (cur_bp, lo)) {
		char *name = line_entry (cur_bp, lo);
		if (name != NULL && str_has_suffix (name, "~")) {
			set_line_mark (cur_bp, lo, 'D');
			count++;
		}
		free (name);
	}
	minibuf_write ("%zu backup file%s flagged for deletion", count, count == 1 ? "" : "s");
	return true;
}

static bool
F_dired_do_flagged_delete (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired () || remote_refuse (cur_bp, "dired-do-flagged-delete"))
		return false;
	StrVec names = { NULL, 0, 0 };
	marked_entries (cur_bp, 'D', &names);
	bool ok = true;
	if (names.n == 0)
		minibuf_write ("(No deletions requested)");
	else
		ok = delete_entries (&names);
	sv_clear (&names);
	return ok;
}

static bool
F_dired_do_delete (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired () || remote_refuse (cur_bp, "dired-do-delete"))
		return false;
	StrVec names = { NULL, 0, 0 };
	bool ok = target_entries (cur_bp, &names) && delete_entries (&names);
	sv_clear (&names);
	return ok;
}

static bool
F_dired_do_rename (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return do_copy_or_rename (OP_RENAME);
}

static bool
F_dired_do_copy (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return do_copy_or_rename (OP_COPY);
}

static bool
F_dired_do_chmod (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired () || remote_refuse (cur_bp, "dired-do-chmod"))
		return false;
	StrVec names = { NULL, 0, 0 };
	if (!target_entries (cur_bp, &names))
		return false;

	char *spec = names.n == 1
		? minibuf_read ("Change mode of %s to: ", "", names.v[0])
		: minibuf_read ("Change mode of %zu files to: ", "", names.n);
	if (spec == NULL) {
		sv_clear (&names);
		return funcall ("keyboard-quit");
	}

	bool ok = true;
	for (size_t i = 0; ok && i < names.n; i++) {
		char *path = entry_path (cur_bp, names.v[i]);
		struct stat st;
		mode_t mode;
		if (stat (path, &st) != 0 || !parse_mode (spec, st.st_mode, &mode)) {
			minibuf_error ("Invalid mode `%s'", spec);
			ok = false;
		} else if (chmod (path, mode) != 0) {
			minibuf_error ("Changing mode of `%s' failed: %s", names.v[i], strerror (errno));
			ok = false;
		}
		free (path);
	}
	free (spec);
	sv_clear (&names);
	dired_refresh (cur_bp);
	return ok;
}

static bool
F_dired_create_directory (long uniarg, Args *args)
{
	(void) uniarg;
	if (!check_dired () || remote_refuse (cur_bp, "dired-create-directory"))
		return false;
	char *dir = xstrdup_or_null (args_poll (args));
	if (dir == NULL)
		dir = minibuf_read_filename ("Create directory: ", cur_bp->dired->dir, NULL);
	if (dir == NULL)
		return funcall ("keyboard-quit");

	char *path = expand_path (dir);
	free (dir);
	if (path == NULL)
		return false;
	size_t len = strlen (path);
	while (len > 1 && path[len - 1] == '/')
		path[--len] = '\0';

	bool ok = true;
	if (file_exists (path)) {
		minibuf_error ("%s already exists", path);
		ok = false;
	} else
		/* Create missing parents too. */
		for (size_t i = 1; ok && i <= len; i++)
			if (path[i] == '/' || path[i] == '\0') {
				char c = path[i];
				path[i] = '\0';
				if (mkdir (path, 0777) != 0 && errno != EEXIST) {
					minibuf_error ("Creating directory `%s' failed: %s", path, strerror (errno));
					ok = false;
				}
				path[i] = c;
			}

	if (ok) {
		dired_refresh (cur_bp);
		char *parent = path_dirname (path);
		if (strcmp (parent, cur_bp->dired->dir) == 0) {
			char *base = path_basename (path);
			goto_entry (cur_bp, base);
			free (base);
		}
		free (parent);
	}
	free (path);
	return ok;
}

/* Remove lines of marked files (or the current one) from the listing. */
static bool
F_dired_do_kill_lines (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired ())
		return false;

	size_t *starts = NULL, n = 0;
	for (size_t lo = 0; lo != NOPOS; lo = buffer_next_line (cur_bp, lo))
		if (line_mark (cur_bp, lo) == '*' && buffer_offset_to_line (cur_bp, lo) >= HEADER_LINES) {
			starts = xrealloc (starts, (n + 1) * sizeof (size_t));
			starts[n++] = lo;
		}
	if (n == 0) {
		char *name = line_entry (cur_bp, buffer_line_o (cur_bp));
		if (name == NULL) {
			minibuf_error ("No file on this line");
			return false;
		}
		free (name);
		starts = xmalloc (sizeof (size_t));
		starts[n++] = buffer_line_o (cur_bp);
	}

	cur_bp->readonly = false;
	for (size_t i = n; i > 0; i--) {
		size_t lo = starts[i - 1];
		size_t next = buffer_next_line (cur_bp, lo);
		buffer_goto_offset (cur_bp, lo);
		buffer_replace_estr (cur_bp, (next != NOPOS ? next : buffer_length (cur_bp)) - lo, ev_empty ());
	}
	cur_bp->readonly = true;
	cur_bp->modified = false;
	free (starts);
	goto_filename (cur_bp);
	minibuf_write ("Killed %zu line%s", n, n == 1 ? "" : "s");
	return true;
}

static bool
F_dired_copy_filename_as_kill (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired ())
		return false;
	StrVec names = { NULL, 0, 0 };
	if (!target_entries (cur_bp, &names))
		return false;

	StrBuf sb;
	sb_init (&sb);
	for (size_t i = 0; i < names.n; i++) {
		if (i > 0)
			sb_addc (&sb, ' ');
		sb_add (&sb, names.v[i]);
	}
	estr_free (kill_ring_text);
	kill_ring_text = NULL;
	kill_ring_push (ev_cstr (sb.s));
	minibuf_write ("%s", sb.s);
	sb_free (&sb);
	sv_clear (&names);
	return true;
}

/* Visit `path', listed in a Dired buffer here or on `r'. For a remote
   entry, `dir' says the listing shows a directory. */
static bool
visit (Remote *r, const char *path, bool dir)
{
	if (r != NULL)
		return dir ? dired_remote (r, path) : find_file_remote (r, path);
	return file_is_dir (path) ? dired (path) : find_file (path);
}

/* Whether the line at `lo' lists a directory. */
static bool
lists_dir (Buffer *bp, size_t lo)
{
	return buffer_end_of_line (bp, lo) > lo + 2 && buffer_get_char (bp, lo + 2) == 'd';
}

/* Whether the current line lists a directory. */
static bool
line_is_dir (Buffer *bp)
{
	return lists_dir (bp, buffer_line_o (bp));
}

/* The file of the entry `name' of `bp'. */
static char *
name_path (Buffer *bp, const char *name)
{
	if (strcmp (name, ".") == 0)
		return xstrdup (bp->dired->dir);
	if (strcmp (name, "..") == 0)
		return path_dirname (bp->dired->dir);
	return entry_path (bp, name);
}

static char *
current_path (void)
{
	if (!check_dired ())
		return NULL;
	char *name = line_entry (cur_bp, buffer_line_o (cur_bp));
	if (name == NULL) {
		minibuf_error ("No file on this line");
		return NULL;
	}
	char *path = name_path (cur_bp, name);
	free (name);
	return path;
}

static bool
F_dired_find_file (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Remote *r = cur_bp->remote;
	bool dir = line_is_dir (cur_bp);
	char *path = current_path ();
	bool ok = path != NULL && visit (r, path, dir);
	free (path);
	return ok;
}

static bool
F_dired_find_file_other_window (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Remote *r = cur_bp->remote;
	bool dir = line_is_dir (cur_bp);
	char *path = current_path ();
	if (path == NULL)
		return false;
	if (head_wp->next == NULL)
		funcall ("split-window");
	funcall ("other-window");
	bool ok = visit (r, path, dir);
	free (path);
	return ok;
}

bool
dired_link_at (Buffer *bp, size_t lo, size_t i, LinkSpan *span, char **target, enum LinkKind *kind)
{
	char *name = bp->dired != NULL ? line_entry (bp, lo) : NULL;
	if (name == NULL)
		return false;
	size_t start = bp->dired->name_col, end = start + strlen (name);
	if (i < start || i >= end) {
		free (name);
		return false;
	}
	*span = (LinkSpan) { start, end };
	char *path = name_path (bp, name);
	free (name);
	if (lists_dir (bp, lo))
		*kind = LINK_DIR;
	else if (buffer_get_char (bp, lo + 2) != 'l')
		*kind = LINK_FILE;
	else if (bp->remote != NULL)
		*kind = LINK_FILE_OR_DIR;
	else
		*kind = file_is_dir (path) ? LINK_DIR : LINK_FILE;
	if (bp->remote != NULL) {
		*target = xasprintf ("%s on %s", path, remote_label (bp->remote));
		free (path);
	} else {
		*target = compact_path (path);
		free (path);
	}
	return true;
}

/* A click at `o', on the line of point in the current Dired buffer:
   on the name of a directory, list it in this window; on the name of a
   file, visit it in another one (see window_for_link), keeping the
   listing in view. Elsewhere, nothing is done, and false returned. */
bool
dired_follow_link (size_t o)
{
	Buffer *bp = cur_bp;
	size_t lo = buffer_start_of_line (bp, o);
	LinkSpan span;
	char *target;
	enum LinkKind kind;
	if (!dired_link_at (bp, lo, o - lo, &span, &target, &kind))
		return false;
	free (target);

	Remote *r = bp->remote;
	bool dir = line_is_dir (bp);
	char *path = current_path ();
	if (path == NULL)
		return true;
	/* A symbolic link to a directory is listed like one. */
	if (!dir)
		find_file_for_link (cur_wp, r, path, &dir);
	if (dir)
		visit (r, path, true);
	free (path);
	return true;
}

static bool
F_dired_view_file (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Remote *r = cur_bp->remote;
	bool dir = line_is_dir (cur_bp);
	char *path = current_path ();
	bool ok = path != NULL && visit (r, path, dir);
	if (ok && cur_bp->dired == NULL)
		cur_bp->readonly = true;
	free (path);
	return ok;
}

static bool
F_dired_up_directory (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired ())
		return false;
	char *dir = xstrdup (cur_bp->dired->dir);
	if (strcmp (dir, "/") == 0) {
		free (dir);
		return false;
	}
	char *parent = path_dirname (dir);
	char *base = path_basename (dir);
	bool ok = cur_bp->remote != NULL ? dired_remote (cur_bp->remote, parent) : dired (parent);
	if (ok)
		goto_entry (cur_bp, base);
	free (dir);
	free (parent);
	free (base);
	return ok;
}

static bool
F_dired_revert (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return check_dired () && dired_refresh (cur_bp);
}

static bool
F_dired_sort_toggle (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_dired () || remote_refuse (cur_bp, "Sorting by date"))
		return false;
	cur_bp->dired->by_date = !cur_bp->dired->by_date;
	buffer_set_highlighter (cur_bp, cur_bp->dired->by_date ? &dired_date_mode : &dired_name_mode);
	return dired_refresh (cur_bp);
}

/* Bury the current buffer and show another one. */
static bool
F_quit_window (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Buffer *bp = cur_bp, *other = NULL;
	for (Buffer *it = head_bp; it != NULL && other == NULL; it = it->next)
		if (it != bp)
			other = it;
	if (other == NULL)
		return true;

	/* Move the buffer to the end of the list. */
	Buffer **pp = &head_bp;
	while (*pp != bp)
		pp = &(*pp)->next;
	*pp = bp->next;
	Buffer *last = head_bp;
	while (last->next != NULL)
		last = last->next;
	last->next = bp;
	bp->next = NULL;

	buffer_switch_to (other, true);
	return true;
}

static void
init_dired_map (void)
{
	if (dired_map != NULL)
		return;
	static const struct {
		const char *keys;
		const char *func;
	} bindings[] = {
		{ "n", "dired-next-line" },
		{ " ", "dired-next-line" },
		{ "\\C-n", "dired-next-line" },
		{ "\\DOWN", "dired-next-line" },
		{ "p", "dired-previous-line" },
		{ "\\C-p", "dired-previous-line" },
		{ "\\UP", "dired-previous-line" },
		{ "\\RET", "dired-find-file" },
		{ "f", "dired-find-file" },
		{ "e", "dired-find-file" },
		{ "o", "dired-find-file-other-window" },
		{ "v", "dired-view-file" },
		{ "^", "dired-up-directory" },
		{ "g", "dired-revert" },
		{ "m", "dired-mark" },
		{ "u", "dired-unmark" },
		{ "\\BACKSPACE", "dired-unmark-backward" },
		{ "U", "dired-unmark-all-marks" },
		{ "t", "dired-toggle-marks" },
		{ "d", "dired-flag-file-deletion" },
		{ "~", "dired-flag-backup-files" },
		{ "x", "dired-do-flagged-delete" },
		{ "D", "dired-do-delete" },
		{ "R", "dired-do-rename" },
		{ "C", "dired-do-copy" },
		{ "M", "dired-do-chmod" },
		{ "+", "dired-create-directory" },
		{ "k", "dired-do-kill-lines" },
		{ "w", "dired-copy-filename-as-kill" },
		{ "s", "dired-sort-toggle" },
		{ "%m", "dired-mark-files-regexp" },
		{ "%d", "dired-flag-files-regexp" },
		{ "q", "quit-window" },
	};
	dired_map = keymap_new ();
	for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++)
		keymap_bind (dired_map, bindings[i].keys, bindings[i].func);
}

void
dired_init (void)
{
	lisp_func_new ("dired", F_dired, true,
		       "Edit directory DIRNAME: list its files, and operate on them.\n"
		       "In Dired, `n' and `p' move between files, RET visits the file at point,\n"
		       "`m' marks and `u' unmarks it, `d' flags it for deletion and `x'\n"
		       "deletes flagged files. `R' renames or moves, `C' copies, `D' deletes\n"
		       "and `M' changes the mode of the marked files (or the file at point).\n"
		       "`+' creates a directory, `g' rereads the directory, `^' visits its\n"
		       "parent and `q' buries the buffer.");
	lisp_func_new ("dired-jump", F_dired_jump, true,
		       "Jump to Dired buffer corresponding to current buffer.\n"
		       "Point is put on the line of the file visited by the buffer.");
	lisp_func_new ("dired-next-line", F_dired_next_line, true,
		       "Move down lines then position at filename.");
	lisp_func_new ("dired-previous-line", F_dired_previous_line, true,
		       "Move up lines then position at filename.");
	lisp_func_new ("dired-find-file", F_dired_find_file, true,
		       "In Dired, visit the file or directory named on this line.");
	lisp_func_new ("dired-find-file-other-window", F_dired_find_file_other_window, true,
		       "In Dired, visit this file or directory in another window.");
	lisp_func_new ("dired-view-file", F_dired_view_file, true,
		       "In Dired, examine a file in view mode, returning to Dired when done.");
	lisp_func_new ("dired-up-directory", F_dired_up_directory, true,
		       "Run Dired on parent directory of current directory.");
	lisp_func_new ("dired-revert", F_dired_revert, true,
		       "Reread the Dired buffer, keeping marks.");
	lisp_func_new ("dired-mark", F_dired_mark, true,
		       "Mark the file at point in the Dired buffer.\n"
		       "With a prefix argument ARG, mark that many lines.");
	lisp_func_new ("dired-unmark", F_dired_unmark, true,
		       "Unmark the file at point in the Dired buffer.\n"
		       "With a prefix argument ARG, unmark that many lines.");
	lisp_func_new ("dired-unmark-backward", F_dired_unmark_backward, true,
		       "In a Dired buffer, move up lines and remove marks or deletion flags there.");
	lisp_func_new ("dired-unmark-all-marks", F_dired_unmark_all_marks, true,
		       "Remove all marks from all files in the Dired buffer.");
	lisp_func_new ("dired-toggle-marks", F_dired_toggle_marks, true,
		       "Toggle marks: marked files become unmarked, and vice versa.\n"
		       "Files flagged for deletion are left alone.");
	lisp_func_new ("dired-flag-file-deletion", F_dired_flag_file_deletion, true,
		       "In Dired, flag the current line's file for deletion.\n"
		       "With a prefix argument ARG, flag that many lines.");
	lisp_func_new ("dired-flag-backup-files", F_dired_flag_backup_files, true,
		       "Flag all backup files (names ending with `~') for deletion.");
	lisp_func_new ("dired-do-flagged-delete", F_dired_do_flagged_delete, true,
		       "In Dired, delete the files flagged for deletion.");
	lisp_func_new ("dired-do-delete", F_dired_do_delete, true,
		       "Delete all marked (or the current) files.");
	lisp_func_new ("dired-do-rename", F_dired_do_rename, true,
		       "Rename the current file or all marked files.\n"
		       "If the target is a directory, move the files into it.");
	lisp_func_new ("dired-do-copy", F_dired_do_copy, true,
		       "Copy all marked (or the current) files.\n"
		       "Directories are copied recursively.");
	lisp_func_new ("dired-do-chmod", F_dired_do_chmod, true,
		       "Change the mode of the marked (or the current) files.\n"
		       "The mode may be octal, or symbolic as for chmod(1), e.g. `u+x'.");
	lisp_func_new ("dired-create-directory", F_dired_create_directory, true,
		       "Create a directory called DIRECTORY, and any missing parents.");
	lisp_func_new ("dired-do-kill-lines", F_dired_do_kill_lines, true,
		       "Remove the lines of the marked (or the current) files from the listing.\n"
		       "The files themselves are not affected; `g' brings them back.");
	lisp_func_new ("dired-copy-filename-as-kill", F_dired_copy_filename_as_kill, true,
		       "Copy names of marked (or the current) files into the kill ring.");
	lisp_func_new ("dired-sort-toggle", F_dired_sort_toggle, true,
		       "Toggle sorting by date or by name.");
	lisp_func_new ("dired-mark-files-regexp", F_dired_mark_files_regexp, true,
		       "Mark all files matching REGEXP for use in later commands.");
	lisp_func_new ("dired-flag-files-regexp", F_dired_flag_files_regexp, true,
		       "In Dired, flag all files containing the specified REGEXP for deletion.");
	lisp_func_new ("quit-window", F_quit_window, true,
		       "Bury the current buffer and switch to another one.");
}
