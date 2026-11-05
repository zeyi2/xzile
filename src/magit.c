/* A Git interface in the style of Magit

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

/* `magit-status' shows a repository's state in sections, from which
   files and hunks can be staged, unstaged and discarded, and from which
   commits, branches, stashes, logs, diffs and remotes are reached
   through Magit's keys and popups.

   Large repositories are the point of the design:
   - A refresh runs its git queries concurrently, on worker threads,
     each of which also parses its own output.
   - The status comes from `git status --porcelain=v2', which costs
     O(files); a file's diff is only fetched when it is expanded. `4'
     fetches every diff at once, parsing large diffs on several threads.
   - Operations on many files pass the paths on standard input, with
     --pathspec-from-file, in one git command.
   - Very large sections start collapsed.

   Worker threads only run git and parse; they never touch buffers.

   A repository may be on another machine, reached through remote.c:
   git then runs there, through ssh, and the repository's buffers are
   remote buffers of that machine. Remote commands wait for ssh while
   reading the keyboard, on the editor's thread, so a refresh runs them
   there one after the other (parsing still uses threads); C-g cancels
   a remote command and remote-timeout bounds it, and a refresh whose
   commands did not all finish keeps the buffer as it was. A
   repository is known by its machine and its top directory: the same
   directory here and on another machine are different repositories. */

#include "xzile.h"

#include <ctype.h>
#include <pthread.h>
#include <unistd.h>

/* Sections with more entries than this start collapsed. */
#define MAX_EXPANDED_ENTRIES 500
/* Fetch the whole diff, rather than per file, beyond this many files. */
#define MAX_PATHS_PER_DIFF 64
/* Split diffs bigger than this to parse them on several threads. */
#define PARALLEL_PARSE_THRESHOLD (1024 * 1024)
#define LOG_LIMIT "256"

/*--------------------------------------------------------------------------
 * The model.
 *--------------------------------------------------------------------------*/

typedef struct {
	char *text;		/* "@@ ... @@" header line and body, each line ending in \n. */
	size_t len;
} Hunk;

typedef struct {
	char *path;		/* Relative to the repository root. */
	char *orig_path;	/* The old name of a renamed file. */
	const char *label;	/* "modified", "new file", ... */
	char *head;		/* The diff header, up to the "+++" line. */
	Hunk *hunks;
	size_t nhunks;
	bool has_diff;
	bool binary;
} FileDiff;

typedef struct {
	FileDiff *v;
	size_t n, cap;
} FileList;

enum Section {
	SEC_HEADER,
	SEC_UNTRACKED,
	SEC_UNMERGED,
	SEC_UNSTAGED,
	SEC_STAGED,
	SEC_STASHES,
	SEC_UNPULLED,
	SEC_UNPUSHED,
	SEC_RECENT,
	SEC_LOG,
	SEC_FILES,		/* The files of a diff or revision buffer. */
	NSECTIONS
};

enum LineKind {
	L_TEXT,
	L_HEADING,		/* A section heading. */
	L_FILE,
	L_HUNK,			/* A hunk header. */
	L_DIFF,			/* A line within a hunk. */
	L_COMMIT,
	L_STASH,
};

#define MAX_SPANS 4

typedef struct {
	enum LineKind kind;
	enum Section sec;
	int file;		/* Index in the section's file list. */
	int hunk;
	int line;		/* Index within the hunk; 0 is its header. */
	char *ref;		/* Commit hash or stash name. */
	const char *face;	/* For the whole line. */
	uint8_t nspans;
	struct {
		uint16_t start, len;
		const char *face;
	} spans[MAX_SPANS];
} LineInfo;

enum MagitKind {
	MAGIT_STATUS,
	MAGIT_LOG,
	MAGIT_REVISION,
	MAGIT_DIFF,
	MAGIT_PROCESS,
	MAGIT_COMMIT_MESSAGE,
};

/* A repository: its top directory, on a machine. */
typedef struct {
	Remote *remote;		/* The machine, or NULL for this one. */
	char *root;
} Repo;

typedef struct {
	char *branch;		/* NULL if detached. */
	char *oid;		/* NULL before the first commit. */
	char *head;		/* "HASH subject" */
	char *upstream;
	char *upstream_head;
	char *merging;		/* MERGE_HEAD, during a merge. */
	FileList untracked, unmerged, unstaged, staged;
	StrVec stashes, unpushed, unpulled, recent;
} Status;

struct Magit {
	enum MagitKind kind;
	Repo repo;
	char *args;		/* The log, diff or revision arguments. */
	bool amend, reword, all;	/* For commit messages. */
	Status st;
	FileList files;		/* Diff and revision buffers. */
	StrVec header;		/* Revision header lines. */
	StrVec log;		/* Raw `git log --graph' lines. */
	bool collapsed[NSECTIONS];
	bool collapsed_set[NSECTIONS];
	Map *toggled;		/* Files and hunks toggled from their default. */
	LineInfo *lines;
	size_t nlines, cap;
};

static Binding *status_map, *commit_map;
static int magit_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
			    size_t len, int start_state, const char **face_names);
static const Mode magit_mode = { "Magit", magit_scan_line, NULL, NULL, 0 };
static int commit_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
			     size_t len, int start_state, const char **face_names);
static const Mode commit_mode = { "Git-Commit", commit_scan_line, NULL, NULL, 0 };

static bool
repo_eq (const Repo *a, const Repo *b)
{
	return a->remote == b->remote && strcmp (a->root, b->root) == 0;
}

static Repo
repo_copy (const Repo *repo)
{
	return (Repo) { repo->remote, xstrdup (repo->root) };
}

static void
repo_clear (Repo *repo)
{
	free (repo->root);
	repo->root = NULL;
}

static void
file_diff_free (FileDiff *f)
{
	free (f->path);
	free (f->orig_path);
	free (f->head);
	for (size_t i = 0; i < f->nhunks; i++)
		free (f->hunks[i].text);
	free (f->hunks);
	memset (f, 0, sizeof *f);
}

static void
file_list_clear (FileList *fl)
{
	for (size_t i = 0; i < fl->n; i++)
		file_diff_free (&fl->v[i]);
	free (fl->v);
	fl->v = NULL;
	fl->n = fl->cap = 0;
}

static FileDiff *
file_list_add (FileList *fl)
{
	if (fl->n == fl->cap) {
		fl->cap = fl->cap ? fl->cap * 2 : 16;
		fl->v = xrealloc (fl->v, fl->cap * sizeof (FileDiff));
	}
	FileDiff *f = &fl->v[fl->n++];
	memset (f, 0, sizeof *f);
	f->label = "modified";
	return f;
}

/* Move the files of `src' to the end of `dst'. */
static void
file_list_append (FileList *dst, FileList *src)
{
	for (size_t i = 0; i < src->n; i++)
		*file_list_add (dst) = src->v[i];
	free (src->v);
	src->v = NULL;
	src->n = src->cap = 0;
}

static void
status_clear (Status *st)
{
	free (st->branch);
	free (st->oid);
	free (st->head);
	free (st->upstream);
	free (st->upstream_head);
	free (st->merging);
	file_list_clear (&st->untracked);
	file_list_clear (&st->unmerged);
	file_list_clear (&st->unstaged);
	file_list_clear (&st->staged);
	sv_clear (&st->stashes);
	sv_clear (&st->unpushed);
	sv_clear (&st->unpulled);
	sv_clear (&st->recent);
	memset (st, 0, sizeof *st);
}

static void
clear_lines (Magit *m)
{
	for (size_t i = 0; i < m->nlines; i++)
		free (m->lines[i].ref);
	m->nlines = 0;
}

bool
magit_buffer_is_transient (const Buffer *bp)
{
	return bp->magit != NULL &&
		(bp->magit->kind == MAGIT_PROCESS || bp->magit->kind == MAGIT_COMMIT_MESSAGE);
}

void
magit_free (Buffer *bp)
{
	Magit *m = bp->magit;
	if (m == NULL)
		return;
	status_clear (&m->st);
	file_list_clear (&m->files);
	sv_clear (&m->header);
	sv_clear (&m->log);
	map_free (m->toggled, NULL);
	clear_lines (m);
	free (m->lines);
	free (m->repo.root);
	free (m->args);
	free (m);
	bp->magit = NULL;
}

/* The top directory of the repository of a Magit buffer, or NULL. */
const char *
magit_directory (const Buffer *bp)
{
	return bp->magit != NULL ? bp->magit->repo.root : NULL;
}

/* Make `bp' a buffer of `repo'. The buffers of a remote repository
   are remote buffers of its machine, and keep the local directory they
   were made in. */
static void
set_repo (Buffer *bp, const Repo *repo, bool remote_buffer)
{
	bp->magit->repo = repo_copy (repo);
	if (repo->remote == NULL) {
		free (bp->dir);
		bp->dir = xstrdup (repo->root);
	} else if (remote_buffer)
		bp->remote = repo->remote;
}


/*--------------------------------------------------------------------------
 * Running git.
 *--------------------------------------------------------------------------*/

static const char *const git_env[] = {
	"GIT_TERMINAL_PROMPT=0",
	"GIT_OPTIONAL_LOCKS=0",
	"GIT_PAGER=cat",
	"GIT_EDITOR=false",
	NULL
};

/* "git SUBCOMMAND", to name a remote git command in messages. */
static char *
git_what (const char *const *args)
{
	for (size_t i = 0; args[i] != NULL; i++)
		if (strcmp (args[i], "-c") == 0 && args[i + 1] != NULL)
			i++;
		else if (args[i][0] != '-')
			return xasprintf ("git %s", args[i]);
	return xstrdup ("git");
}

/* Run git with `args' in `repo' (or in a directory of its machine).
   Returns git's exit status, or -1 if it could not be run; on another
   machine, also if it did not finish (cancelled, timed out or the
   connection lost), with remote_last_error saying why. Thread-safe
   for repositories on this machine only. */
static int
git_args (const Repo *repo, const char *const *args, const char *input, size_t input_len,
	  StrBuf *out, StrBuf *err)
{
	static const char *const opts[] = {
		"--no-pager", "-c", "core.quotepath=false", "-c", "color.ui=false", "-c", "diff.noprefix=false",
	};
	size_t no = sizeof opts / sizeof opts[0], ne = sizeof git_env / sizeof git_env[0] - 1, na = 0;
	while (args[na] != NULL)
		na++;
	const char **argv = xmalloc ((ne + no + na + 4) * sizeof (char *));
	size_t k = 0;
	if (repo->remote != NULL) {
		/* There git runs in the directory, with env(1) setting its
		   environment. */
		argv[k++] = "env";
		for (size_t i = 0; i < ne; i++)
			argv[k++] = git_env[i];
		argv[k++] = "git";
	} else {
		argv[k++] = "git";
		argv[k++] = "-C";
		argv[k++] = repo->root;
	}
	memcpy (argv + k, opts, no * sizeof (char *));
	memcpy (argv + k + no, args, (na + 1) * sizeof (char *));
	int status;
	if (repo->remote != NULL) {
		char *what = git_what (args);
		status = remote_run (repo->remote, repo->root, argv, input, input_len, out, err, what);
		free (what);
	} else
		status = process_run (argv, git_env, input, input_len, out, err);
	free (argv);
	return status;
}

/* Whether a git command in `repo', which gave `status', did not run
   to completion on the repository's machine; if so, say why. */
static bool
remote_failed (const Repo *repo, int status)
{
	if (repo->remote == NULL || status >= 0)
		return false;
	minibuf_error ("%s", remote_last_error ());
	return true;
}

/* Run a git query; returns its output, without a trailing newline, or
   NULL if it failed. `*status', if not NULL, gets git_args's result. */
static char *
git_query_status (const Repo *repo, const char *const *args, int *status)
{
	StrBuf out;
	sb_init (&out);
	int st = git_args (repo, args, NULL, 0, &out, NULL);
	if (status != NULL)
		*status = st;
	if (st != 0) {
		sb_free (&out);
		return NULL;
	}
	while (out.len > 0 && (out.s[out.len - 1] == '\n' || out.s[out.len - 1] == '\r'))
		out.s[--out.len] = '\0';
	return sb_take (&out);
}

/* The *magit-process* buffer logs the commands that change things. */
static Buffer *
process_buffer (const Repo *repo)
{
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->magit != NULL && bp->magit->kind == MAGIT_PROCESS && repo_eq (&bp->magit->repo, repo))
			return bp;
	char *base = path_basename (repo->root);
	char *name = xasprintf ("magit-process: %s", base);
	free (base);
	char *unique = buffer_unique_name (name);
	free (name);
	Buffer *bp = buffer_new (NULL);
	bp->name = unique;
	bp->nosave = true;
	bp->noundo = true;
	bp->readonly = true;
	bp->keymap = status_map;
	bp->mode_label = xstrdup ("Magit Process");
	bp->magit = xzalloc (sizeof (Magit));
	bp->magit->kind = MAGIT_PROCESS;
	set_repo (bp, repo, true);
	return bp;
}

static void
append_text (Buffer *bp, const char *s, size_t len)
{
	size_t pt = buffer_pt (bp), old_len = buffer_length (bp);
	bool readonly = bp->readonly;
	bp->readonly = false;
	buffer_goto_offset (bp, old_len);
	buffer_insert_estr (bp, ev_of (s, len, eol_lf));
	if (pt != old_len)
		buffer_goto_offset (bp, pt);
	bp->readonly = readonly;
	bp->modified = false;
}

static void
log_command (const Repo *repo, const char *const *args, int status, const StrBuf *out, const StrBuf *err)
{
	Buffer *bp = process_buffer (repo);
	StrBuf sb;
	sb_init (&sb);
	sb_addf (&sb, "%3d git", status);
	for (size_t i = 0; args[i] != NULL; i++)
		sb_addf (&sb, " %s", args[i]);
	sb_addc (&sb, '\n');
	if (out != NULL && out->len > 0) {
		sb_add (&sb, out->s);
		if (out->s[out->len - 1] != '\n')
			sb_addc (&sb, '\n');
	}
	if (err != NULL && err->len > 0) {
		sb_add (&sb, err->s);
		if (err->s[err->len - 1] != '\n')
			sb_addc (&sb, '\n');
	}
	if (status < 0 && repo->remote != NULL)
		sb_addf (&sb, "%s\n", remote_last_error ());
	sb_addc (&sb, '\n');
	append_text (bp, sb.s, sb.len);
	sb_free (&sb);
}

/* Report the last line of a failed command's error output. */
static void
report_failure (const StrBuf *out, const StrBuf *err)
{
	const StrBuf *b = err->len > 0 ? err : out;
	size_t end = b->len;
	while (end > 0 && (b->s[end - 1] == '\n' || b->s[end - 1] == ' '))
		end--;
	size_t start = end;
	while (start > 0 && b->s[start - 1] != '\n')
		start--;
	/* Prefer git's own "error:" or "fatal:" line. */
	for (size_t i = 0; i < b->len;) {
		size_t eol = i;
		while (eol < b->len && b->s[eol] != '\n')
			eol++;
		if (strncmp (b->s + i, "fatal:", 6) == 0 || strncmp (b->s + i, "error:", 6) == 0) {
			start = i;
			end = eol;
			break;
		}
		i = eol + 1;
	}
	minibuf_error ("%.*s (see $ for details)", (int) (end - start), b->s + start);
}

/* Run a git command that changes things, logging it; returns true if
   it succeeded, reporting any error otherwise. */
static bool
git_run_input (const Repo *repo, const char *const *args, const char *input, size_t input_len, StrBuf *out_copy)
{
	StrBuf out, err;
	sb_init (&out);
	sb_init (&err);
	int status = git_args (repo, args, input, input_len, &out, &err);
	log_command (repo, args, status, &out, &err);
	if (status != 0 && !remote_failed (repo, status))
		report_failure (&out, &err);
	if (out_copy != NULL && status == 0)
		sb_add (out_copy, out.s);
	sb_free (&out);
	sb_free (&err);
	return status == 0;
}

static bool
git_run (const Repo *repo, const char *const *args)
{
	return git_run_input (repo, args, NULL, 0, NULL);
}


/*--------------------------------------------------------------------------
 * Parallel jobs.
 *--------------------------------------------------------------------------*/

typedef struct Job Job;
typedef void (*JobFn) (Job *job);

struct Job {
	JobFn fn;
	const Repo *repo;	/* NULL for parse jobs. */
	const char *args[16];
	char **argv;		/* For diff jobs, instead of `args'. */
	bool skip;		/* Do not run git: an earlier command failed. */
	StrBuf out;
	int status;
	void *result;		/* Filled in by `fn'. */
	const char *text;	/* For parse jobs. */
	size_t len;
};

static void *
job_thread (void *p)
{
	Job *job = p;
	job->fn (job);
	return NULL;
}

/* Run jobs concurrently, one thread each, and wait for them all. The
   jobs of a remote repository run on this thread instead, one after
   the other, as remote commands must; once one of them could not run
   git to completion, the others do not run it. */
static void
run_jobs (Job *jobs, size_t n)
{
	bool remote = false;
	for (size_t i = 0; i < n; i++)
		remote = remote || (jobs[i].repo != NULL && jobs[i].repo->remote != NULL);
	if (remote) {
		bool failed = false;
		for (size_t i = 0; i < n; i++) {
			jobs[i].skip = failed;
			jobs[i].fn (&jobs[i]);
			failed = failed || jobs[i].status < 0;
		}
		return;
	}

	pthread_t *threads = xmalloc ((n ? n : 1) * sizeof (pthread_t));
	bool *started = xzalloc ((n ? n : 1) * sizeof (bool));
	for (size_t i = 0; i < n; i++)
		started[i] = pthread_create (&threads[i], NULL, job_thread, &jobs[i]) == 0;
	for (size_t i = 0; i < n; i++) {
		if (started[i])
			pthread_join (threads[i], NULL);
		else
			jobs[i].fn (&jobs[i]);
	}
	free (threads);
	free (started);
}

static size_t
cpu_count (void)
{
	long n = sysconf (_SC_NPROCESSORS_ONLN);
	return n > 0 ? (size_t) n : 2;
}

static void
run_git_job (Job *job, const char *const *args)
{
	sb_init (&job->out);
	job->status = job->skip ? -1 : git_args (job->repo, args, NULL, 0, &job->out, NULL);
}

static void
query_job (Job *job)
{
	run_git_job (job, job->args);
}

static void
set_args (Job *job, const Repo *repo, JobFn fn, const char *const *args)
{
	memset (job, 0, sizeof *job);
	job->fn = fn;
	job->repo = repo;
	for (size_t i = 0; args[i] != NULL && i < 15; i++)
		job->args[i] = args[i];
}


/*--------------------------------------------------------------------------
 * Parsing diffs.
 *--------------------------------------------------------------------------*/

static bool
line_starts (const char *s, size_t len, const char *prefix)
{
	size_t n = strlen (prefix);
	return len >= n && memcmp (s, prefix, n) == 0;
}

static char *
strip_side (const char *s, size_t len)
{
	/* "a/path" or "b/path"; "/dev/null" stays as it is. */
	if (len >= 2 && (s[0] == 'a' || s[0] == 'b' || s[0] == 'c' || s[0] == 'i' || s[0] == 'w') && s[1] == '/')
		return xstrndup (s + 2, len - 2);
	return xstrndup (s, len);
}

/* Parse `git diff' output into files and hunks. */
static void
parse_diff (const char *s, size_t len, FileList *out)
{
	FileDiff *f = NULL;
	StrBuf head, hunk;
	bool in_hunk = false;
	sb_init (&head);
	sb_init (&hunk);

#define FINISH_HUNK()								\
	do {									\
		if (in_hunk && f != NULL) {					\
			f->hunks = xrealloc (f->hunks, (f->nhunks + 1) * sizeof (Hunk)); \
			f->hunks[f->nhunks].len = hunk.len;			\
			f->hunks[f->nhunks++].text = sb_take (&hunk);		\
			sb_init (&hunk);					\
		}								\
		in_hunk = false;						\
	} while (0)
#define FINISH_FILE()							\
	do {								\
		FINISH_HUNK ();						\
		if (f != NULL) {					\
			f->head = sb_take (&head);			\
			sb_init (&head);				\
		}							\
	} while (0)

	for (size_t i = 0; i < len;) {
		size_t eol = i;
		while (eol < len && s[eol] != '\n')
			eol++;
		const char *l = s + i;
		size_t ll = eol - i;
		size_t next = eol < len ? eol + 1 : len;

		if (line_starts (l, ll, "diff --git ") || line_starts (l, ll, "diff --cc ") ||
		    line_starts (l, ll, "diff --combined ")) {
			FINISH_FILE ();
			f = file_list_add (out);
			f->has_diff = true;
			if (line_starts (l, ll, "diff --git ")) {
				/* "diff --git a/X b/Y": take Y, after the last " b/". */
				const char *b = NULL;
				for (const char *p = l; p + 3 <= l + ll; p++)
					if (memcmp (p, " b/", 3) == 0)
						b = p;
				f->path = b != NULL ? xstrndup (b + 3, (size_t) (l + ll - b - 3)) : xstrndup (l + 11, ll - 11);
			} else {
				const char *p = memchr (l + 5, ' ', ll - 5);
				f->path = p != NULL ? xstrndup (p + 1, (size_t) (l + ll - p - 1)) : xstrdup ("");
				f->label = "unmerged";
			}
			sb_addn (&head, l, next - i);
		} else if (f != NULL && !in_hunk && !(line_starts (l, ll, "@@"))) {
			sb_addn (&head, l, next - i);
			if (line_starts (l, ll, "new file mode"))
				f->label = "new file";
			else if (line_starts (l, ll, "deleted file mode"))
				f->label = "deleted";
			else if (line_starts (l, ll, "rename from ")) {
				free (f->orig_path);
				f->orig_path = xstrndup (l + 12, ll - 12);
				f->label = "renamed";
			} else if (line_starts (l, ll, "rename to ")) {
				free (f->path);
				f->path = xstrndup (l + 10, ll - 10);
			} else if (line_starts (l, ll, "copy to ")) {
				free (f->path);
				f->path = xstrndup (l + 8, ll - 8);
				f->label = "copied";
			} else if (line_starts (l, ll, "new mode"))
				f->label = f->label == NULL ? "modified" : f->label;
			else if (line_starts (l, ll, "Binary files") || line_starts (l, ll, "GIT binary patch"))
				f->binary = true;
			else if (line_starts (l, ll, "+++ ") && !line_starts (l, ll, "+++ /dev/null")) {
				free (f->path);
				f->path = strip_side (l + 4, ll - 4);
			}
		} else if (f != NULL && line_starts (l, ll, "@@")) {
			FINISH_HUNK ();
			in_hunk = true;
			sb_addn (&hunk, l, next - i);
			if (eol == len)
				sb_addc (&hunk, '\n');
		} else if (in_hunk) {
			sb_addn (&hunk, l, next - i);
			if (eol == len)
				sb_addc (&hunk, '\n');
		}
		i = next;
	}
	FINISH_FILE ();
	sb_free (&head);
	sb_free (&hunk);
#undef FINISH_HUNK
#undef FINISH_FILE
}

static void
parse_job (Job *job)
{
	FileList *fl = xzalloc (sizeof (FileList));
	parse_diff (job->text, job->len, fl);
	job->result = fl;
}

/* Parse a diff, splitting a large one at file boundaries to parse the
   pieces on several threads. */
static void
parse_diff_parallel (const char *s, size_t len, FileList *out)
{
	size_t nchunks = MIN (cpu_count (), len / PARALLEL_PARSE_THRESHOLD + 1);
	if (nchunks <= 1) {
		parse_diff (s, len, out);
		return;
	}

	Job *jobs = xzalloc (nchunks * sizeof (Job));
	size_t start = 0, n = 0;
	for (size_t k = 0; k < nchunks && start < len; k++) {
		size_t end = k + 1 == nchunks ? len : MIN (len, start + len / nchunks);
		/* Move the end to the start of a file. */
		while (end < len) {
			const char *p = xmemmem (s + end, len - end, "\ndiff --", 8);
			if (p == NULL) {
				end = len;
				break;
			}
			end = (size_t) (p - s) + 1;
			break;
		}
		jobs[n].fn = parse_job;
		jobs[n].text = s + start;
		jobs[n].len = end - start;
		n++;
		start = end;
	}
	run_jobs (jobs, n);
	for (size_t i = 0; i < n; i++) {
		FileList *fl = jobs[i].result;
		file_list_append (out, fl);
		free (fl);
	}
	free (jobs);
}

static void
diff_job (Job *job)
{
	run_git_job (job, (const char *const *) job->argv);
	FileList *fl = xzalloc (sizeof (FileList));
	if (job->status == 0)
		parse_diff_parallel (job->out.s, job->out.len, fl);
	sb_free (&job->out);
	job->result = fl;
}


/*--------------------------------------------------------------------------
 * Parsing `git status --porcelain=v2 -z --branch'.
 *--------------------------------------------------------------------------*/

static const char *
status_label (char c)
{
	switch (c) {
	case 'A': return "new file";
	case 'D': return "deleted";
	case 'R': return "renamed";
	case 'C': return "copied";
	case 'T': return "typechange";
	default: return "modified";
	}
}

static const char *
unmerged_label (char x, char y)
{
	if (x == 'U' && y == 'U') return "both modified";
	if (x == 'A' && y == 'A') return "both added";
	if (x == 'D' && y == 'D') return "both deleted";
	if (x == 'A' && y == 'U') return "added by us";
	if (x == 'U' && y == 'A') return "added by them";
	if (x == 'D' && y == 'U') return "deleted by us";
	if (x == 'U' && y == 'D') return "deleted by them";
	return "unmerged";
}

/* Skip `n' space-separated fields. */
static const char *
skip_fields (const char *s, int n)
{
	for (int i = 0; i < n && s != NULL; i++) {
		s = strchr (s, ' ');
		if (s != NULL)
			s++;
	}
	return s;
}

static void
add_status_file (FileList *fl, const char *path, const char *orig, const char *label)
{
	FileDiff *f = file_list_add (fl);
	f->path = xstrdup (path);
	f->orig_path = xstrdup_or_null (orig);
	f->label = label;
}

static void
status_job (Job *job)
{
	query_job (job);
	Status *st = xzalloc (sizeof (Status));
	job->result = st;
	if (job->status != 0) {
		sb_free (&job->out);
		return;
	}

	const char *s = job->out.s, *end = s + job->out.len;
	while (s < end) {
		size_t n = strnlen (s, (size_t) (end - s));
		const char *rec = s;
		s += n + 1;
		if (n < 2)
			continue;
		if (rec[0] == '#') {
			if (str_has_prefix (rec, "# branch.oid ") && strcmp (rec + 13, "(initial)") != 0)
				st->oid = xstrdup (rec + 13);
			else if (str_has_prefix (rec, "# branch.head ") && strcmp (rec + 14, "(detached)") != 0)
				st->branch = xstrdup (rec + 14);
			else if (str_has_prefix (rec, "# branch.upstream "))
				st->upstream = xstrdup (rec + 18);
		} else if (rec[0] == '1' || rec[0] == '2') {
			char x = rec[2], y = rec[3];
			const char *path = skip_fields (rec, rec[0] == '1' ? 8 : 9);
			const char *orig = NULL;
			if (rec[0] == '2' && s < end) {
				orig = s;
				s += strnlen (s, (size_t) (end - s)) + 1;
			}
			if (path == NULL)
				continue;
			if (x != '.')
				add_status_file (&st->staged, path, orig, status_label (x));
			if (y != '.')
				add_status_file (&st->unstaged, path, y == 'R' ? orig : NULL, status_label (y));
		} else if (rec[0] == 'u') {
			const char *path = skip_fields (rec, 10);
			if (path != NULL)
				add_status_file (&st->unmerged, path, NULL, unmerged_label (rec[2], rec[3]));
		} else if (rec[0] == '?')
			add_status_file (&st->untracked, rec + 2, NULL, "untracked");
	}
	sb_free (&job->out);
}

static void
lines_job (Job *job)
{
	query_job (job);
	StrVec *sv = xzalloc (sizeof (StrVec));
	job->result = sv;
	if (job->status == 0)
		for (const char *p = job->out.s; *p != '\0';) {
			size_t n = strcspn (p, "\n");
			if (n > 0) {
				char *line = xstrndup (p, n);
				sv_push (sv, line);
				free (line);
			}
			p += n + (p[n] != '\0');
		}
	sb_free (&job->out);
}

static void
string_job (Job *job)
{
	query_job (job);
	if (job->status == 0) {
		while (job->out.len > 0 && job->out.s[job->out.len - 1] == '\n')
			job->out.s[--job->out.len] = '\0';
		job->result = sb_take (&job->out);
	} else
		sb_free (&job->out);
}


/*--------------------------------------------------------------------------
 * Rendering.
 *--------------------------------------------------------------------------*/

typedef struct {
	Magit *m;
	StrBuf text;
} Render;

static LineInfo *
add_line (Render *r, enum LineKind kind, enum Section sec, const char *face, const char *fmt, ...)
	PRINTF_LIKE (5, 6);

static LineInfo *
add_line (Render *r, enum LineKind kind, enum Section sec, const char *face, const char *fmt, ...)
{
	Magit *m = r->m;
	va_list ap;
	va_start (ap, fmt);
	char *s = xvasprintf (fmt, ap);
	va_end (ap);
	sb_add (&r->text, s);
	sb_addc (&r->text, '\n');
	free (s);

	if (m->nlines == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 256;
		m->lines = xrealloc (m->lines, m->cap * sizeof (LineInfo));
	}
	LineInfo *li = &m->lines[m->nlines++];
	memset (li, 0, sizeof *li);
	li->kind = kind;
	li->sec = sec;
	li->face = face;
	li->file = li->hunk = li->line = -1;
	return li;
}

static void
add_span (LineInfo *li, size_t start, size_t len, const char *face)
{
	if (li->nspans < MAX_SPANS && start < 65535) {
		li->spans[li->nspans].start = (uint16_t) start;
		li->spans[li->nspans].len = (uint16_t) MIN (len, 65535 - start);
		li->spans[li->nspans].face = face;
		li->nspans++;
	}
}

static char
section_letter (enum Section sec)
{
	return (char) ('A' + sec);
}

static char *
toggle_key (enum Section sec, const char *path, const char *hunk)
{
	if (hunk == NULL)
		return xasprintf ("%c:%s", section_letter (sec), path);
	size_t n = strcspn (hunk, "\n");
	return xasprintf ("%c:%s:%.*s", section_letter (sec), path, (int) n, hunk);
}

static bool
is_toggled (Magit *m, const char *key)
{
	return m->toggled != NULL && map_get (m->toggled, key) != NULL;
}

static void
toggle (Magit *m, const char *key)
{
	if (m->toggled == NULL)
		m->toggled = map_new ();
	if (map_get (m->toggled, key) != NULL)
		map_remove (m->toggled, key);
	else
		map_set (m->toggled, key, (void *) 1);
}

/* Files are collapsed in the status buffer and expanded elsewhere. */
static bool
file_expanded (Magit *m, enum Section sec, const char *path)
{
	char *key = toggle_key (sec, path, NULL);
	bool toggled = is_toggled (m, key);
	free (key);
	return m->kind == MAGIT_STATUS ? toggled : !toggled;
}

static bool
hunk_collapsed (Magit *m, enum Section sec, const char *path, const char *hunk)
{
	char *key = toggle_key (sec, path, hunk);
	bool toggled = is_toggled (m, key);
	free (key);
	return toggled;
}

static bool
section_collapsed (Magit *m, enum Section sec, size_t entries)
{
	if (!m->collapsed_set[sec]) {
		m->collapsed[sec] = entries > MAX_EXPANDED_ENTRIES;
		m->collapsed_set[sec] = true;
	}
	return m->collapsed[sec];
}

static void
render_hunks (Render *r, enum Section sec, int fi, const FileDiff *f)
{
	for (size_t h = 0; h < f->nhunks; h++) {
		const char *t = f->hunks[h].text;
		size_t n = strcspn (t, "\n");
		LineInfo *li = add_line (r, L_HUNK, sec, FACE_MAGIT_DIFF_HUNK_HEADING, "%.*s", (int) n, t);
		li->file = fi;
		li->hunk = (int) h;
		li->line = 0;
		if (hunk_collapsed (r->m, sec, f->path, t))
			continue;
		const char *p = t + n + (t[n] != '\0');
		for (int k = 1; *p != '\0'; k++) {
			size_t ln = strcspn (p, "\n");
			const char *face = p[0] == '+' ? FACE_MAGIT_DIFF_ADDED :
				p[0] == '-' ? FACE_MAGIT_DIFF_REMOVED : NULL;
			li = add_line (r, L_DIFF, sec, face, "%.*s", (int) ln, p);
			li->file = fi;
			li->hunk = (int) h;
			li->line = k;
			p += ln + (p[ln] != '\0');
		}
	}
}

static void
render_files (Render *r, enum Section sec, const FileList *fl)
{
	for (size_t i = 0; i < fl->n; i++) {
		const FileDiff *f = &fl->v[i];
		LineInfo *li;
		if (sec == SEC_UNTRACKED)
			li = add_line (r, L_FILE, sec, FACE_MAGIT_DIFF_FILE_HEADING, "%s", f->path);
		else if (f->orig_path != NULL)
			li = add_line (r, L_FILE, sec, FACE_MAGIT_DIFF_FILE_HEADING, "%-11s%s -> %s",
				       f->label, f->orig_path, f->path);
		else
			li = add_line (r, L_FILE, sec, FACE_MAGIT_DIFF_FILE_HEADING, "%-11s%s", f->label, f->path);
		li->file = (int) i;
		if (sec != SEC_UNTRACKED && f->has_diff && file_expanded (r->m, sec, f->path)) {
			if (f->binary && f->nhunks == 0)
				add_line (r, L_TEXT, sec, NULL, "Binary files differ")->file = (int) i;
			render_hunks (r, sec, (int) i, f);
		}
	}
}

static void
render_commits (Render *r, enum Section sec, const StrVec *sv, enum LineKind kind)
{
	for (size_t i = 0; i < sv->n; i++) {
		const char *s = sv->v[i];
		size_t hl = strcspn (s, " ");
		LineInfo *li = add_line (r, kind, sec, NULL, "%s", s);
		li->ref = xstrndup (s, hl);
		add_span (li, 0, hl, FACE_MAGIT_HASH);
	}
}

static void
render_heading (Render *r, enum Section sec, const char *title, size_t count, bool show_count)
{
	LineInfo *li = show_count
		? add_line (r, L_HEADING, sec, FACE_MAGIT_SECTION_HEADING, "%s (%zu)", title, count)
		: add_line (r, L_HEADING, sec, FACE_MAGIT_SECTION_HEADING, "%s", title);
	(void) li;
}

static void
render_status (Render *r)
{
	Magit *m = r->m;
	Status *st = &m->st;
	LineInfo *li;

	if (st->head != NULL) {
		size_t hl = strcspn (st->head, " ");
		if (st->branch != NULL) {
			li = add_line (r, L_TEXT, SEC_HEADER, NULL, "Head:     %s %s", st->branch, st->head);
			add_span (li, 10, strlen (st->branch), FACE_MAGIT_BRANCH_LOCAL);
			add_span (li, 11 + strlen (st->branch), hl, FACE_MAGIT_HASH);
		} else {
			li = add_line (r, L_TEXT, SEC_HEADER, NULL, "Head:     %s", st->head);
			add_span (li, 10, hl, FACE_MAGIT_HASH);
		}
		li->ref = xstrndup (st->head, hl);
	} else {
		li = add_line (r, L_TEXT, SEC_HEADER, NULL, "Head:     %s nothing committed yet",
			       st->branch != NULL ? st->branch : "HEAD");
		add_span (li, 10, st->branch != NULL ? strlen (st->branch) : 4, FACE_MAGIT_BRANCH_LOCAL);
	}
	if (st->upstream != NULL) {
		const char *uh = st->upstream_head != NULL ? st->upstream_head : "";
		li = add_line (r, L_TEXT, SEC_HEADER, NULL, "Merge:    %s %s", st->upstream, uh);
		add_span (li, 10, strlen (st->upstream), FACE_MAGIT_BRANCH_REMOTE);
		add_span (li, 11 + strlen (st->upstream), strcspn (uh, " "), FACE_MAGIT_HASH);
	}
	if (st->merging != NULL) {
		li = add_line (r, L_TEXT, SEC_HEADER, NULL, "Merging:  %s", st->merging);
		add_span (li, 10, strcspn (st->merging, " "), FACE_MAGIT_HASH);
	}
	add_line (r, L_TEXT, SEC_HEADER, NULL, "%s", "");

	struct {
		enum Section sec;
		const char *title;
		FileList *files;
		StrVec *commits;
	} sections[] = {
		{ SEC_UNTRACKED, "Untracked files", &st->untracked, NULL },
		{ SEC_UNMERGED, "Unmerged paths", &st->unmerged, NULL },
		{ SEC_UNSTAGED, "Unstaged changes", &st->unstaged, NULL },
		{ SEC_STAGED, "Staged changes", &st->staged, NULL },
		{ SEC_STASHES, "Stashes", NULL, &st->stashes },
		{ SEC_UNPULLED, NULL, NULL, &st->unpulled },
		{ SEC_UNPUSHED, NULL, NULL, &st->unpushed },
		{ SEC_RECENT, "Recent commits", NULL, &st->recent },
	};
	char *pulled = st->upstream != NULL ? xasprintf ("Unpulled from %s", st->upstream) : NULL;
	char *pushed = st->upstream != NULL ? xasprintf ("Unmerged into %s", st->upstream) : NULL;
	sections[5].title = pulled;
	sections[6].title = pushed;

	for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
		size_t n = sections[i].files != NULL ? sections[i].files->n : sections[i].commits->n;
		if (n == 0 || sections[i].title == NULL)
			continue;
		if (sections[i].sec == SEC_RECENT && st->upstream != NULL && st->unpushed.n > 0)
			continue;
		render_heading (r, sections[i].sec, sections[i].title, n, sections[i].sec != SEC_RECENT);
		if (!section_collapsed (m, sections[i].sec, n)) {
			if (sections[i].files != NULL)
				render_files (r, sections[i].sec, sections[i].files);
			else
				render_commits (r, sections[i].sec, sections[i].commits,
						sections[i].sec == SEC_STASHES ? L_STASH : L_COMMIT);
		}
		add_line (r, L_TEXT, sections[i].sec, NULL, "%s", "");
	}
	free (pulled);
	free (pushed);
}

static void
render_log (Render *r)
{
	Magit *m = r->m;
	for (size_t i = 0; i < m->log.n; i++) {
		const char *s = m->log.v[i];
		const char *f1 = strchr (s, '\x1f');
		if (f1 == NULL) {
			add_line (r, L_TEXT, SEC_LOG, NULL, "%s", s);
			continue;
		}
		/* GRAPH \x1f HASH \x1f REFS \x1f SUBJECT \x1f AUTHOR \x1f DATE */
		const char *fields[5] = { NULL };
		const char *p = f1 + 1;
		for (int k = 0; k < 5; k++) {
			fields[k] = p;
			const char *q = strchr (p, '\x1f');
			p = q != NULL ? q + 1 : p + strlen (p);
		}
		size_t lens[5];
		for (int k = 0; k < 5; k++) {
			const char *q = strchr (fields[k], '\x1f');
			lens[k] = q != NULL ? (size_t) (q - fields[k]) : strlen (fields[k]);
		}
		size_t gl = (size_t) (f1 - s);
		const char *refs = fields[1];
		size_t rl = lens[1];
		while (rl > 0 && *refs == ' ') {
			refs++;
			rl--;
		}
		LineInfo *li = add_line (r, L_COMMIT, SEC_LOG, NULL, "%.*s %.*s%s%.*s%s%.*s  %.*s, %.*s",
					 (int) lens[0], fields[0], (int) gl, s,
					 rl > 0 ? "" : "", (int) rl, refs, rl > 0 ? " " : "",
					 (int) lens[2], fields[2], (int) lens[3], fields[3], (int) lens[4], fields[4]);
		li->ref = xstrndup (fields[0], lens[0]);
		add_span (li, 0, lens[0], FACE_MAGIT_HASH);
		if (rl > 0)
			add_span (li, lens[0] + 1 + gl, rl, FACE_MAGIT_BRANCH_LOCAL);
		size_t tail = lens[0] + 1 + gl + rl + (rl > 0 ? 1 : 0) + lens[2] + 2;
		add_span (li, tail, lens[3] + 2 + lens[4], FACE_MAGIT_LOG_DATE);
	}
	if (m->log.n == 0)
		add_line (r, L_TEXT, SEC_LOG, NULL, "%s", "No commits");
}

static void
render_files_buffer (Render *r)
{
	Magit *m = r->m;
	for (size_t i = 0; i < m->header.n; i++) {
		LineInfo *li = add_line (r, L_TEXT, SEC_HEADER, NULL, "%s", m->header.v[i]);
		if (i == 0 && str_has_prefix (m->header.v[i], "commit "))
			add_span (li, 7, strlen (m->header.v[i]) - 7, FACE_MAGIT_HASH);
	}
	if (m->files.n == 0 && m->kind == MAGIT_DIFF)
		add_line (r, L_TEXT, SEC_FILES, NULL, "%s", "(empty diff)");
	render_files (r, SEC_FILES, &m->files);
}

/* Identifies a line, to find it again after a refresh. */
typedef struct {
	enum LineKind kind;
	enum Section sec;
	char *path;
	char *ref;
	int hunk, line;
	size_t line_no;
} Position;

static FileList *
section_files (Magit *m, enum Section sec)
{
	switch (sec) {
	case SEC_UNTRACKED: return &m->st.untracked;
	case SEC_UNMERGED: return &m->st.unmerged;
	case SEC_UNSTAGED: return &m->st.unstaged;
	case SEC_STAGED: return &m->st.staged;
	case SEC_FILES: return &m->files;
	default: return NULL;
	}
}

static const LineInfo *
line_at (Buffer *bp, size_t line_no)
{
	Magit *m = bp->magit;
	return m != NULL && line_no < m->nlines ? &m->lines[line_no] : NULL;
}

static const LineInfo *
current_line (void)
{
	return line_at (cur_bp, buffer_offset_to_line (cur_bp, buffer_pt (cur_bp)));
}

static FileDiff *
line_file (Magit *m, const LineInfo *li)
{
	FileList *fl = li != NULL ? section_files (m, li->sec) : NULL;
	return fl != NULL && li->file >= 0 && (size_t) li->file < fl->n ? &fl->v[li->file] : NULL;
}

static Position
save_position (Buffer *bp)
{
	Position pos;
	memset (&pos, 0, sizeof pos);
	pos.line_no = buffer_offset_to_line (bp, buffer_pt (bp));
	const LineInfo *li = line_at (bp, pos.line_no);
	if (li != NULL) {
		pos.kind = li->kind;
		pos.sec = li->sec;
		pos.hunk = li->hunk;
		pos.line = li->line;
		FileDiff *f = line_file (bp->magit, li);
		pos.path = f != NULL ? xstrdup (f->path) : NULL;
		pos.ref = xstrdup_or_null (li->ref);
	}
	return pos;
}

static void
restore_position (Buffer *bp, Position *pos)
{
	Magit *m = bp->magit;
	size_t best = MIN (pos->line_no, m->nlines > 0 ? m->nlines - 1 : 0);
	int best_score = 0;
	for (size_t i = 0; i < m->nlines; i++) {
		const LineInfo *li = &m->lines[i];
		int score = 0;
		FileDiff *f = line_file (m, li);
		if (pos->path != NULL && f != NULL && strcmp (f->path, pos->path) == 0) {
			score = li->sec == pos->sec ? 3 : 2;
			if (li->hunk == pos->hunk && li->line == pos->line && li->sec == pos->sec)
				score = 5;
			else if (li->kind == L_FILE && li->sec == pos->sec)
				score = 4;
		} else if (pos->ref != NULL && li->ref != NULL && strcmp (li->ref, pos->ref) == 0)
			score = 4;
		else if (pos->path == NULL && pos->ref == NULL && li->kind == pos->kind &&
			 li->sec == pos->sec && li->kind == L_HEADING)
			score = 4;
		else if (li->kind == L_HEADING && li->sec == pos->sec)
			score = 1;
		if (score > best_score) {
			best_score = score;
			best = i;
		}
	}
	buffer_goto_offset (bp, 0);
	buffer_move_line (bp, (long) best);
	buffer_goto_offset (bp, buffer_line_o (bp));
	free (pos->path);
	free (pos->ref);
}

/* Regenerate the buffer's text from its model. */
static void
redraw (Buffer *bp)
{
	Magit *m = bp->magit;
	Position pos = save_position (bp);
	clear_lines (m);
	Render r = { m, { NULL, 0, 0 } };
	sb_init (&r.text);
	switch (m->kind) {
	case MAGIT_STATUS:
		render_status (&r);
		break;
	case MAGIT_LOG:
		render_log (&r);
		break;
	default:
		render_files_buffer (&r);
		break;
	}

	bp->readonly = false;
	buffer_goto_offset (bp, 0);
	buffer_replace_estr (bp, buffer_length (bp), ev_of (r.text.s, r.text.len, eol_lf));
	bp->readonly = true;
	bp->modified = false;
	sb_free (&r.text);
	restore_position (bp, &pos);
	thisflag |= FLAG_NEED_RESYNC;
}


/*--------------------------------------------------------------------------
 * Refreshing.
 *--------------------------------------------------------------------------*/

/* Expanded files of a section, for which to fetch diffs. */
static void
expanded_paths (Magit *m, enum Section sec, const FileList *fl, StrVec *out)
{
	for (size_t i = 0; i < fl->n; i++)
		if (file_expanded (m, sec, fl->v[i].path))
			sv_push (out, fl->v[i].path);
}

/* Attach parsed diffs to the files of `fl' with the same paths. */
static void
attach_diffs (FileList *fl, FileList *diffs)
{
	Map *index = map_new ();
	for (size_t i = 0; i < fl->n; i++)
		map_set (index, fl->v[i].path, &fl->v[i]);
	for (size_t i = 0; i < diffs->n; i++) {
		FileDiff *d = &diffs->v[i];
		FileDiff *f = map_get (index, d->path);
		if (f == NULL && d->orig_path != NULL)
			f = map_get (index, d->orig_path);
		if (f == NULL)
			continue;
		const char *label = f->label;
		char *orig = f->orig_path;
		f->orig_path = NULL;
		file_diff_free (f);
		*f = *d;
		memset (d, 0, sizeof *d);
		f->label = label;
		if (f->orig_path == NULL)
			f->orig_path = orig;
		else
			free (orig);
	}
	map_free (index, NULL);
	file_list_clear (diffs);
}

/* Prepare a job fetching the diffs of `paths' (all if `all'). */
static void
setup_diff_job (Job *job, const Repo *repo, bool cached, const StrVec *paths, bool all, char ***argv_out)
{
	size_t n = all ? 0 : paths->n;
	char **argv = xmalloc ((n + 8) * sizeof (char *));
	size_t k = 0;
	argv[k++] = "diff";
	argv[k++] = "--no-ext-diff";
	argv[k++] = "--no-color";
	if (cached)
		argv[k++] = "--cached";
	argv[k++] = "--";
	for (size_t i = 0; i < n; i++)
		argv[k++] = paths->v[i];
	argv[k] = NULL;
	memset (job, 0, sizeof *job);
	job->fn = diff_job;
	job->repo = repo;
	/* Diff jobs take their arguments from `argv', which may be long. */
	job->argv = argv;
	*argv_out = argv;
}

enum { J_STATUS, J_HEAD, J_UPSTREAM_HEAD, J_UNPUSHED, J_UNPULLED, J_RECENT, J_STASHES, J_MERGING,
       J_UNSTAGED_DIFF, J_STAGED_DIFF, NJOBS };

/* Free what the jobs of a status refresh found. */
static void
free_status_results (Job *jobs, size_t njobs)
{
	for (size_t i = 0; i < njobs; i++) {
		void *res = jobs[i].result;
		if (res == NULL)
			continue;
		switch (i) {
		case J_STATUS:
			status_clear (res);
			break;
		case J_UNPUSHED:
		case J_UNPULLED:
		case J_RECENT:
		case J_STASHES:
			sv_clear (res);
			break;
		case J_HEAD:
		case J_UPSTREAM_HEAD:
		case J_MERGING:
			break;
		default:
			file_list_clear (res);
			break;
		}
		free (res);
	}
}

/* Refresh a status buffer; `all_diffs' fetches every file's diff.
   Returns false, leaving the buffer as it was, if the commands of a
   remote repository did not all run to completion. */
static bool
refresh_status (Buffer *bp, bool all_diffs)
{
	Magit *m = bp->magit;
	const Repo *repo = &m->repo;
	int64_t start = monotonic_ms ();

	StrVec unstaged_paths = { NULL, 0, 0 }, staged_paths = { NULL, 0, 0 };
	expanded_paths (m, SEC_UNSTAGED, &m->st.unstaged, &unstaged_paths);
	expanded_paths (m, SEC_UNMERGED, &m->st.unmerged, &unstaged_paths);
	expanded_paths (m, SEC_STAGED, &m->st.staged, &staged_paths);
	bool unstaged_all = all_diffs || unstaged_paths.n > MAX_PATHS_PER_DIFF;
	bool staged_all = all_diffs || staged_paths.n > MAX_PATHS_PER_DIFF;

	Job jobs[NJOBS];
	set_args (&jobs[J_STATUS], repo, status_job, (const char *[]) {
			"status", "--porcelain=v2", "-z", "--branch", "--untracked-files=normal", NULL });
	set_args (&jobs[J_HEAD], repo, string_job, (const char *[]) {
			"log", "-1", "--no-show-signature", "--format=%h %s", NULL });
	set_args (&jobs[J_UPSTREAM_HEAD], repo, string_job, (const char *[]) {
			"log", "-1", "--no-show-signature", "--format=%h %s", "@{upstream}", NULL });
	set_args (&jobs[J_UNPUSHED], repo, lines_job, (const char *[]) {
			"log", "--no-show-signature", "--format=%h %s", "-n", LOG_LIMIT, "@{upstream}..HEAD", NULL });
	set_args (&jobs[J_UNPULLED], repo, lines_job, (const char *[]) {
			"log", "--no-show-signature", "--format=%h %s", "-n", LOG_LIMIT, "HEAD..@{upstream}", NULL });
	set_args (&jobs[J_RECENT], repo, lines_job, (const char *[]) {
			"log", "--no-show-signature", "--format=%h %s", "-n", "10", NULL });
	set_args (&jobs[J_STASHES], repo, lines_job, (const char *[]) {
			"stash", "list", "--format=%gd %gs", NULL });
	set_args (&jobs[J_MERGING], repo, string_job, (const char *[]) {
			"log", "-1", "--no-show-signature", "--format=%h %s", "MERGE_HEAD", NULL });

	size_t njobs = J_UNSTAGED_DIFF;
	char **argv_unstaged = NULL, **argv_staged = NULL;
	if (unstaged_all || unstaged_paths.n > 0)
		setup_diff_job (&jobs[njobs++], repo, false, &unstaged_paths, unstaged_all, &argv_unstaged);
	size_t staged_job = njobs;
	if (staged_all || staged_paths.n > 0)
		setup_diff_job (&jobs[njobs++], repo, true, &staged_paths, staged_all, &argv_staged);

	run_jobs (jobs, njobs);
	free (argv_unstaged);
	free (argv_staged);

	bool failed = false;
	for (size_t i = 0; i < njobs; i++)
		failed = failed || jobs[i].status < 0;
	if (repo->remote != NULL && failed) {
		free_status_results (jobs, njobs);
		sv_clear (&unstaged_paths);
		sv_clear (&staged_paths);
		minibuf_error ("%s", remote_last_error ());
		return false;
	}

	status_clear (&m->st);
	Status *st = jobs[J_STATUS].result;
	m->st = *st;
	free (st);
	m->st.head = jobs[J_HEAD].result;
	m->st.upstream_head = jobs[J_UPSTREAM_HEAD].result;
	m->st.merging = jobs[J_MERGING].result;
	StrVec *lists[] = { &m->st.unpushed, &m->st.unpulled, &m->st.recent, &m->st.stashes };
	int list_jobs[] = { J_UNPUSHED, J_UNPULLED, J_RECENT, J_STASHES };
	for (size_t i = 0; i < 4; i++) {
		StrVec *sv = jobs[list_jobs[i]].result;
		*lists[i] = *sv;
		free (sv);
	}
	if (m->st.upstream == NULL) {
		sv_clear (&m->st.unpushed);
		sv_clear (&m->st.unpulled);
	}

	if (njobs > J_UNSTAGED_DIFF && (unstaged_all || unstaged_paths.n > 0)) {
		FileList *fl = jobs[J_UNSTAGED_DIFF].result;
		/* Unmerged files appear in the unstaged diff too. */
		FileList unmerged_part = { NULL, 0, 0 };
		for (size_t i = 0; i < fl->n; i++)
			if (strcmp (fl->v[i].label, "unmerged") == 0) {
				*file_list_add (&unmerged_part) = fl->v[i];
				memset (&fl->v[i], 0, sizeof fl->v[i]);
				fl->v[i].path = xstrdup ("");
				fl->v[i].label = "modified";
			}
		attach_diffs (&m->st.unmerged, &unmerged_part);
		attach_diffs (&m->st.unstaged, fl);
		free (fl);
	}
	if (staged_job < njobs) {
		FileList *fl = jobs[staged_job].result;
		attach_diffs (&m->st.staged, fl);
		free (fl);
	}
	if (all_diffs) {
		/* Show every file expanded. */
		FileList *lists2[] = { &m->st.unmerged, &m->st.unstaged, &m->st.staged };
		enum Section secs[] = { SEC_UNMERGED, SEC_UNSTAGED, SEC_STAGED };
		for (size_t k = 0; k < 3; k++) {
			m->collapsed[secs[k]] = false;
			m->collapsed_set[secs[k]] = true;
			for (size_t i = 0; i < lists2[k]->n; i++)
				if (!file_expanded (m, secs[k], lists2[k]->v[i].path)) {
					char *key = toggle_key (secs[k], lists2[k]->v[i].path, NULL);
					toggle (m, key);
					free (key);
				}
		}
	}

	sv_clear (&unstaged_paths);
	sv_clear (&staged_paths);
	redraw (bp);

	int64_t elapsed = monotonic_ms () - start;
	debug_log ("magit", "refresh %s: %lld ms, %zu unmerged, %zu unstaged, %zu staged, %zu untracked, %zu lines",
		   all_diffs ? "with all diffs" : "status", (long long) elapsed, m->st.unmerged.n,
		   m->st.unstaged.n, m->st.staged.n, m->st.untracked.n, m->nlines);
	if (elapsed > 1000)
		minibuf_write ("Refreshing magit buffer...done (%.2fs)", (double) elapsed / 1000.0);
	return true;
}

/* Fetch one file's diff, when it is expanded; false, saying why, if
   git did not run to completion on the repository's machine. */
static bool
load_file_diff (Magit *m, enum Section sec, FileDiff *f)
{
	if (f->has_diff || sec == SEC_UNTRACKED)
		return true;
	StrBuf out;
	sb_init (&out);
	const char *args[] = { "diff", "--no-ext-diff", "--no-color", sec == SEC_STAGED ? "--cached" : "--",
			       sec == SEC_STAGED ? "--" : f->path, sec == SEC_STAGED ? f->path : NULL, NULL };
	int status = git_args (&m->repo, args, NULL, 0, &out, NULL);
	if (remote_failed (&m->repo, status)) {
		sb_free (&out);
		return false;
	}
	if (status == 0) {
		FileList fl = { NULL, 0, 0 };
		parse_diff_parallel (out.s, out.len, &fl);
		FileList one = { NULL, 0, 0 };
		*file_list_add (&one) = *f;
		memset (f, 0, sizeof *f);
		attach_diffs (&one, &fl);
		*f = one.v[0];
		f->has_diff = true;
		free (one.v);
	}
	sb_free (&out);
	return true;
}

/* Refresh a Magit buffer; false, leaving it as it was, if git did not
   run to completion on the repository's machine. */
static bool
refresh_buffer (Buffer *bp)
{
	Magit *m = bp->magit;
	if (m == NULL)
		return true;
	switch (m->kind) {
	case MAGIT_STATUS:
		return refresh_status (bp, false);
	case MAGIT_LOG: {
		StrBuf out;
		sb_init (&out);
		const char *args[16] = { "log", "--graph", "--decorate=short", "--no-show-signature",
					 "--format=%x1f%h%x1f%d%x1f%s%x1f%an%x1f%ar", "-n", LOG_LIMIT };
		size_t k = 7;
		char *copy = m->args != NULL ? xstrdup (m->args) : NULL;
		for (char *tok = copy != NULL ? strtok (copy, " ") : NULL; tok != NULL && k < 15; tok = strtok (NULL, " "))
			args[k++] = tok;
		args[k] = NULL;
		int status = git_args (&m->repo, args, NULL, 0, &out, NULL);
		free (copy);
		if (remote_failed (&m->repo, status)) {
			sb_free (&out);
			return false;
		}
		sv_clear (&m->log);
		for (const char *p = out.s; *p != '\0';) {
			size_t n = strcspn (p, "\n");
			char *line = xstrndup (p, n);
			sv_push (&m->log, line);
			free (line);
			p += n + (p[n] != '\0');
		}
		sb_free (&out);
		redraw (bp);
		break;
	}
	case MAGIT_REVISION:
	case MAGIT_DIFF: {
		StrBuf out;
		sb_init (&out);
		const char *args[24];
		size_t k = 0;
		char *copy = xstrdup (m->args != NULL ? m->args : "");
		if (m->kind == MAGIT_REVISION) {
			bool stash = str_has_prefix (copy, "stash@");
			args[k++] = stash ? "stash" : "show";
			if (stash)
				args[k++] = "show";
			args[k++] = "-p";
			args[k++] = "--no-ext-diff";
			args[k++] = "--no-color";
			args[k++] = "--no-show-signature";
			if (!stash)
				args[k++] = "--format=commit %H%nAuthor:     %an <%ae>%nAuthorDate: %ad%nCommit:     %cn <%ce>%nCommitDate: %cd%n%n%w(0,4,4)%B";
		} else {
			args[k++] = "diff";
			args[k++] = "--no-ext-diff";
			args[k++] = "--no-color";
		}
		for (char *tok = strtok (copy, " "); tok != NULL && k < 22; tok = strtok (NULL, " "))
			args[k++] = tok;
		args[k] = NULL;
		int status = git_args (&m->repo, args, NULL, 0, &out, NULL);
		free (copy);
		if (remote_failed (&m->repo, status)) {
			sb_free (&out);
			return false;
		}

		file_list_clear (&m->files);
		sv_clear (&m->header);
		const char *diff = out.s;
		if (m->kind == MAGIT_REVISION) {
			const char *d = strstr (out.s, "\ndiff --");
			size_t hl = d != NULL ? (size_t) (d - out.s) : out.len;
			for (const char *p = out.s; p < out.s + hl;) {
				size_t n = strcspn (p, "\n");
				n = MIN (n, (size_t) (out.s + hl - p));
				char *line = xstrndup (p, n);
				sv_push (&m->header, line);
				free (line);
				p += n + 1;
			}
			sv_push (&m->header, "");
			diff = d != NULL ? d + 1 : out.s + out.len;
		}
		parse_diff_parallel (diff, (size_t) (out.s + out.len - diff), &m->files);
		sb_free (&out);
		redraw (bp);
		break;
	}
	default:
		break;
	}
	return true;
}

/* Refresh the status buffer of `repo', if there is one. */
static void
refresh_status_of (const Repo *repo)
{
	/* Git may have rewritten visited files. Remote buffers are
	   checked when saved instead. */
	if (repo->remote == NULL)
		buffers_check_disk (true);
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->magit != NULL && bp->magit->kind == MAGIT_STATUS && repo_eq (&bp->magit->repo, repo))
			refresh_status (bp, false);
}

static void
refresh_after_change (const Repo *repo)
{
	refresh_status_of (repo);
	if (cur_bp->magit != NULL && cur_bp->magit->kind != MAGIT_STATUS &&
	    cur_bp->magit->kind != MAGIT_PROCESS && cur_bp->magit->kind != MAGIT_COMMIT_MESSAGE &&
	    repo_eq (&cur_bp->magit->repo, repo))
		refresh_buffer (cur_bp);
}


/*--------------------------------------------------------------------------
 * Buffers.
 *--------------------------------------------------------------------------*/

/* Find the repository containing `dir' on `r' (NULL for this
   machine) and put it in `*repo'; false, saying why, if there is
   none. */
static bool
find_repo (Remote *r, const char *dir, Repo *repo)
{
	Repo where = { r, (char *) dir };
	int status;
	char *root = git_query_status (&where, (const char *[]) { "rev-parse", "--show-toplevel", NULL }, &status);
	if (root == NULL || root[0] == '\0') {
		free (root);
		if (!remote_failed (&where, status))
			minibuf_error ("Not inside a Git repository%s%s", r != NULL ? " on ssh:" : "",
				       r != NULL ? remote_label (r) : "");
		return false;
	}
	*repo = (Repo) { r, root };
	return true;
}

/* The repository of the current buffer: a Magit buffer's own, or the
   one containing the buffer's directory, on the buffer's machine. */
static bool
current_repo (Repo *repo)
{
	if (cur_bp->magit != NULL) {
		*repo = repo_copy (&cur_bp->magit->repo);
		return true;
	}
	if (cur_bp->remote != NULL) {
		char *dir = remote_buffer_dir (cur_bp);
		bool ok = find_repo (cur_bp->remote, dir, repo);
		free (dir);
		return ok;
	}
	return find_repo (NULL, cur_bp->dir, repo);
}

/* The Magit buffer of `kind' for `repo', made if there is none. */
static Buffer *
magit_buffer (const char *prefix, const Repo *repo, enum MagitKind kind, bool *created)
{
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->magit != NULL && bp->magit->kind == kind && repo_eq (&bp->magit->repo, repo)) {
			*created = false;
			return bp;
		}
	*created = true;
	char *base = path_basename (repo->root);
	char *name = xasprintf ("%s: %s", prefix, base);
	free (base);
	char *unique = buffer_unique_name (name);
	free (name);
	Buffer *bp = buffer_new (NULL);
	bp->name = unique;
	bp->nosave = true;
	bp->noundo = true;
	bp->readonly = true;
	bp->keymap = status_map;
	buffer_set_highlighter (bp, &magit_mode);
	bp->magit = xzalloc (sizeof (Magit));
	bp->magit->kind = kind;
	set_repo (bp, repo, true);
	static const char *const labels[] = { "Magit", "Magit Log", "Magit Rev", "Magit Diff" };
	bp->mode_label = xstrdup (labels[kind]);
	return bp;
}

/* Show a Magit buffer for `repo' and refresh it. If the refresh fails,
   a new buffer goes, and an old one is left as it was. */
static bool
show_magit_buffer (const char *prefix, const Repo *repo, enum MagitKind kind, const char *args)
{
	bool created;
	Buffer *bp = magit_buffer (prefix, repo, kind, &created);
	if (created && kind != MAGIT_STATUS)
		file_list_clear (&bp->magit->files);
	char *old_args = bp->magit->args;
	bp->magit->args = xstrdup_or_null (args);
	if (!refresh_buffer (bp)) {
		if (created)
			buffer_kill (bp);
		else {
			free (bp->magit->args);
			bp->magit->args = old_args;
		}
		return false;
	}
	free (old_args);
	if (created)
		buffer_goto_offset (bp, 0);
	buffer_switch_to (bp, true);
	return true;
}


/*--------------------------------------------------------------------------
 * Section commands.
 *--------------------------------------------------------------------------*/

static bool
check_magit (void)
{
	if (cur_bp->magit == NULL || cur_bp->magit->kind == MAGIT_COMMIT_MESSAGE) {
		minibuf_error ("Not in a Magit buffer");
		return false;
	}
	return true;
}

/* Without a directory, the repository of the current buffer. A
   directory is on the current buffer's machine, and a relative one is
   relative to the buffer's directory there. */
static bool
F_magit_status (long uniarg, Args *args)
{
	(void) uniarg;
	char *dir = xstrdup_or_null (args_poll (args));
	Repo repo;
	bool found;
	if (dir == NULL)
		found = current_repo (&repo);
	else {
		Remote *r = cur_bp->remote;
		char *start = r != NULL ? remote_buffer_dir (cur_bp) : xstrdup (cur_bp->dir);
		char *where = dir[0] == '/' || dir[0] == '~' ? xstrdup (dir) : path_join (start, dir);
		char *full = r == NULL ? expand_path (where) : NULL;
		found = find_repo (r, full != NULL ? full : where, &repo);
		free (full);
		free (where);
		free (start);
	}
	free (dir);
	if (!found)
		return false;
	bool ok = show_magit_buffer ("magit", &repo, MAGIT_STATUS, NULL);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_refresh (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit ())
		return false;
	return refresh_buffer (cur_bp);
}

static bool
F_magit_section_toggle (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit ())
		return false;
	Magit *m = cur_bp->magit;
	const LineInfo *li = current_line ();
	if (li == NULL)
		return false;

	if (li->kind == L_HEADING) {
		m->collapsed[li->sec] = !m->collapsed[li->sec];
		m->collapsed_set[li->sec] = true;
	} else if (li->kind == L_FILE) {
		FileDiff *f = line_file (m, li);
		if (f == NULL || li->sec == SEC_UNTRACKED || !load_file_diff (m, li->sec, f))
			return false;
		char *key = toggle_key (li->sec, f->path, NULL);
		toggle (m, key);
		free (key);
	} else if (li->kind == L_HUNK || li->kind == L_DIFF) {
		FileDiff *f = line_file (m, li);
		if (f == NULL || li->hunk < 0)
			return false;
		char *key = toggle_key (li->sec, f->path, f->hunks[li->hunk].text);
		toggle (m, key);
		free (key);
		/* Leave point on the hunk's header. */
		size_t line_no = buffer_offset_to_line (cur_bp, buffer_pt (cur_bp));
		while (line_no > 0 && m->lines[line_no].kind != L_HUNK)
			line_no--;
		buffer_goto_offset (cur_bp, 0);
		buffer_move_line (cur_bp, (long) line_no);
	} else
		return false;
	redraw (cur_bp);
	return true;
}

/* Show every section with all diffs (4), or collapse all files (1). */
static bool
F_magit_show_level_4 (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit () || cur_bp->magit->kind != MAGIT_STATUS)
		return false;
	return refresh_status (cur_bp, true);
}

static bool
F_magit_show_level_1 (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit ())
		return false;
	Magit *m = cur_bp->magit;
	map_free (m->toggled, NULL);
	m->toggled = NULL;
	if (m->kind != MAGIT_STATUS) {
		/* Elsewhere files are expanded by default: collapse them all. */
		for (size_t i = 0; i < m->files.n; i++) {
			char *key = toggle_key (SEC_FILES, m->files.v[i].path, NULL);
			toggle (m, key);
			free (key);
		}
	}
	redraw (cur_bp);
	return true;
}

static bool
move_section (long n)
{
	if (!check_magit ())
		return false;
	Magit *m = cur_bp->magit;
	size_t line_no = buffer_offset_to_line (cur_bp, buffer_pt (cur_bp));
	bool forward = n >= 0;
	for (long k = 0; k < (n < 0 ? -n : n); k++) {
		size_t i = line_no;
		for (;;) {
			if (forward ? i + 1 >= m->nlines : i == 0)
				return false;
			i = forward ? i + 1 : i - 1;
			enum LineKind kind = m->lines[i].kind;
			if (kind == L_HEADING || kind == L_FILE || kind == L_HUNK || kind == L_COMMIT || kind == L_STASH)
				break;
		}
		line_no = i;
	}
	buffer_goto_offset (cur_bp, 0);
	buffer_move_line (cur_bp, (long) line_no);
	buffer_goto_offset (cur_bp, buffer_line_o (cur_bp));
	return true;
}

static bool
F_magit_section_forward (long uniarg, Args *args)
{
	(void) args;
	return move_section (uniarg);
}

static bool
F_magit_section_backward (long uniarg, Args *args)
{
	(void) args;
	return move_section (-uniarg);
}

/* The new-side line number of line `k' of a hunk. */
static long
hunk_target_line (const Hunk *h, int k)
{
	const char *plus = strchr (h->text, '+');
	long line = plus != NULL ? strtol (plus + 1, NULL, 10) : 1;
	const char *p = h->text + strcspn (h->text, "\n") + 1;
	for (int i = 1; i < k && *p != '\0'; i++) {
		if (*p != '-' && *p != '\\')
			line++;
		p += strcspn (p, "\n") + 1;
	}
	return MAX (line, 1);
}

static bool
show_revision (const Repo *repo, const char *rev)
{
	return show_magit_buffer ("magit-revision", repo, MAGIT_REVISION, rev);
}

/* Visit `path' (absolute) of the repository of `m', a file or a
   directory, on the repository's machine. */
static bool
visit_repo_file (Magit *m, const char *path, const char *shown)
{
	Remote *r = m->repo.remote;
	if (r == NULL) {
		if (!file_exists (path)) {
			minibuf_error ("File `%s' does not exist", shown);
			return false;
		}
		return file_is_dir (path) ? dired (path) : find_file (path);
	}
	int status = remote_run (r, m->repo.root, (const char *[]) { "test", "-e", path, NULL }, NULL, 0,
				 NULL, NULL, "Checking a file");
	if (status < 0) {
		minibuf_error ("%s", remote_last_error ());
		return false;
	}
	if (status != 0) {
		minibuf_error ("File `%s' does not exist on ssh:%s", shown, remote_label (r));
		return false;
	}
	return find_file_remote (r, path);
}

static bool
F_magit_visit_thing (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit ())
		return false;
	Magit *m = cur_bp->magit;
	const LineInfo *li = current_line ();
	if (li == NULL)
		return false;

	if ((li->kind == L_COMMIT || li->kind == L_STASH || li->kind == L_TEXT) && li->ref != NULL) {
		Repo repo = repo_copy (&m->repo);
		char *ref = xstrdup (li->ref);
		bool ok = show_revision (&repo, ref);
		repo_clear (&repo);
		free (ref);
		return ok;
	}
	if (li->kind == L_HEADING)
		return F_magit_section_toggle (uniarg, args);

	FileDiff *f = line_file (m, li);
	if (f == NULL)
		return false;
	char *path = path_join (m->repo.root, f->path);
	long line = 1;
	if ((li->kind == L_HUNK || li->kind == L_DIFF) && li->hunk >= 0)
		line = hunk_target_line (&f->hunks[li->hunk], li->line);
	bool ok = visit_repo_file (m, path, f->path);
	free (path);
	if (ok && cur_bp->dired == NULL) {
		buffer_goto_offset (cur_bp, 0);
		buffer_move_line (cur_bp, line - 1);
		buffer_goto_offset (cur_bp, buffer_line_o (cur_bp));
	}
	return ok;
}


/*--------------------------------------------------------------------------
 * Staging, unstaging and discarding.
 *--------------------------------------------------------------------------*/

/* The patch for hunk `h' of `f'. */
static char *
hunk_patch (const FileDiff *f, const Hunk *h)
{
	return xstrcat3 (f->head != NULL ? f->head : "", h->text, "");
}

/* The files selected: those touched by the region, if active, else the
   one at point. Returns their section, or NSECTIONS if none. */
static enum Section
selected_files (Magit *m, StrVec *paths)
{
	size_t first, last;
	first = last = buffer_offset_to_line (cur_bp, buffer_pt (cur_bp));
	if (cur_bp->mark_active && cur_bp->mark != NULL) {
		size_t other = buffer_offset_to_line (cur_bp, cur_bp->mark->o);
		first = MIN (first, other);
		last = MAX (last, other);
	}
	enum Section sec = NSECTIONS;
	for (size_t i = first; i <= last && i < m->nlines; i++) {
		const LineInfo *li = &m->lines[i];
		FileDiff *f = line_file (m, li);
		if (f == NULL || (li->kind != L_FILE && first != last))
			continue;
		if (sec == NSECTIONS)
			sec = li->sec;
		if (li->sec == sec && (paths->n == 0 || strcmp (paths->v[paths->n - 1], f->path) != 0))
			sv_push (paths, f->path);
	}
	return sec;
}

/* Run `git ARGS... -- PATHS...', in batches to stay within the
   argument length limit, for commands without --pathspec-from-file. */
static bool
git_on_path_args (const Repo *repo, const char *const *args, const StrVec *paths)
{
	enum { BATCH = 256 };
	size_t na = 0;
	while (args[na] != NULL)
		na++;
	const char **argv = xmalloc ((na + BATCH + 2) * sizeof (char *));
	bool ok = true;
	for (size_t i = 0; ok && i < paths->n; i += BATCH) {
		size_t k = 0;
		for (; k < na; k++)
			argv[k] = args[k];
		argv[k++] = "--";
		for (size_t j = i; j < paths->n && j < i + BATCH; j++)
			argv[k++] = paths->v[j];
		argv[k] = NULL;
		ok = git_run (repo, argv);
	}
	free (argv);
	return ok;
}

/* Run `git ARGS... --pathspec-from-file=- --pathspec-file-nul' on
   `paths', which are passed on standard input however many there are.
   `args' must not end with "--". */
static bool
git_on_paths (const Repo *repo, const char *const *args, const StrVec *paths)
{
	const char *argv[16];
	size_t k = 0;
	for (; args[k] != NULL && k < 13; k++)
		argv[k] = args[k];
	argv[k++] = "--pathspec-from-file=-";
	argv[k++] = "--pathspec-file-nul";
	argv[k] = NULL;
	StrBuf in;
	sb_init (&in);
	for (size_t i = 0; i < paths->n; i++)
		sb_addn (&in, paths->v[i], strlen (paths->v[i]) + 1);
	bool ok = git_run_input (repo, argv, in.s, in.len, NULL);
	sb_free (&in);
	return ok;
}

static void
section_paths (const FileList *fl, StrVec *paths)
{
	for (size_t i = 0; i < fl->n; i++)
		sv_push (paths, fl->v[i].path);
}

static bool
apply_hunk (Magit *m, const LineInfo *li, const char *const *args)
{
	FileDiff *f = line_file (m, li);
	if (f == NULL || li->hunk < 0)
		return false;
	char *patch = hunk_patch (f, &f->hunks[li->hunk]);
	bool ok = git_run_input (&m->repo, args, patch, strlen (patch), NULL);
	free (patch);
	return ok;
}

static bool
F_magit_stage (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit () || cur_bp->magit->kind != MAGIT_STATUS)
		return false;
	Magit *m = cur_bp->magit;
	Repo repo = repo_copy (&m->repo);
	const LineInfo *li = current_line ();
	bool ok = false;

	if (li != NULL && (li->kind == L_HUNK || li->kind == L_DIFF) && li->sec == SEC_UNSTAGED)
		ok = apply_hunk (m, li, (const char *[]) { "apply", "--cached", "-", NULL });
	else if (li != NULL && li->kind == L_HEADING) {
		StrVec paths = { NULL, 0, 0 };
		if (li->sec == SEC_UNSTAGED)
			ok = git_run (&repo, (const char *[]) { "add", "-u", NULL });
		else if (li->sec == SEC_UNTRACKED || li->sec == SEC_UNMERGED) {
			section_paths (section_files (m, li->sec), &paths);
			ok = git_on_paths (&repo, (const char *[]) { "add", "-A", NULL }, &paths);
		} else
			minibuf_error ("Nothing to stage here");
		sv_clear (&paths);
	} else {
		StrVec paths = { NULL, 0, 0 };
		enum Section sec = selected_files (m, &paths);
		if (sec == SEC_STAGED)
			minibuf_error ("Already staged");
		else if (paths.n == 0)
			minibuf_error ("Nothing to stage here");
		else
			ok = git_on_paths (&repo, (const char *[]) { "add", "-A", NULL }, &paths);
		sv_clear (&paths);
	}
	cur_bp->mark_active = false;
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_stage_modified (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = git_run (&repo, (const char *[]) { "add", "-u", NULL });
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
unstage_all (Magit *m, const Repo *repo)
{
	if (m->st.oid == NULL)
		return git_run (repo, (const char *[]) { "rm", "--cached", "-r", "-q", "--", ".", NULL });
	return git_run (repo, (const char *[]) { "reset", "-q", "--", ".", NULL });
}

static bool
F_magit_unstage (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit () || cur_bp->magit->kind != MAGIT_STATUS)
		return false;
	Magit *m = cur_bp->magit;
	Repo repo = repo_copy (&m->repo);
	const LineInfo *li = current_line ();
	bool ok = false;

	if (li != NULL && (li->kind == L_HUNK || li->kind == L_DIFF) && li->sec == SEC_STAGED)
		ok = apply_hunk (m, li, (const char *[]) { "apply", "--cached", "--reverse", "-", NULL });
	else if (li != NULL && li->kind == L_HEADING && li->sec == SEC_STAGED)
		ok = unstage_all (m, &repo);
	else {
		StrVec paths = { NULL, 0, 0 };
		enum Section sec = selected_files (m, &paths);
		if (sec != SEC_STAGED || paths.n == 0)
			minibuf_error ("Nothing to unstage here");
		else if (m->st.oid == NULL)
			ok = git_on_paths (&repo, (const char *[]) { "rm", "--cached", "-r", "-q", NULL }, &paths);
		else
			ok = git_on_paths (&repo, (const char *[]) { "reset", "-q", NULL }, &paths);
		sv_clear (&paths);
	}
	cur_bp->mark_active = false;
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_unstage_all (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit () || cur_bp->magit->kind != MAGIT_STATUS)
		return false;
	Repo repo = repo_copy (&cur_bp->magit->repo);
	bool ok = unstage_all (cur_bp->magit, &repo);
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_discard (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!check_magit () || cur_bp->magit->kind != MAGIT_STATUS)
		return false;
	Magit *m = cur_bp->magit;
	Repo repo = repo_copy (&m->repo);
	const LineInfo *li = current_line ();
	bool ok = false;

	if (li != NULL && (li->kind == L_HUNK || li->kind == L_DIFF) &&
	    (li->sec == SEC_UNSTAGED || li->sec == SEC_STAGED)) {
		if (minibuf_read_y_or_n ("Discard hunk? ") == 1)
			ok = apply_hunk (m, li, li->sec == SEC_UNSTAGED
					 ? (const char *[]) { "apply", "--reverse", "-", NULL }
					 : (const char *[]) { "apply", "--reverse", "--index", "-", NULL });
	} else if (li != NULL && li->kind == L_HEADING) {
		if (li->sec == SEC_UNTRACKED) {
			if (minibuf_read_y_or_n ("Delete all %zu untracked files? ", m->st.untracked.n) == 1) {
				StrVec paths = { NULL, 0, 0 };
				section_paths (&m->st.untracked, &paths);
				ok = git_on_path_args (&repo, (const char *[]) { "clean", "-f", "-d", "-q", NULL }, &paths);
				sv_clear (&paths);
			}
		} else if (li->sec == SEC_UNSTAGED) {
			if (minibuf_read_y_or_n ("Discard all unstaged changes? ") == 1)
				ok = git_run (&repo, (const char *[]) { "checkout", "--", ".", NULL });
		} else
			minibuf_error ("Cannot discard this section");
	} else {
		StrVec paths = { NULL, 0, 0 };
		enum Section sec = selected_files (m, &paths);
		const char *what = paths.n == 1 ? paths.v[0] : NULL;
		int ans = paths.n == 0 ? 0 : what != NULL
			? minibuf_read_y_or_n ("%s %s? ", sec == SEC_UNTRACKED ? "Delete" : "Discard changes to", what)
			: minibuf_read_y_or_n ("%s %zu files? ", sec == SEC_UNTRACKED ? "Delete" : "Discard changes to", paths.n);
		if (paths.n == 0)
			minibuf_error ("Nothing to discard here");
		else if (ans == 1) {
			if (sec == SEC_UNTRACKED)
				ok = git_on_path_args (&repo, (const char *[]) { "clean", "-f", "-d", "-q", NULL }, &paths);
			else if (sec == SEC_UNSTAGED)
				ok = git_on_paths (&repo, (const char *[]) { "checkout", NULL }, &paths);
			else if (sec == SEC_STAGED) {
				/* New files go; others return to HEAD. */
				StrVec added = { NULL, 0, 0 }, existing = { NULL, 0, 0 };
				for (size_t i = 0; i < paths.n; i++)
					for (size_t j = 0; j < m->st.staged.n; j++)
						if (strcmp (m->st.staged.v[j].path, paths.v[i]) == 0) {
							sv_push (strcmp (m->st.staged.v[j].label, "new file") == 0 ? &added : &existing,
								 paths.v[i]);
							break;
						}
				ok = true;
				if (added.n > 0)
					ok = git_on_paths (&repo, (const char *[]) { "rm", "-f", "-q", NULL }, &added);
				if (ok && existing.n > 0)
					ok = git_on_paths (&repo, (const char *[]) { "checkout", "HEAD", NULL }, &existing);
				sv_clear (&added);
				sv_clear (&existing);
			} else
				minibuf_error ("Cannot discard unmerged paths; resolve them instead");
		}
		sv_clear (&paths);
	}
	cur_bp->mark_active = false;
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}


/*--------------------------------------------------------------------------
 * Popups.
 *--------------------------------------------------------------------------*/

typedef struct {
	char key;
	const char *desc;
} Action;

/* Show a popup of actions in the minibuffer and read the choice. */
static char
transient (const char *title, const Action *actions, size_t n)
{
	/* Lay the actions out in as many columns as fit. */
	size_t width = 0;
	for (size_t i = 0; i < n; i++)
		width = MAX (width, strlen (actions[i].desc) + 4);
	size_t columns = MAX ((term_width () - 1) / width, 1);
	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, title);
	for (size_t i = 0; i < n; i++) {
		if (i % columns == 0)
			sb_add (&sb, "\n");
		char *item = xasprintf (" %c %s", actions[i].key, actions[i].desc);
		sb_addf (&sb, "%-*s", (int) width, item);
		free (item);
	}
	minibuf_write ("%s", sb.s);
	sb_free (&sb);
	Keystroke key = getkey (GETKEY_DEFAULT);
	minibuf_clear ();
	if (key == KBD_CANCEL) {
		funcall ("keyboard-quit");
		return 0;
	}
	for (size_t i = 0; i < n; i++)
		if (key == (Keystroke) (unsigned char) actions[i].key)
			return actions[i].key;
	char *desc = chordtodesc (key);
	minibuf_error ("%s is undefined in this popup", desc);
	free (desc);
	return 0;
}

#define ACTIONS(a) a, sizeof a / sizeof a[0]

static char *
read_ref (const Repo *repo, const char *prompt, const char *def, bool remotes)
{
	const char *args[] = { "for-each-ref", "--format=%(refname:short)", "refs/heads",
			       remotes ? "refs/remotes" : NULL, NULL };
	int status;
	char *refs = git_query_status (repo, args, &status);
	if (remote_failed (repo, status))
		return NULL;
	Completion *cp = completion_new (false);
	for (char *p = refs, *tok; p != NULL && (tok = strsep (&p, "\n")) != NULL;)
		if (*tok != '\0')
			sv_add_sorted (&cp->completions, tok);
	free (refs);
	char *s = def != NULL
		? minibuf_read_completion ("%s (default %s): ", "", cp, NULL, prompt, def)
		: minibuf_read_completion ("%s: ", "", cp, NULL, prompt);
	completion_free (cp);
	if (s == NULL) {
		funcall ("keyboard-quit");
		return NULL;
	}
	if (s[0] == '\0') {
		free (s);
		return def != NULL ? xstrdup (def) : NULL;
	}
	return s;
}

static char *
read_string (const char *prompt, const char *initial)
{
	char *s = minibuf_read ("%s", initial != NULL ? initial : "", prompt);
	if (s == NULL)
		funcall ("keyboard-quit");
	return s;
}

/* The commit or stash at point, if any. A copy is returned, as a
   refresh may free the line's information. */
static const char *
ref_at_point (void)
{
	static char ref[256];
	const LineInfo *li = cur_bp->magit != NULL ? current_line () : NULL;
	if (li == NULL || li->ref == NULL)
		return NULL;
	snprintf (ref, sizeof ref, "%s", li->ref);
	return ref;
}

/* The current branch of `repo' goes to `*branch' (NULL if detached);
   false, saying why, if git did not run to completion there. */
static bool
current_branch (const Repo *repo, char **branch)
{
	int status;
	*branch = git_query_status (repo, (const char *[]) { "symbolic-ref", "--short", "-q", "HEAD", NULL }, &status);
	return !remote_failed (repo, status);
}


/* Running long commands asynchronously. */

typedef struct {
	Repo repo;
	char *what;
} AsyncGit;

static void
async_output (Process *p, const char *data, size_t len, void *owner)
{
	(void) p;
	AsyncGit *a = owner;
	Buffer *bp = process_buffer (&a->repo);
	/* Show progress meters' last state rather than every update. */
	StrBuf sb;
	sb_init (&sb);
	for (size_t i = 0; i < len; i++)
		if (data[i] == '\r')
			sb_addc (&sb, '\n');
		else
			sb_addc (&sb, data[i]);
	append_text (bp, sb.s, sb.len);
	sb_free (&sb);
}

static void
async_exit (Process *p, int status, void *owner)
{
	AsyncGit *a = owner;
	int code = WIFEXITED (status) ? WEXITSTATUS (status) : 128;
	char *msg = xasprintf ("%3d (exit status)\n\n", code);
	append_text (process_buffer (&a->repo), msg, strlen (msg));
	free (msg);
	if (code == 0)
		minibuf_write ("Git %s...done", a->what);
	else
		minibuf_error ("Git %s failed (see $ for details)", a->what);
	refresh_status_of (&a->repo);
	repo_clear (&a->repo);
	free (a->what);
	free (a);
	process_free (p);
}

static bool
git_async (const Repo *repo, const char *what, const char *const *args)
{
	size_t n = 0, ne = sizeof git_env / sizeof git_env[0] - 1;
	while (args[n] != NULL)
		n++;
	const char **git = xmalloc ((n + ne + 5) * sizeof (char *));
	size_t k = 0;
	if (repo->remote != NULL) {
		git[k++] = "env";
		for (size_t i = 0; i < ne; i++)
			git[k++] = git_env[i];
		git[k++] = "git";
	} else {
		git[k++] = "git";
		git[k++] = "-C";
		git[k++] = repo->root;
	}
	git[k++] = "--no-pager";
	memcpy (git + k, args, (n + 1) * sizeof (char *));

	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, "run git");
	for (size_t i = 0; i < n; i++)
		sb_addf (&sb, " %s", args[i]);
	sb_addc (&sb, '\n');
	append_text (process_buffer (repo), sb.s, sb.len);
	sb_free (&sb);

	/* On another machine, ssh runs git there as a process of its own,
	   as for a terminal: remote-timeout does not bound it. */
	StrVec keep = { NULL, 0, 0 };
	const char **argv = repo->remote != NULL ? remote_command_argv (repo->remote, repo->root, git, &keep) : git;
	AsyncGit *a = xzalloc (sizeof (AsyncGit));
	a->repo = repo_copy (repo);
	a->what = xstrdup (what);
	Process *p = repo->remote != NULL
		? process_start (argv, NULL, NULL, false, 0, 0, NULL, async_output, async_exit, a)
		: process_start (argv, NULL, repo->root, false, 0, 0, git_env, async_output, async_exit, a);
	if (argv != git)
		free (argv);
	free (git);
	sv_clear (&keep);
	if (p == NULL) {
		minibuf_error (repo->remote != NULL ? "Cannot run ssh" : "Cannot run git");
		repo_clear (&a->repo);
		free (a->what);
		free (a);
		return false;
	}
	minibuf_write ("Running git %s...", what);
	return true;
}


/* Committing. */

static void
init_commit_map (void);

static bool
start_commit (const Repo *repo, bool amend, bool reword)
{
	bool all = false;
	if (!amend) {
		/* Exit status 1 if something is staged. */
		int staged = git_args (repo, (const char *[]) { "diff", "--cached", "--quiet", NULL }, NULL, 0, NULL, NULL);
		if (remote_failed (repo, staged))
			return false;
		if (staged != 1) {
			int ans = minibuf_read_y_or_n ("Nothing staged.  Commit all uncommitted changes? ");
			if (ans != 1) {
				if (ans == 0)
					minibuf_error ("Nothing staged");
				return false;
			}
			all = true;
		}
	}

	int status = 0;
	char *message = amend
		? git_query_status (repo, (const char *[]) { "log", "-1", "--format=%B", NULL }, &status)
		: NULL;
	char *branch = NULL;
	if (remote_failed (repo, status) || !current_branch (repo, &branch)) {
		free (message);
		return false;
	}
	StrBuf text;
	sb_init (&text);
	sb_add (&text, message != NULL ? message : "");
	sb_add (&text, "\n\n# Please enter the commit message for your changes. Lines starting\n"
		"# with '#' will be ignored, and an empty message aborts the commit.\n#\n");
	sb_addf (&text, "# On branch %s\n#\n# C-c C-c to commit, C-c C-k to abort.\n",
		 branch != NULL ? branch : "(detached HEAD)");
	free (message);
	free (branch);

	init_commit_map ();
	char *name = buffer_unique_name ("COMMIT_EDITMSG");
	Buffer *bp = buffer_new (NULL);
	bp->name = name;
	bp->nosave = true;
	bp->keymap = commit_map;
	buffer_set_highlighter (bp, &commit_mode);
	bp->mode_label = xstrdup ("Git-Commit");
	bp->magit = xzalloc (sizeof (Magit));
	bp->magit->kind = MAGIT_COMMIT_MESSAGE;
	/* Not a remote buffer, even for a remote repository: it has no
	   file there to be saved to. */
	set_repo (bp, repo, false);
	bp->magit->amend = amend;
	bp->magit->reword = reword;
	bp->magit->all = all;
	buffer_insert_estr (bp, ev_of (text.s, text.len, eol_lf));
	sb_free (&text);
	bp->modified = false;

	/* Show what will be committed in the other window. */
	char *diff_args = xstrdup (all ? "HEAD" : amend ? "--cached HEAD^" : "--cached");
	bool shown = show_magit_buffer ("magit-diff", repo, MAGIT_DIFF, diff_args);
	free (diff_args);
	if (!shown) {
		buffer_kill (bp);
		return false;
	}
	Buffer *diff_bp = cur_bp;
	buffer_switch_to (bp, true);
	buffer_goto_offset (bp, 0);
	Window *other = popup_window ();
	window_show_buffer (other, diff_bp, 0);
	return true;
}

static bool
F_git_commit_commit (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Magit *m = cur_bp->magit;
	if (m == NULL || m->kind != MAGIT_COMMIT_MESSAGE)
		return false;

	EstrView pre = buffer_pre_point (cur_bp), post = buffer_post_point (cur_bp);
	StrBuf msg;
	sb_init (&msg);
	sb_addn (&msg, pre.text, pre.len);
	sb_addn (&msg, post.text, post.len);

	const char *argv[8];
	size_t k = 0;
	argv[k++] = "commit";
	argv[k++] = "--cleanup=strip";
	argv[k++] = "-F";
	argv[k++] = "-";
	if (m->amend)
		argv[k++] = "--amend";
	if (m->reword)
		argv[k++] = "--only";
	if (m->all)
		argv[k++] = "--all";
	argv[k] = NULL;

	Repo repo = repo_copy (&m->repo);
	StrBuf out;
	sb_init (&out);
	bool ok = git_run_input (&repo, argv, msg.s, msg.len, &out);
	sb_free (&msg);
	if (ok) {
		Buffer *bp = cur_bp;
		size_t n = strcspn (out.s, "\n");
		/* Return to the status buffer. */
		Buffer *status = NULL;
		for (Buffer *b = head_bp; b != NULL; b = b->next)
			if (b->magit != NULL && b->magit->kind == MAGIT_STATUS && repo_eq (&b->magit->repo, &repo))
				status = b;
		if (status != NULL)
			buffer_switch_to (status, true);
		buffer_kill (bp);
		refresh_status_of (&repo);
		minibuf_write ("%.*s", (int) n, out.s);
	}
	sb_free (&out);
	repo_clear (&repo);
	return ok;
}

static bool
F_git_commit_abort (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Magit *m = cur_bp->magit;
	if (m == NULL || m->kind != MAGIT_COMMIT_MESSAGE)
		return false;
	Buffer *bp = cur_bp;
	Repo repo = repo_copy (&m->repo);
	for (Buffer *b = head_bp; b != NULL; b = b->next)
		if (b->magit != NULL && b->magit->kind == MAGIT_STATUS && repo_eq (&b->magit->repo, &repo)) {
			buffer_switch_to (b, true);
			break;
		}
	buffer_kill (bp);
	repo_clear (&repo);
	minibuf_write ("Commit aborted");
	return true;
}

static bool
F_magit_commit (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'c', "Commit" }, { 'a', "Amend" }, { 'e', "Extend" }, { 'w', "Reword" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	switch (transient ("Create", ACTIONS (actions))) {
	case 'c':
		ok = start_commit (&repo, false, false);
		break;
	case 'a':
		ok = start_commit (&repo, true, false);
		break;
	case 'w':
		ok = start_commit (&repo, true, true);
		break;
	case 'e':
		ok = git_run (&repo, (const char *[]) { "commit", "--amend", "--no-edit", NULL });
		refresh_status_of (&repo);
		break;
	}
	repo_clear (&repo);
	return ok;
}


/* Remotes. */

static bool
remote_command (const char *verb, const char *what)
{
	static const Action actions[] = {
		{ 'p', "upstream (default)" }, { 'u', "upstream, setting it" }, { 'e', "elsewhere" }, { 'a', "all remotes" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	char *title = xasprintf ("%s", what);
	char key = transient (title, actions, strcmp (verb, "fetch") == 0 ? 4 : 3);
	free (title);
	bool ok = false;
	char *branch;
	if (!current_branch (&repo, &branch))
		key = 0;
	switch (key) {
	case 'p':
		if (strcmp (verb, "push") == 0) {
			int status;
			char *upstream = git_query_status (&repo, (const char *[]) {
					"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}", NULL }, &status);
			if (remote_failed (&repo, status))
				break;
			ok = upstream != NULL
				? git_async (&repo, verb, (const char *[]) { "push", "-v", NULL })
				: git_async (&repo, verb, (const char *[]) { "push", "-v", "-u", "origin", "HEAD", NULL });
			free (upstream);
		} else
			ok = git_async (&repo, verb, (const char *[]) { verb, "-v", NULL });
		break;
	case 'u': {
		char *remote = read_string ("Remote: ", "origin");
		if (remote != NULL)
			ok = strcmp (verb, "push") == 0
				? git_async (&repo, verb, (const char *[]) { "push", "-v", "-u", remote, "HEAD", NULL })
				: git_async (&repo, verb, (const char *[]) { verb, "-v", remote, branch != NULL ? branch : "HEAD", NULL });
		free (remote);
		break;
	}
	case 'e': {
		char *where = read_string ("Remote and branch (e.g. origin main): ", "origin ");
		if (where != NULL) {
			char *remote = strtok (where, " "), *ref = strtok (NULL, " ");
			if (remote != NULL)
				ok = git_async (&repo, verb, (const char *[]) { verb, "-v", remote, ref, NULL });
		}
		free (where);
		break;
	}
	case 'a':
		ok = git_async (&repo, verb, (const char *[]) { "fetch", "-v", "--all", NULL });
		break;
	}
	free (branch);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_push (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return remote_command ("push", "Push to");
}

static bool
F_magit_pull (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return remote_command ("pull", "Pull from");
}

static bool
F_magit_fetch (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return remote_command ("fetch", "Fetch from");
}


/* Branches, merging, rebasing, resetting, tags and stashes. */

static bool
F_magit_branch (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'b', "Checkout" }, { 'c', "Create and checkout" }, { 'n', "Create" },
		{ 'm', "Rename" }, { 'k', "Delete" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char *name = NULL, *start = NULL, *branch;
	if (!current_branch (&repo, &branch)) {
		repo_clear (&repo);
		return false;
	}
	char key = transient ("Branch", ACTIONS (actions));
	switch (key) {
	case 'b':
		if ((name = read_ref (&repo, "Checkout", ref_at_point (), true)) != NULL)
			ok = git_run (&repo, (const char *[]) { "checkout", name, NULL });
		break;
	case 'c':
	case 'n': {
		bool checkout = key == 'c';
		if ((name = read_string ("Name for new branch: ", "")) != NULL && name[0] != '\0' &&
		    (start = read_ref (&repo, "Start point", ref_at_point () != NULL ? ref_at_point () : "HEAD", true)) != NULL)
			ok = checkout
				? git_run (&repo, (const char *[]) { "checkout", "-b", name, start, NULL })
				: git_run (&repo, (const char *[]) { "branch", name, start, NULL });
		break;
	}
	case 'm':
		if ((start = read_ref (&repo, "Rename branch", branch, false)) != NULL &&
		    (name = read_string ("Rename to: ", "")) != NULL && name[0] != '\0')
			ok = git_run (&repo, (const char *[]) { "branch", "-m", start, name, NULL });
		break;
	case 'k':
		if ((name = read_ref (&repo, "Delete branch", NULL, false)) != NULL) {
			ok = git_run (&repo, (const char *[]) { "branch", "-d", name, NULL });
			if (!ok && minibuf_read_y_or_n ("Branch %s is not fully merged; delete it anyway? ", name) == 1)
				ok = git_run (&repo, (const char *[]) { "branch", "-D", name, NULL });
		}
		break;
	}
	free (name);
	free (start);
	free (branch);
	refresh_after_change (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_merge (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'm', "Merge" }, { 'n', "Merge, don't commit" }, { 's', "Squash merge" }, { 'a', "Abort" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char key = transient ("Merge", ACTIONS (actions));
	char *name = NULL;
	if (key == 'a') {
		if (minibuf_read_y_or_n ("Abort merge? ") == 1)
			ok = git_run (&repo, (const char *[]) { "merge", "--abort", NULL });
	} else if (key != 0 && (name = read_ref (&repo, "Merge", ref_at_point (), true)) != NULL) {
		const char *opt = key == 'm' ? "--no-edit" : key == 'n' ? "--no-commit" : "--squash";
		ok = git_run (&repo, (const char *[]) { "merge", opt, name, NULL });
	}
	free (name);
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_rebase (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'u', "onto upstream" }, { 'e', "onto elsewhere" }, { 'c', "Continue" },
		{ 's', "Skip" }, { 'a', "Abort" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char *name = NULL;
	switch (transient ("Rebase", ACTIONS (actions))) {
	case 'u':
		ok = git_run (&repo, (const char *[]) { "rebase", "@{upstream}", NULL });
		break;
	case 'e':
		if ((name = read_ref (&repo, "Rebase onto", ref_at_point (), true)) != NULL)
			ok = git_run (&repo, (const char *[]) { "rebase", name, NULL });
		break;
	case 'c':
		ok = git_run (&repo, (const char *[]) { "-c", "core.editor=true", "rebase", "--continue", NULL });
		break;
	case 's':
		ok = git_run (&repo, (const char *[]) { "rebase", "--skip", NULL });
		break;
	case 'a':
		if (minibuf_read_y_or_n ("Abort this rebase? ") == 1)
			ok = git_run (&repo, (const char *[]) { "rebase", "--abort", NULL });
		break;
	}
	free (name);
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
reset_to (const char *mode)
{
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char *prompt = xasprintf ("Reset %s to", mode + 2);
	char *rev = read_ref (&repo, prompt, ref_at_point () != NULL ? ref_at_point () : "HEAD", true);
	free (prompt);
	if (rev != NULL && (strcmp (mode, "--hard") != 0 ||
			    minibuf_read_y_or_n ("Hard reset to %s, discarding all changes? ", rev) == 1))
		ok = git_run (&repo, (const char *[]) { "reset", mode, rev, NULL });
	free (rev);
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_reset (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'm', "mixed (HEAD and index)" }, { 's', "soft (HEAD only)" }, { 'h', "hard (and worktree)" },
	};
	switch (transient ("Reset", ACTIONS (actions))) {
	case 'm':
		return reset_to ("--mixed");
	case 's':
		return reset_to ("--soft");
	case 'h':
		return reset_to ("--hard");
	}
	return false;
}

static bool
F_magit_reset_quickly (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return reset_to ("--mixed");
}

static bool
F_magit_tag (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = { { 't', "Create" }, { 'k', "Delete" } };
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char *name = NULL, *rev = NULL;
	switch (transient ("Tag", ACTIONS (actions))) {
	case 't':
		if ((name = read_string ("Tag name: ", "")) != NULL && name[0] != '\0' &&
		    (rev = read_ref (&repo, "Place tag on", ref_at_point () != NULL ? ref_at_point () : "HEAD", true)) != NULL)
			ok = git_run (&repo, (const char *[]) { "tag", name, rev, NULL });
		break;
	case 'k':
		if ((name = read_string ("Delete tag: ", "")) != NULL && name[0] != '\0')
			ok = git_run (&repo, (const char *[]) { "tag", "-d", name, NULL });
		break;
	}
	free (name);
	free (rev);
	refresh_after_change (&repo);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_stash (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'z', "Stash" }, { 'i', "Stash index" }, { 'p', "Pop" }, { 'a', "Apply" }, { 'k', "Drop" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	const char *at = ref_at_point ();
	char *stash = xstrdup (at != NULL && str_has_prefix (at, "stash@") ? at : "stash@{0}");
	bool ok = false;
	char *msg = NULL;
	char key = transient ("Stash", ACTIONS (actions));
	switch (key) {
	case 'z':
	case 'i':
		if ((msg = read_string ("Stash message: ", "")) != NULL) {
			bool index = key == 'i';
			const char *argv[6] = { "stash", "push" };
			size_t k = 2;
			if (index)
				argv[k++] = "--staged";
			if (msg[0] != '\0') {
				argv[k++] = "-m";
				argv[k++] = msg;
			}
			argv[k] = NULL;
			ok = git_run (&repo, argv);
		}
		break;
	case 'p':
		ok = git_run (&repo, (const char *[]) { "stash", "pop", "--index", stash, NULL }) ||
			git_run (&repo, (const char *[]) { "stash", "pop", stash, NULL });
		break;
	case 'a':
		ok = git_run (&repo, (const char *[]) { "stash", "apply", stash, NULL });
		break;
	case 'k':
		if (minibuf_read_y_or_n ("Drop %s? ", stash) == 1)
			ok = git_run (&repo, (const char *[]) { "stash", "drop", stash, NULL });
		break;
	}
	free (msg);
	free (stash);
	refresh_status_of (&repo);
	repo_clear (&repo);
	return ok;
}


/* Logs and diffs. */

static bool
F_magit_log (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'l', "current branch" }, { 'a', "all references" }, { 'o', "other" }, { 'f', "file at point" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char *other = NULL;
	switch (transient ("Log", ACTIONS (actions))) {
	case 'l':
		ok = show_magit_buffer ("magit-log", &repo, MAGIT_LOG, NULL);
		break;
	case 'a':
		ok = show_magit_buffer ("magit-log", &repo, MAGIT_LOG, "--all");
		break;
	case 'o':
		if ((other = read_ref (&repo, "Log rev,s", ref_at_point (), true)) != NULL)
			ok = show_magit_buffer ("magit-log", &repo, MAGIT_LOG, other);
		break;
	case 'f': {
		const LineInfo *li = cur_bp->magit != NULL ? current_line () : NULL;
		FileDiff *f = li != NULL ? line_file (cur_bp->magit, li) : NULL;
		char *file = f != NULL ? xstrdup (f->path) : NULL;
		const char *visited = repo.remote != NULL
			? (cur_bp->remote == repo.remote ? cur_bp->remote_path : NULL)
			: (cur_bp->remote == NULL ? cur_bp->filename : NULL);
		size_t rlen = strlen (repo.root);
		if (file == NULL && visited != NULL && str_has_prefix (visited, repo.root) && visited[rlen] == '/')
			file = xstrdup (visited + rlen + 1);
		if (file == NULL)
			minibuf_error ("No file here");
		else {
			char *log_args = xstrcat3 ("--follow -- ", file, "");
			ok = show_magit_buffer ("magit-log", &repo, MAGIT_LOG, log_args);
			free (log_args);
		}
		free (file);
		break;
	}
	}
	free (other);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_diff (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	static const Action actions[] = {
		{ 'd', "Dwim" }, { 'u', "unstaged" }, { 's', "staged" }, { 'w', "worktree" }, { 'r', "range" },
		{ 'c', "commit at point" },
	};
	Repo repo;
	if (!current_repo (&repo))
		return false;
	bool ok = false;
	char *range = NULL;
	switch (transient ("Diff", ACTIONS (actions))) {
	case 'd': {
		const LineInfo *li = cur_bp->magit != NULL ? current_line () : NULL;
		if (li != NULL && li->sec == SEC_STAGED)
			ok = show_magit_buffer ("magit-diff", &repo, MAGIT_DIFF, "--cached");
		else if (li != NULL && li->ref != NULL)
			ok = show_revision (&repo, li->ref);
		else
			ok = show_magit_buffer ("magit-diff", &repo, MAGIT_DIFF, NULL);
		break;
	}
	case 'u':
		ok = show_magit_buffer ("magit-diff", &repo, MAGIT_DIFF, NULL);
		break;
	case 's':
		ok = show_magit_buffer ("magit-diff", &repo, MAGIT_DIFF, "--cached");
		break;
	case 'w':
		ok = show_magit_buffer ("magit-diff", &repo, MAGIT_DIFF, "HEAD");
		break;
	case 'r':
		if ((range = read_ref (&repo, "Diff for range", ref_at_point (), true)) != NULL)
			ok = show_magit_buffer ("magit-diff", &repo, MAGIT_DIFF, range);
		break;
	case 'c':
		if (ref_at_point () != NULL) {
			char *ref = xstrdup (ref_at_point ());
			ok = show_revision (&repo, ref);
			free (ref);
		} else
			minibuf_error ("No commit at point");
		break;
	}
	free (range);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_process_buffer (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Repo repo;
	if (!current_repo (&repo))
		return false;
	Buffer *bp = process_buffer (&repo);
	repo_clear (&repo);
	buffer_switch_to (bp, true);
	buffer_goto_offset (bp, buffer_length (bp));
	return true;
}

static bool
F_magit_git_command (long uniarg, Args *args)
{
	(void) uniarg;
	Repo repo;
	if (!current_repo (&repo))
		return false;
	char *cmd = xstrdup_or_null (args_poll (args));
	if (cmd == NULL)
		cmd = read_string ("Git subcommand (in repository root): git ", "");
	bool ok = false;
	if (cmd != NULL && cmd[0] != '\0') {
		/* Split on spaces, honouring simple quotes. */
		const char *argv[64];
		size_t k = 0;
		char *p = cmd;
		while (*p != '\0' && k < 63) {
			while (*p == ' ')
				p++;
			if (*p == '\0')
				break;
			char quote = *p == '"' || *p == '\'' ? *p++ : 0;
			argv[k++] = p;
			while (*p != '\0' && (quote ? *p != quote : *p != ' '))
				p++;
			if (*p != '\0')
				*p++ = '\0';
		}
		argv[k] = NULL;
		StrBuf out;
		sb_init (&out);
		ok = git_run_input (&repo, argv, NULL, 0, &out);
		if (ok) {
			size_t n = out.len;
			while (n > 0 && out.s[n - 1] == '\n')
				n--;
			if (memchr (out.s, '\n', n) != NULL)
				minibuf_write ("Git finished (see $ for output)");
			else
				minibuf_write ("%.*s", (int) n, out.s);
		}
		sb_free (&out);
		refresh_status_of (&repo);
	}
	free (cmd);
	repo_clear (&repo);
	return ok;
}

static bool
F_magit_dispatch (long uniarg, Args *args)
{
	(void) args;
	static const Action actions[] = {
		{ 'b', "Branch" }, { 'c', "Commit" }, { 'd', "Diff" }, { 'f', "Fetch" },
		{ 'F', "Pull" }, { 'l', "Log" }, { 'm', "Merge" }, { 'P', "Push" },
		{ 'r', "Rebase" }, { 't', "Tag" }, { 'X', "Reset" }, { 'z', "Stash" },
		{ 's', "Stage" }, { 'u', "Unstage" }, { 'k', "Discard" }, { 'g', "Refresh" },
		{ '$', "Process buffer" }, { '!', "Git command" }, { '4', "Show all diffs" }, { 'q', "Quit" },
	};
	static const struct {
		char key;
		const char *func;
	} funcs[] = {
		{ 'b', "magit-branch" }, { 'c', "magit-commit" }, { 'd', "magit-diff" }, { 'f', "magit-fetch" },
		{ 'F', "magit-pull" }, { 'l', "magit-log" }, { 'm', "magit-merge" }, { 'P', "magit-push" },
		{ 'r', "magit-rebase" }, { 't', "magit-tag" }, { 'X', "magit-reset" }, { 'z', "magit-stash" },
		{ 's', "magit-stage" }, { 'u', "magit-unstage" }, { 'k', "magit-discard" }, { 'g', "magit-refresh" },
		{ '$', "magit-process-buffer" }, { '!', "magit-git-command" }, { '4', "magit-show-level-4" },
		{ 'q', "quit-window" },
	};
	char key = transient ("Magit (TAB toggle, RET visit, n/p move)", ACTIONS (actions));
	for (size_t i = 0; i < sizeof funcs / sizeof funcs[0]; i++)
		if (funcs[i].key == key)
			return funcall_n (funcs[i].func, uniarg);
	return false;
}


/*--------------------------------------------------------------------------
 * Highlighting.
 *--------------------------------------------------------------------------*/

static int
magit_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		 size_t len, int start_state, const char **face_names)
{
	(void) mode;
	(void) start_o;
	(void) start_state;
	const LineInfo *li = line_at (bp, line_idx);
	for (size_t i = 0; i < len; i++)
		face_names[i] = li != NULL ? li->face : NULL;
	if (li != NULL)
		for (uint8_t k = 0; k < li->nspans; k++)
			for (size_t i = li->spans[k].start; i < (size_t) li->spans[k].start + li->spans[k].len && i < len; i++)
				face_names[i] = li->spans[k].face;
	return SYNTAX_STATE_NORMAL;
}

static int
commit_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		  size_t len, int start_state, const char **face_names)
{
	(void) mode;
	(void) start_state;
	bool comment = len > 0 && buffer_get_char (bp, start_o) == '#';
	for (size_t i = 0; i < len; i++)
		face_names[i] = comment ? FACE_FONT_LOCK_COMMENT :
			line_idx == 0 && i >= 50 ? FACE_WARNING : line_idx == 0 ? FACE_FONT_LOCK_KEYWORD : NULL;
	return SYNTAX_STATE_NORMAL;
}


/*--------------------------------------------------------------------------
 * Keys.
 *--------------------------------------------------------------------------*/

static void
init_commit_map (void)
{
	if (commit_map != NULL)
		return;
	commit_map = keymap_new ();
	keymap_bind (commit_map, "\\C-c\\C-c", "git-commit-commit");
	keymap_bind (commit_map, "\\C-c\\C-k", "git-commit-abort");
}

static void
init_status_map (void)
{
	static const struct {
		const char *keys;
		const char *func;
	} bindings[] = {
		{ "\\TAB", "magit-section-toggle" },
		{ "\\RET", "magit-visit-thing" },
		{ "n", "magit-section-forward" },
		{ "p", "magit-section-backward" },
		{ "\\M-n", "magit-section-forward" },
		{ "\\M-p", "magit-section-backward" },
		{ "g", "magit-refresh" },
		{ "1", "magit-show-level-1" },
		{ "4", "magit-show-level-4" },
		{ "s", "magit-stage" },
		{ "S", "magit-stage-modified" },
		{ "u", "magit-unstage" },
		{ "U", "magit-unstage-all" },
		{ "k", "magit-discard" },
		{ "c", "magit-commit" },
		{ "P", "magit-push" },
		{ "F", "magit-pull" },
		{ "f", "magit-fetch" },
		{ "b", "magit-branch" },
		{ "l", "magit-log" },
		{ "d", "magit-diff" },
		{ "m", "magit-merge" },
		{ "r", "magit-rebase" },
		{ "x", "magit-reset-quickly" },
		{ "X", "magit-reset" },
		{ "t", "magit-tag" },
		{ "z", "magit-stash" },
		{ "$", "magit-process-buffer" },
		{ "!", "magit-git-command" },
		{ "?", "magit-dispatch" },
		{ "h", "magit-dispatch" },
		{ "q", "quit-window" },
	};
	status_map = keymap_new ();
	for (size_t i = 0; i < sizeof bindings / sizeof bindings[0]; i++)
		keymap_bind (status_map, bindings[i].keys, bindings[i].func);
}

void
magit_init (void)
{
	lisp_func_new ("magit-status", F_magit_status, true,
		       "Show the status of the current Git repository.\n"
		       "TAB toggles sections, RET visits the thing at point, s, u and k stage,\n"
		       "unstage and discard it (or the files in the region), and c, P, F, f,\n"
		       "b, l, d, m, r, X, t and z show popups for committing, pushing, pulling,\n"
		       "fetching, branches, logs, diffs, merging, rebasing, resetting, tags\n"
		       "and stashes. 4 shows every diff, g refreshes, $ shows the git commands\n"
		       "run, and ? lists the commands.\n"
		       "\n"
		       "In a remote buffer, the repository is on its machine, and git runs\n"
		       "there. The optional argument DIR names a directory in the repository,\n"
		       "on the current buffer's machine, relative to the buffer's directory.");
	lisp_func_new ("magit-refresh", F_magit_refresh, true, "Refresh the current Magit buffer.");
	lisp_func_new ("magit-section-toggle", F_magit_section_toggle, true,
		       "Show or hide the section at point.");
	lisp_func_new ("magit-section-forward", F_magit_section_forward, true, "Move to the next section.");
	lisp_func_new ("magit-section-backward", F_magit_section_backward, true, "Move to the previous section.");
	lisp_func_new ("magit-show-level-1", F_magit_show_level_1, true, "Collapse every file's diff.");
	lisp_func_new ("magit-show-level-4", F_magit_show_level_4, true,
		       "Show every section and every file's diff.");
	lisp_func_new ("magit-visit-thing", F_magit_visit_thing, true,
		       "Visit the file, line, commit or stash at point.");
	lisp_func_new ("magit-stage", F_magit_stage, true,
		       "Stage the change at point, or the files in the region.");
	lisp_func_new ("magit-stage-modified", F_magit_stage_modified, true,
		       "Stage all changes to tracked files.");
	lisp_func_new ("magit-unstage", F_magit_unstage, true,
		       "Unstage the change at point, or the files in the region.");
	lisp_func_new ("magit-unstage-all", F_magit_unstage_all, true, "Unstage all staged changes.");
	lisp_func_new ("magit-discard", F_magit_discard, true,
		       "Discard the change at point, or the files in the region.");
	lisp_func_new ("magit-commit", F_magit_commit, true, "Commit, amend, extend or reword.");
	lisp_func_new ("git-commit-commit", F_git_commit_commit, true,
		       "Commit with the message in this buffer.");
	lisp_func_new ("git-commit-abort", F_git_commit_abort, true, "Abort the commit.");
	lisp_func_new ("magit-push", F_magit_push, true, "Push the current branch.");
	lisp_func_new ("magit-pull", F_magit_pull, true, "Pull into the current branch.");
	lisp_func_new ("magit-fetch", F_magit_fetch, true, "Fetch from remotes.");
	lisp_func_new ("magit-branch", F_magit_branch, true, "Check out, create, rename or delete branches.");
	lisp_func_new ("magit-merge", F_magit_merge, true, "Merge a branch, or abort a merge.");
	lisp_func_new ("magit-rebase", F_magit_rebase, true, "Rebase, or continue, skip or abort a rebase.");
	lisp_func_new ("magit-reset", F_magit_reset, true, "Reset HEAD, the index and the worktree.");
	lisp_func_new ("magit-reset-quickly", F_magit_reset_quickly, true,
		       "Reset HEAD and the index to a commit.");
	lisp_func_new ("magit-tag", F_magit_tag, true, "Create or delete tags.");
	lisp_func_new ("magit-stash", F_magit_stash, true, "Stash, pop, apply or drop stashes.");
	lisp_func_new ("magit-log", F_magit_log, true, "Show a log.");
	lisp_func_new ("magit-diff", F_magit_diff, true, "Show a diff.");
	lisp_func_new ("magit-process-buffer", F_magit_process_buffer, true,
		       "Show the git commands run and their output.");
	lisp_func_new ("magit-git-command", F_magit_git_command, true, "Run a git command.");
	lisp_func_new ("magit-dispatch", F_magit_dispatch, true, "Show Magit's commands.");
	init_status_map ();
}
