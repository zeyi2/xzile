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
	}

	buffer_switch_to (bp, true);
	thisflag |= FLAG_NEED_RESYNC;
	return true;
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
			undo_set_unchanged (bp->last_undop);
		} else
			ok = false;
	}

	free (name);
	return ok;
}

static bool
save_buffer (Buffer *bp)
{
	if (bp->modified)
		return write_buffer (bp, bp->needname, false, bp->filename, "File to save in: ");

	minibuf_write ("(No changes need to be saved)");
	return true;
}

static bool
F_find_file (long uniarg, Args *args)
{
	(void) uniarg;
	bool ok = true;
	char *filename = xstrdup_or_null (args_poll (args));
	if (filename == NULL) {
		filename = minibuf_read_filename ("Find file: ", cur_bp->dir, NULL);
		if (filename == NULL)
			ok = funcall ("keyboard-quit");
	}
	if (filename == NULL || filename[0] == '\0')
		ok = false;

	if (ok)
		ok = find_file (filename);

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
	const char *buf = cur_bp->filename;
	char *basename = NULL;

	if (buf == NULL)
		buf = cur_bp->dir;
	else
		basename = path_basename (buf);
	char *ms = minibuf_read_filename ("Find alternate: ", buf, basename);
	free (basename);

	bool ok = false;
	if (ms == NULL)
		ok = funcall ("keyboard-quit");
	else if (ms[0] != '\0' && buffer_check_modified (cur_bp)) {
		buffer_kill (cur_bp);
		ok = find_file (ms);
	}
	free (ms);
	return ok;
}

static bool
F_switch_to_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	Buffer *bp = cur_bp->next != NULL ? cur_bp->next : head_bp;

	bool ok = true;
	char *buf = xstrdup_or_null (args_poll (args));
	if (buf == NULL) {
		Completion *cp = buffer_make_completion ();
		buf = minibuf_read_completion ("Switch to buffer (default %s): ", "", cp, NULL, bp->name);
		completion_free (cp);

		if (buf == NULL)
			ok = funcall ("keyboard-quit");
	}
	if (buf == NULL)
		ok = false;

	if (ok) {
		if (buf[0] != '\0') {
			bp = buffer_find (buf);
			if (bp == NULL) {
				bp = buffer_new (NULL);
				bp->name = xstrdup (buf);
				bp->needname = true;
				bp->nosave = true;
			}
		}

		buffer_switch_to (bp, true);
	}

	free (buf);
	return ok;
}

static bool
F_insert_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	Buffer *def_bp = cur_bp->next != NULL ? cur_bp->next : head_bp;

	if (buffer_warn_if_readonly (cur_bp))
		return false;

	bool ok = true;
	char *buf = xstrdup_or_null (args_poll (args));
	if (buf == NULL) {
		Completion *cp = buffer_make_completion ();
		buf = minibuf_read_completion ("Insert buffer (default %s): ", "", cp, NULL, def_bp->name);
		completion_free (cp);
		if (buf == NULL)
			ok = funcall ("keyboard-quit");
	}
	if (buf == NULL)
		ok = false;

	if (ok) {
		Buffer *bp;

		if (buf[0] != '\0') {
			bp = buffer_find (buf);
			if (bp == NULL) {
				minibuf_error ("Buffer `%s' not found", buf);
				ok = false;
			}
		} else
			bp = def_bp;

		if (ok) {
			buffer_insert_buffer (cur_bp, bp);
			funcall ("set-mark-command");
		}
	}

	free (buf);
	return ok;
}

static bool
F_insert_file (long uniarg, Args *args)
{
	(void) uniarg;
	if (buffer_warn_if_readonly (cur_bp))
		return false;

	bool ok = true;
	char *file = xstrdup_or_null (args_poll (args));
	if (file == NULL) {
		file = minibuf_read_filename ("Insert file: ", cur_bp->dir, NULL);
		if (file == NULL)
			ok = funcall ("keyboard-quit");
	}

	if (file == NULL || file[0] == '\0')
		ok = false;

	if (ok) {
		Estr *es = estr_from_file (file);
		if (es == NULL) {
			minibuf_error ("%s: %s", file, strerror (errno));
			ok = false;
		} else {
			buffer_insert_estr (cur_bp, estr_view (es));
			estr_free (es);
			funcall ("set-mark-command");
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
			const char *fname = buffer_get_filename_or_name (bp);
			if (noask)
				save_buffer (bp);
			else
				for (;;) {
					minibuf_write ("Save file %s? (y, n, !, ., q) ", fname);
					Keystroke c = getkey (GETKEY_DEFAULT);
					minibuf_clear ();

					if (c == KBD_CANCEL) {	/* C-g */
						funcall ("keyboard-quit");
						return false;
					} else if (c == 'q') {
						return true;
					} else if (c == '.') {
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

	zile_exit (EXIT_SUCCESS);
}

static bool
F_cd (long uniarg, Args *args)
{
	(void) uniarg;
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

static bool
F_revert_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
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

	Estr *es = estr_from_file (cur_bp->filename);
	if (es == NULL) {
		minibuf_error ("%s: %s", cur_bp->filename, strerror (errno));
		return false;
	}

	size_t old_pt = buffer_pt (cur_bp);
	bool old_noundo = cur_bp->noundo;

	cur_bp->noundo = true;
	cur_bp->readonly = false;
	buffer_goto_offset (cur_bp, 0);
	buffer_replace_estr (cur_bp, buffer_length (cur_bp), estr_view (es));
	estr_free (es);
	buffer_goto_offset (cur_bp, MIN (old_pt, buffer_length (cur_bp)));
	cur_bp->noundo = old_noundo;
	cur_bp->readonly = !check_writable (cur_bp->filename);

	cur_bp->modified = false;
	undo_free_all (cur_bp);
	thisflag |= FLAG_NEED_RESYNC;

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

	lisp_func_new ("switch-to-buffer", F_switch_to_buffer, true,
		       "Select buffer BUFFER in the current window.");

	lisp_func_new ("insert-buffer", F_insert_buffer, true,
		       "Insert after point the contents of BUFFER.\n"
		       "Puts mark after the inserted text.");

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
