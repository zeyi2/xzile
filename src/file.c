/* Disk file handling

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

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * This functions does some corrections and expansions to
 * the passed path:
 *
 * - expands `~/' and `~name/' expressions;
 * - replaces `//' with `/' (restarting from the root directory);
 * - removes `..' and `.' entries.
 *
 * Returns NULL on failure.
 */
char *
expand_path (const char *path)
{
	StrBuf epath;
	sb_init (&epath);

	if (path[0] != '/' && path[0] != '~') {
		char *cwd = get_current_dir ();
		sb_add (&epath, cwd);
		free (cwd);
		if (epath.len == 0 || epath.s[epath.len - 1] != '/')
			sb_addc (&epath, '/');
	}

	for (size_t i = 0; path[i] != '\0';) {
		if (path[i] == '/') {
			if (path[++i] == '/') {
				/* Got `//'.  Restart from this point. */
				while (path[i] == '/')
					i++;
				epath.len = 0;
				epath.s[0] = '\0';
			}
			if (epath.len == 0 || epath.s[epath.len - 1] != '/')
				sb_addc (&epath, '/');
		} else if (path[i] == '~' && (i == 0 || path[i - 1] == '/')) {
			/* Got `/~' or leading `~'.  Restart from this point. */
			epath.len = 0;
			epath.s[0] = '\0';
			++i;

			if (path[i] == '/' || path[i] == '\0') {
				/* Got `~/'.  Insert the user's home directory. */
				char *home = get_home_dir ();
				if (strcmp (home, "/") != 0)
					sb_add (&epath, home);
				free (home);
				if (path[i] == '\0')
					sb_addc (&epath, '/');
			} else {
				/* Got `~something'.  Insert that user's home directory. */
				size_t start = i;
				while (path[i] != '\0' && path[i] != '/')
					i++;
				char *name = xstrndup (path + start, i - start);
				struct passwd *pw = getpwnam (name);
				free (name);
				if (pw == NULL) {
					sb_free (&epath);
					return NULL;
				}
				sb_add (&epath, pw->pw_dir);
			}
		} else if (path[i] == '.' && (path[i + 1] == '/' || path[i + 1] == '\0')) {
			/* Got `.'. */
			++i;
		} else if (path[i] == '.' && path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == '\0')) {
			/* Got `..'. */
			if (epath.len >= 1 && epath.s[epath.len - 1] == '/')
				epath.len--;
			while (epath.len >= 1 && epath.s[epath.len - 1] != '/')
				epath.len--;
			epath.s[epath.len] = '\0';
			i += 2;
		} else
			while (path[i] != '\0' && path[i] != '/')
				sb_addc (&epath, path[i++]);
	}

	if (epath.len == 0)
		sb_addc (&epath, '/');
	return sb_take (&epath);
}

/*
 * Return a `~/foo' like path if the user is under his home directory,
 * else the unmodified path.
 */
char *
compact_path (const char *path)
{
	char *home = get_home_dir ();
	size_t homelen = strlen (home);
	if (homelen > 0 && home[homelen - 1] == '/')
		homelen--;

	char *res;
	/* Replace `/userhome/' (if found) with `~/'. */
	if (homelen > 0 && strlen (path) > homelen &&
	    strncmp (home, path, homelen) == 0 && path[homelen] == '/')
		res = xstrcat3 ("~/", path + homelen + 1, "");
	else
		res = xstrdup (path);
	free (home);
	return res;
}

/* Return true if file exists and can be written. */
static bool
check_writable (const char *filename)
{
	return faccessat (AT_FDCWD, filename, W_OK, AT_EACCESS) == 0;
}

#ifdef __APPLE__
#define ST_MTIM(st) ((st).st_mtimespec)
#else
#define ST_MTIM(st) ((st).st_mtim)
#endif

static FileStamp
file_stamp (const char *filename)
{
	FileStamp fs = { 0 };
	struct stat st;
	if (filename != NULL && stat (filename, &st) == 0) {
		fs.known = true;
		fs.mtime = ST_MTIM (st);
		fs.size = st.st_size;
		fs.dev = st.st_dev;
		fs.ino = st.st_ino;
	}
	return fs;
}

/* Record the visited file as matching the buffer. */
void
buffer_note_disk (Buffer *bp)
{
	bp->disk = file_stamp (bp->filename);
	bp->disk_warned = false;
}

/* Whether the visited file looks different from when it was last read
   or written. A file that has gone away does not count. */
static bool
buffer_disk_changed (Buffer *bp)
{
	/* Remote files are only checked when saved. */
	if (bp->filename == NULL || bp->remote != NULL || bp->needname || !bp->disk.known)
		return false;
	FileStamp now = file_stamp (bp->filename);
	return now.known &&
		(now.mtime.tv_sec != bp->disk.mtime.tv_sec || now.mtime.tv_nsec != bp->disk.mtime.tv_nsec ||
		 now.size != bp->disk.size || now.ino != bp->disk.ino || now.dev != bp->disk.dev);
}

static char
buffer_byte (EstrView pre, EstrView post, size_t i)
{
	return i < pre.len ? pre.text[i] : post.text[i - pre.len];
}

/* Find the part of `es' that differs from the buffer: everything but
   the first `*prefix' and last `*suffix' bytes. */
static void
text_diff (Buffer *bp, Estr *es, size_t *prefix, size_t *suffix)
{
	EstrView nv = estr_view (es);
	EstrView pre = buffer_pre_point (bp), post = buffer_post_point (bp);
	size_t len = pre.len + post.len, p = 0, s = 0;
	if (strcmp (nv.eol, buffer_eol (bp)) == 0) {
		size_t n = MIN (len, nv.len);
		while (p < n && buffer_byte (pre, post, p) == nv.text[p])
			p++;
		while (s < n - p && buffer_byte (pre, post, len - 1 - s) == nv.text[nv.len - 1 - s])
			s++;
	}
	*prefix = p;
	*suffix = s;
}

/* Read the visited file and find the part of it that differs from the
   buffer. Returns NULL if the file cannot be read. */
static Estr *
read_disk_diff (Buffer *bp, size_t *prefix, size_t *suffix)
{
	Estr *es = estr_from_file (bp->filename);
	if (es != NULL)
		text_diff (bp, es, prefix, suffix);
	return es;
}

static bool
buffer_matches_disk (Buffer *bp)
{
	size_t p, s;
	Estr *es = read_disk_diff (bp, &p, &s);
	if (es == NULL)
		return false;
	bool same = p == buffer_length (bp) && p == estr_view (es).len;
	estr_free (es);
	return same;
}

/* Replace the buffer text with `es', changing only the part that
   differs so that point, the mark and other windows stay where they
   were. */
static void
revert_to_text (Buffer *bp, Estr *es)
{
	size_t p, s;
	text_diff (bp, es, &p, &s);
	EstrView nv = estr_view (es);
	size_t len = buffer_length (bp);
	if (p < len || p < nv.len) {
		Marker *pt = xzalloc (sizeof (Marker));
		marker_move (pt, bp, buffer_pt (bp));
		bool readonly = bp->readonly, noundo = bp->noundo;
		bp->readonly = false;
		bp->noundo = true;
		buffer_goto_offset (bp, p);
		buffer_replace_estr (bp, len - p - s, ev_substring (nv, p, nv.len - p - s));
		buffer_goto_offset (bp, MIN (pt->o, buffer_length (bp)));
		marker_free (pt);
		bp->readonly = readonly;
		bp->noundo = noundo;
		undo_free_all (bp);
		thisflag |= FLAG_NEED_RESYNC;
	}
	bp->modified = false;
}

static bool
revert_from_disk (Buffer *bp)
{
	Estr *es = estr_from_file (bp->filename);
	if (es == NULL)
		return false;
	revert_to_text (bp, es);
	estr_free (es);
	buffer_note_disk (bp);
	return true;
}

/* Bring buffers in line with files changed on disk: revert unmodified
   buffers and warn about modified ones. Checks the current buffer, and
   the others too if `all' or a second has passed since they were last
   checked. Returns true if anything was reverted or reported. */
bool
buffers_check_disk (bool all)
{
	static int64_t last_all;
	if (!get_variable_bool ("global-auto-revert-mode"))
		return false;
	int64_t now = monotonic_ms ();
	if (now - last_all >= 1000)
		all = true;
	if (all)
		last_all = now;

	size_t reverted = 0;
	Buffer *last = NULL, *warned = NULL;
	for (Buffer *bp = all ? head_bp : cur_bp; bp != NULL; bp = all ? bp->next : NULL) {
		if (!buffer_disk_changed (bp))
			continue;
		if (preview_follow_disk (bp)) {
			buffer_note_disk (bp);
			continue;
		}
		if (!bp->modified) {
			if (revert_from_disk (bp)) {
				reverted++;
				last = bp;
			}
		} else if (buffer_matches_disk (bp)) {
			buffer_note_disk (bp);
		} else if (!bp->disk_warned) {
			bp->disk_warned = true;
			warned = bp;
		}
	}

	if (warned != NULL)
		minibuf_error ("%s changed on disk, but the buffer has unsaved changes", warned->name);
	else if (reverted == 1)
		minibuf_write ("Reverted buffer `%s' from changed file", last->name);
	else if (reverted > 1)
		minibuf_write ("Reverted %zu buffers from changed files", reverted);
	return warned != NULL || reverted > 0;
}

bool
find_file (const char *filename)
{
	Buffer *bp;
	for (bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->filename != NULL && strcmp (bp->filename, filename) == 0)
			break;

	if (bp == NULL) {
		if (file_is_dir (filename))
			return dired (filename);
		if (file_exists (filename) && !file_is_regular (filename)) {
			minibuf_error ("File exists but could not be read");
			return false;
		}

		bp = preview_visit (filename);
		if (bp == NULL) {
			Estr *es = estr_from_file (filename);
			if (es != NULL) {
				bp = buffer_new (es);
				bp->readonly = !check_writable (filename);
			} else
				bp = buffer_new (NULL);
			buffer_set_names (bp, filename);
			free (bp->dir);
			bp->dir = path_dirname (bp->filename);

			/* Reset undo history. */
			undo_free_all (bp);
			bp->modified = false;
			html_after_visit (bp);
			markdown_after_visit (bp);
		}
		buffer_note_disk (bp);
	}

	buffer_switch_to (bp, true);
	if (bp->filename != NULL)
		recentf_add (bp->filename);
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}

static Estr *
estr_from_strbuf (StrBuf *sb)
{
	Estr *es = xzalloc (sizeof (Estr));
	es->len = sb->len;
	es->text = sb_take (sb);
	es->buf_size = es->len;
	estr_set_eol_from_text (es);
	return es;
}

/* The buffer visiting `path' (absolute) on `r', read if need be,
   without showing it; `*kind' says what `path' is there. NULL for a
   directory or an error, and for a missing file unless `create'. */
static Buffer *
remote_file_buffer (Remote *r, const char *path, bool create, enum RemoteKind *kind)
{
	*kind = REMOTE_FILE;
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->remote == r && bp->remote_path != NULL && strcmp (bp->remote_path, path) == 0)
			return bp;

	StrBuf content;
	sb_init (&content);
	bool writable = true;
	*kind = remote_read (r, path, &content, &writable);
	if (*kind == REMOTE_ERROR || *kind == REMOTE_DIR || (*kind == REMOTE_MISSING && !create)) {
		sb_free (&content);
		return NULL;
	}

	Buffer *bp;
	char *sum = NULL;
	if (*kind == REMOTE_FILE) {
		sum = remote_checksum (content.s, content.len);
		bp = buffer_new (estr_from_strbuf (&content));
		bp->readonly = !writable;
	} else {
		sb_free (&content);
		bp = buffer_new (NULL);
	}
	bp->remote = r;
	bp->remote_path = xstrdup (path);
	bp->remote_sum = sum;
	char *base = path_basename (path);
	bp->name = buffer_unique_name (base);
	free (base);
	buffer_set_highlighter (bp, filetype_resolve_highlighter (path));
	markdown_after_visit (bp);
	undo_free_all (bp);
	bp->modified = false;
	if (*kind == REMOTE_MISSING)
		minibuf_write ("(New file on ssh:%s)", remote_label (r));
	return bp;
}

/* Visit the file or directory `path' (absolute) on `r'. A failure
   leaves everything as it was: nothing local is used instead. */
bool
find_file_remote (Remote *r, const char *path)
{
	enum RemoteKind kind;
	Buffer *bp = remote_file_buffer (r, path, true, &kind);
	if (bp == NULL) {
		if (kind == REMOTE_DIR)
			return dired_remote (r, path);
		minibuf_error ("%s", remote_last_error ());
		return false;
	}

	buffer_switch_to (bp, true);
	thisflag |= FLAG_NEED_RESYNC;
	char *dir = path_dirname (path);
	location_note (r, dir);
	free (dir);
	return true;
}

/* Visit the existing file `path' (absolute), on `r' unless it is NULL,
   for a link followed in the current window `from': in the window
   that window_for_link gives, which is selected. Nothing changes if
   the file cannot be read, having said why, or if it is a directory,
   which sets `*is_dir'. */
bool
find_file_for_link (Window *from, Remote *r, const char *path, bool *is_dir)
{
	*is_dir = false;
	Buffer *bp = NULL;
	if (r != NULL) {
		enum RemoteKind kind;
		bp = remote_file_buffer (r, path, false, &kind);
		if (bp == NULL) {
			if (kind == REMOTE_DIR)
				*is_dir = true;
			else if (kind == REMOTE_MISSING)
				minibuf_error ("%s on ssh:%s: No such file", path, remote_label (r));
			else
				minibuf_error ("%s", remote_last_error ());
			return false;
		}
	} else {
		if (file_is_dir (path)) {
			*is_dir = true;
			return false;
		}
		if (!file_is_regular (path)) {
			minibuf_error ("%s: No such file", path);
			return false;
		}
		for (bp = head_bp; bp != NULL; bp = bp->next)
			if (bp->filename != NULL && strcmp (bp->filename, path) == 0)
				break;
	}
	window_set_current (window_for_link (from, bp));
	return r != NULL ? find_file_remote (r, path) : find_file (path);
}

static int
write_all (int fd, const char *data, size_t length)
{
	for (size_t tot_written = 0; tot_written < length;) {
		ssize_t written = write (fd, data + tot_written, length - tot_written);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0)
			return 1;
		tot_written += (size_t) written;
	}
	return 0;
}

static bool
buffer_has_final_eol (Buffer *bp)
{
	const char *eol = buffer_eol (bp);
	size_t len = buffer_length (bp), eol_len = strlen (eol);
	if (len == 0)
		return true;
	if (len < eol_len)
		return false;

	size_t start = len - eol_len;
	for (size_t i = 0; i < eol_len; i++)
		if (buffer_get_char (bp, start + i) != eol[i])
			return false;
	return true;
}

static void
ensure_buffer_final_eol (Buffer *bp)
{
	if (buffer_has_final_eol (bp))
		return;

	size_t old_pt = buffer_pt (bp);
	bool old_readonly = bp->readonly;
	const char *eol = buffer_eol (bp);
	bp->readonly = false;
	buffer_goto_offset (bp, buffer_length (bp));
	buffer_insert_estr (bp, ev_of (eol, strlen (eol), eol));
	buffer_goto_offset (bp, MIN (old_pt, buffer_length (bp)));
	bp->readonly = old_readonly;
}

/*
 * Write buffer to given file name with given mode.
 */
int
write_to_disk (Buffer *bp, const char *filename, mode_t mode)
{
	int fd = open (filename, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;

	EstrView es = buffer_pre_point (bp);
	int ret = write_all (fd, es.text, es.len);
	if (ret == 0) {
		es = buffer_post_point (bp);
		ret = write_all (fd, es.text, es.len);
	}
	if (ret == 0 && !buffer_has_final_eol (bp))
		ret = write_all (fd, buffer_eol (bp), strlen (buffer_eol (bp)));

	if (close (fd) < 0 && ret == 0)
		ret = -1;

	return ret;
}

/*
 * Create a backup filename according to user specified variables.
 */
static char *
create_backup_filename (const char *filename, const char *backupdir)
{
	char *res = NULL;

	/* Prepend the backup directory path to the filename */
	if (backupdir != NULL && backupdir[0] != '\0') {
		StrBuf buf;
		sb_init (&buf);
		sb_add (&buf, backupdir);
		if (buf.s[buf.len - 1] != '/')
			sb_addc (&buf, '/');
		for (size_t i = 0; filename[i] != '\0'; i++)
			sb_addc (&buf, filename[i] == '/' ? '!' : filename[i]);

		res = expand_path (buf.s);
		sb_free (&buf);
	}

	char *ret = xstrcat3 (res != NULL ? res : filename, "~", "");
	free (res);
	return ret;
}

/* Copy the file `from' to `to', preserving permissions and times. */
bool
file_copy (const char *from, const char *to)
{
	int in = open (from, O_RDONLY);
	if (in < 0)
		return false;

	struct stat st;
	if (fstat (in, &st) != 0) {
		int saved = errno;
		close (in);
		errno = saved;
		return false;
	}

	int out = open (to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
	if (out < 0) {
		int saved = errno;
		close (in);
		errno = saved;
		return false;
	}

	bool ok = true;
	char buf[65536];
	for (;;) {
		ssize_t n = read (in, buf, sizeof buf);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ok = false;
			break;
		}
		if (n == 0)
			break;
		if (write_all (out, buf, (size_t) n) != 0) {
			ok = false;
			break;
		}
	}

	int saved = errno;
	if (ok) {
		if (fchown (out, st.st_uid, st.st_gid) != 0) { /* Ignore error. */ }
		fchmod (out, st.st_mode & 07777);
#ifdef __APPLE__
		struct timespec times[2] = { st.st_atimespec, st.st_mtimespec };
#else
		struct timespec times[2] = { st.st_atim, st.st_mtim };
#endif
		futimens (out, times);
	}
	close (in);
	if (close (out) != 0 && ok) {
		saved = errno;
		ok = false;
	}
	errno = saved;
	return ok;
}

/*
 * Write the buffer contents to a file.
 * Create a backup file if specified by the user variables.
 */
static bool
backup_and_write (Buffer *bp, const char *filename)
{
	/* Make backup of original file, the first time it is saved. */
	int fd;
	bool backup = get_variable_bool ("make-backup-files");
	if (!bp->backup && backup && (fd = open (filename, O_RDWR)) != -1) {
		close (fd);

		const char *backupdir = get_variable_bool ("backup-directory") ?
			get_variable ("backup-directory") : NULL;
		char *bfilename = create_backup_filename (filename, backupdir);
		if (file_copy (filename, bfilename))
			bp->backup = true;
		else {
			minibuf_error ("Cannot make backup file: %s", strerror (errno));
			waitkey ();
		}
		free (bfilename);
	}

	int ret = write_to_disk (bp, filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (ret == 0)
		return true;

	if (ret == -1)
		minibuf_error ("Error writing `%s': %s", filename, strerror (errno));
	else
		minibuf_error ("Error writing `%s'", filename);
	return false;
}

static bool
write_buffer (Buffer *bp, bool needname, bool confirm, const char *name0, const char *prompt)
{
	int ans = 1;
	bool ok = true;
	char *name;

	if (preview_refuse_save (bp))
		return false;
	if (!needname)
		name = xstrdup (name0);
	else {
		name = minibuf_read_filename ("%s", "", NULL, prompt);
		if (name == NULL)
			return funcall ("keyboard-quit");
		if (name[0] == '\0') {
			free (name);
			return false;
		}
		confirm = true;
	}

	if (confirm && file_exists (name)) {
		const char *errmsg = "";

		for (ans = -2; ans == -2;) {
			minibuf_write ("%sFile `%s' exists; overwrite? (y or n) ", errmsg, name);
			Keystroke key = getkeystroke (GETKEY_DEFAULT);
			if (key == 'y' || key == 'Y' || key == ' ' || key == KBD_RET)
				ans = 1;
			else if (key == 'N' || key == 'n' || key == KBD_DEL)
				ans = 0;
			else if (key == KBD_CANCEL)
				ans = -1;
			else
				errmsg = "Please answer y or n.  ";
		}

		if (ans == -1)
			funcall ("keyboard-quit");
		else if (ans == 0)
			minibuf_error ("Canceled");
		if (ans != 1)
			ok = false;
	}

	if (ans == 1 && bp->filename != NULL && strcmp (name, bp->filename) == 0 &&
	    buffer_disk_changed (bp) && !buffer_matches_disk (bp)) {
		ans = minibuf_read_yesno ("%s has changed since visited or saved.  Save anyway? (yes or no) ",
					  bp->name);
		if (ans < 0)
			funcall ("keyboard-quit");
		else if (ans == 0)
			minibuf_error ("Save not confirmed");
		if (ans != 1)
			ok = false;
	}

	if (ans == 1) {
		if (bp->filename == NULL || strcmp (name, bp->filename) != 0)
			buffer_set_names (bp, name);
		bp->needname = false;
		bp->temporary = false;
		bp->nosave = false;
		if (backup_and_write (bp, name)) {
			ensure_buffer_final_eol (bp);
			minibuf_write ("Wrote %s", name);
			bp->modified = false;
			buffer_note_disk (bp);
			undo_set_unchanged (bp->last_undop);
		} else
			ok = false;
	}

	free (name);
	return ok;
}

/* Save a remote buffer. The file is written only if it is as it was
   when last read or written (or the user agrees), and the buffer is
   marked unmodified only once the remote side has confirmed what it
   wrote. */
static bool
save_remote (Buffer *bp)
{
	StrBuf data;
	sb_init (&data);
	EstrView pre = buffer_pre_point (bp), post = buffer_post_point (bp);
	sb_addn (&data, pre.text, pre.len);
	sb_addn (&data, post.text, post.len);
	if (!buffer_has_final_eol (bp))
		sb_add (&data, buffer_eol (bp));
	char *want = remote_checksum (data.s, data.len);
	char *shown = remote_describe (bp);

	bool backup = !bp->backup && get_variable_bool ("make-backup-files");
	char *sum = NULL;
	bool backed_up = false;
	enum RemoteWrite rw = remote_write (bp->remote, bp->remote_path, bp->remote_sum, backup,
					    data.s, data.len, &sum, &backed_up);
	bool ok = false;
	if (rw == REMOTE_WRITE_CONFLICT) {
		int ans = 1;
		if (sum == NULL || strcmp (sum, want) != 0) {
			ans = minibuf_read_yesno ("%s has changed on ssh:%s since visited or saved.  Save anyway? (yes or no) ",
						  bp->name, remote_label (bp->remote));
			if (ans < 0)
				funcall ("keyboard-quit");
			else if (ans == 0)
				minibuf_error ("Save not confirmed");
		}
		free (sum);
		sum = NULL;
		if (ans == 1)
			rw = remote_write (bp->remote, bp->remote_path, "*", backup, data.s, data.len, &sum, &backed_up);
	}
	if (rw == REMOTE_WRITE_OK) {
		if (sum != NULL && strcmp (sum, want) == 0) {
			ensure_buffer_final_eol (bp);
			bp->modified = false;
			free (bp->remote_sum);
			bp->remote_sum = sum;
			sum = NULL;
			if (backed_up)
				bp->backup = true;
			undo_set_unchanged (bp->last_undop);
			minibuf_write ("Wrote %s", shown);
			ok = true;
		} else
			minibuf_error ("Writing %s: the remote file does not match the buffer, which is still modified", shown);
	} else if (rw == REMOTE_WRITE_ERROR)
		minibuf_error ("%s", remote_last_error ());
	free (sum);
	free (want);
	free (shown);
	sb_free (&data);
	return ok;
}

static bool
save_buffer (Buffer *bp)
{
	if (bp->modified)
		return bp->remote != NULL ? save_remote (bp)
			: write_buffer (bp, bp->needname, false, bp->filename, "File to save in: ");

	minibuf_write ("(No changes need to be saved)");
	return true;
}

static bool
F_find_file (long uniarg, Args *args)
{
	(void) uniarg;
	bool ok = true;
	Remote *loc = NULL;
	char *filename = xstrdup_or_null (args_poll (args));
	bool asked = filename == NULL;
	if (asked) {
		loc = cur_bp->remote;
		char *dir = remote_buffer_dir (cur_bp);
		filename = minibuf_read_location_file ("Find file: ", &loc, dir != NULL ? dir : cur_bp->dir, NULL);
		free (dir);
		if (filename == NULL)
			ok = funcall ("keyboard-quit");
	}
	if (filename == NULL || filename[0] == '\0')
		ok = false;

	if (ok)
		ok = loc != NULL ? find_file_remote (loc, filename) : find_file (filename);
	if (ok && asked && loc == NULL)
		location_note (NULL, cur_bp->dir);

	free (filename);
	return ok;
}

static bool
F_find_file_read_only (long uniarg, Args *args)
{
	bool ok = F_find_file (uniarg, args);
	if (ok)
		cur_bp->readonly = true;
	return ok;
}

static bool
F_find_alternate_file (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Remote *loc = cur_bp->remote;
	char *dir = remote_buffer_dir (cur_bp);
	const char *file = loc != NULL ? cur_bp->remote_path : cur_bp->filename;
	char *ms = minibuf_read_location_file ("Find alternate: ", &loc, dir != NULL ? dir : cur_bp->dir, file);
	free (dir);

	bool ok = false;
	if (ms == NULL)
		ok = funcall ("keyboard-quit");
	else if (ms[0] != '\0' && buffer_check_modified (cur_bp)) {
		if (loc == NULL) {
			buffer_kill (cur_bp);
			ok = find_file (ms);
			if (ok)
				location_note (NULL, cur_bp->dir);
		} else if (cur_bp->remote == loc && cur_bp->remote_path != NULL &&
			   strcmp (cur_bp->remote_path, ms) == 0) {
			buffer_kill (cur_bp);
			ok = find_file_remote (loc, ms);
		} else {
			/* The old buffer goes only once the new one could be
			   read: a remote failure must not lose it. */
			Buffer *old = cur_bp;
			ok = find_file_remote (loc, ms);
			if (ok && cur_bp != old && buffer_is_live (old))
				buffer_kill (old);
		}
	}
	free (ms);
	return ok;
}

static bool
F_insert_file (long uniarg, Args *args)
{
	(void) uniarg;
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	bool ok = true;
	Remote *loc = NULL;
	char *file = xstrdup_or_null (args_poll (args));
	bool asked = file == NULL;
	if (asked) {
		loc = cur_bp->remote;
		char *dir = remote_buffer_dir (cur_bp);
		file = minibuf_read_location_file ("Insert file: ", &loc, dir != NULL ? dir : cur_bp->dir, NULL);
		free (dir);
		if (file == NULL)
			ok = funcall ("keyboard-quit");
	}

	if (file == NULL || file[0] == '\0')
		ok = false;

	if (ok && loc != NULL) {
		StrBuf content;
		sb_init (&content);
		bool writable;
		enum RemoteKind kind = remote_read (loc, file, &content, &writable);
		if (kind == REMOTE_FILE) {
			Estr *es = estr_from_strbuf (&content);
			buffer_insert_estr (cur_bp, estr_view (es));
			estr_free (es);
			funcall ("set-mark-command");
			char *dir = path_dirname (file);
			location_note (loc, dir);
			free (dir);
		} else {
			sb_free (&content);
			if (kind == REMOTE_ERROR)
				minibuf_error ("%s", remote_last_error ());
			else
				minibuf_error ("%s on ssh:%s is %s", file, remote_label (loc),
					       kind == REMOTE_DIR ? "a directory" : "not there");
			ok = false;
		}
	} else if (ok) {
		Estr *es = estr_from_file (file);
		if (es == NULL) {
			minibuf_error ("%s: %s", file, strerror (errno));
			ok = false;
		} else {
			buffer_insert_estr (cur_bp, estr_view (es));
			estr_free (es);
			funcall ("set-mark-command");
			if (asked) {
				char *dir = path_dirname (file);
				location_note (NULL, dir);
				free (dir);
			}
		}
	}
	free (file);
	return ok;
}

static bool
F_save_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return save_buffer (cur_bp);
}

static bool
F_write_file (long uniarg, Args *args)
{
	(void) uniarg;
	if (remote_refuse (cur_bp, "write-file"))
		return false;
	return write_buffer (cur_bp, true, noarg (args), NULL, "Write file: ");
}

static bool
F_save_some_buffers (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	bool noask = false;

	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next) {
		if (bp->modified && !bp->nosave) {
			char *shown = remote_describe (bp);
			const char *fname = shown != NULL ? shown : buffer_get_filename_or_name (bp);
			if (noask)
				save_buffer (bp);
			else
				for (;;) {
					minibuf_write ("Save file %s? (y, n, !, ., q) ", fname);
					Keystroke c = getkey (GETKEY_DEFAULT);
					minibuf_clear ();

					if (c == KBD_CANCEL) {	/* C-g */
						free (shown);
						funcall ("keyboard-quit");
						return false;
					} else if (c == 'q') {
						free (shown);
						return true;
					} else if (c == '.') {
						free (shown);
						save_buffer (bp);
						return true;
					} else if (c == '!') {
						noask = true;
						save_buffer (bp);
						break;
					} else if (c == ' ' || c == 'y') {
						save_buffer (bp);
						break;
					} else if (c == 'n' || c == KBD_RET || c == KBD_DEL)
						break;
					else {
						minibuf_error ("Please answer y, n, !, . or q.");
						waitkey ();
					}
				}
			free (shown);
		}
	}

	minibuf_write ("(No files need saving)");
	return true;
}

static bool
F_save_buffers_kill_emacs (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!funcall ("save-some-buffers"))
		return false;

	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->modified && !bp->needname) {
			int ans = minibuf_read_yesno ("Modified buffers exist; exit anyway? (yes or no) ");
			if (ans < 0)
				return funcall ("keyboard-quit");
			else if (ans == 0)
				return false;
			break; /* We have found a modified buffer, so stop. */
		}

	if (process_any_alive ()) {
		int ans = minibuf_read_yesno ("Active processes exist; kill them and exit anyway? (yes or no) ");
		if (ans < 0)
			return funcall ("keyboard-quit");
		if (ans == 0)
			return false;
	}

	zile_exit (EXIT_SUCCESS);
}

static bool
F_cd (long uniarg, Args *args)
{
	(void) uniarg;
	if (remote_refuse (cur_bp, "cd"))
		return false;
	bool ok = true;
	char *dir = xstrdup_or_null (args_poll (args));
	if (dir == NULL)
		dir = minibuf_read_filename ("Change default directory: ", cur_bp->dir, NULL);

	if (dir == NULL)
		ok = funcall ("keyboard-quit");
	else if (dir[0] != '\0') {
		char *edir = expand_path (dir);
		if (edir == NULL || !file_is_dir (edir)) {
			minibuf_error ("`%s' is not a directory", dir);
			ok = false;
		} else if (chdir (edir) == -1) {
			minibuf_error ("%s: %s", dir, strerror (errno));
			ok = false;
		} else {
			free (cur_bp->dir);
			cur_bp->dir = get_current_dir ();
		}
		free (edir);
	}
	free (dir);
	return ok;
}

/* Revert a remote buffer from its file. */
static bool
revert_remote (Buffer *bp)
{
	StrBuf content;
	sb_init (&content);
	bool writable = true;
	enum RemoteKind kind = remote_read (bp->remote, bp->remote_path, &content, &writable);
	if (kind != REMOTE_FILE) {
		sb_free (&content);
		if (kind == REMOTE_ERROR)
			minibuf_error ("%s", remote_last_error ());
		else
			minibuf_error ("%s on ssh:%s is %s", bp->remote_path, remote_label (bp->remote),
				       kind == REMOTE_DIR ? "now a directory" : "not there");
		return false;
	}
	char *sum = remote_checksum (content.s, content.len);
	Estr *es = estr_from_strbuf (&content);
	revert_to_text (bp, es);
	estr_free (es);
	undo_free_all (bp);
	free (bp->remote_sum);
	bp->remote_sum = sum;
	bp->readonly = !writable;
	return true;
}

static bool
F_revert_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (cur_bp->remote != NULL && cur_bp->remote_path != NULL) {
		char *shown = remote_describe (cur_bp);
		int ans = minibuf_read_yesno ("Revert buffer from file %s? (yes or no) ", shown);
		free (shown);
		if (ans < 0) {
			funcall ("keyboard-quit");
			return false;
		}
		return ans > 0 && revert_remote (cur_bp);
	}
	if (cur_bp->filename == NULL) {
		minibuf_error ("Buffer does not seem to be associated with any file");
		return false;
	}

	int ans = minibuf_read_yesno ("Revert buffer from file %s? (yes or no) ", cur_bp->filename);
	if (ans < 0) {
		funcall ("keyboard-quit");
		return false;
	} else if (ans == 0)
		return false;

	if (preview_follow_disk (cur_bp)) {
		buffer_note_disk (cur_bp);
		return true;
	}
	if (!revert_from_disk (cur_bp)) {
		minibuf_error ("%s: %s", cur_bp->filename, strerror (errno));
		return false;
	}
	undo_free_all (cur_bp);
	if (!preview_showing (cur_bp))
		cur_bp->readonly = !check_writable (cur_bp->filename);
	return true;
}

void
file_init (void)
{
	lisp_func_new ("find-file", F_find_file, true,
		       "Edit file FILENAME.\n"
		       "Switch to a buffer visiting file FILENAME,\n"
		       "creating one if none already exists.");

	lisp_func_new ("find-file-read-only", F_find_file_read_only, true,
		       "Edit file FILENAME but don't allow changes.\n"
		       "Like `find-file', but marks buffer as read-only.\n"
		       "Use M-x toggle-read-only to permit editing.");

	lisp_func_new ("find-alternate-file", F_find_alternate_file, true,
		       "Find the file specified by the user, select its buffer, kill previous buffer.\n"
		       "If the current buffer now contains an empty file that you just visited\n"
		       "(presumably by mistake), use this command to visit the file you really want.");

	lisp_func_new ("insert-file", F_insert_file, true,
		       "Insert contents of file FILENAME into buffer after point.\n"
		       "Set mark after the inserted text.");

	lisp_func_new ("save-buffer", F_save_buffer, true,
		       "Save current buffer in visited file if modified.  By default, makes the\n"
		       "previous version into a backup file if this is the first save.");

	lisp_func_new ("write-file", F_write_file, true,
		       "Write current buffer into file FILENAME.\n"
		       "This makes the buffer visit that file, and marks it as not modified.\n"
		       "\n"
		       "Interactively, confirmation is required unless you supply a prefix argument.");

	lisp_func_new ("save-some-buffers", F_save_some_buffers, true,
		       "Save some modified file-visiting buffers.  Asks user about each one.");

	lisp_func_new ("save-buffers-kill-emacs", F_save_buffers_kill_emacs, true,
		       "Offer to save each buffer, then kill this Zile process.");

	lisp_func_new ("cd", F_cd, true,
		       "Make DIR become the current buffer's default directory.");

	lisp_func_new ("revert-buffer", F_revert_buffer, true,
		       "Replace the buffer text with the text of the visited file on disk.");
}
