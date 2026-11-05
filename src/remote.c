/* Remote files over SSH

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

/* A remote buffer visits a file or directory on another machine,
   reached with the system's OpenSSH client. Each operation runs one
   ssh command, with an argument vector (no local shell), in batch mode
   so that ssh never prompts: authentication must come from keys, the
   agent or the ssh configuration, and host keys are checked as ssh's
   configuration says. Connections to a machine share one master
   connection, through a control socket in a private directory.

   On the remote side, each operation is a fixed sh(1) script, given
   its arguments (paths, quoted for the remote shell) as positional
   parameters; file contents travel on standard input and output. The
   scripts print a marker before their answer, so that output from
   the remote login shell's startup files is skipped.

   Operations wait for ssh while polling the keyboard: C-g kills ssh,
   other keys are kept for later, and remote-timeout bounds the wait.

   Shell commands and programs run the same way. Terminals and
   background programs instead run ssh as long-lived processes of
   terminal.c or process.c (terminals on a local pseudo-terminal,
   asking for a remote one), which remote-timeout does not bound.

   Operations use the keyboard, the minibuffer and the error state
   below, so they must run on the editor's thread only.

   Machines are interned, and live until exit: buffers point to them. */

#include "xzile.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <time.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

/* Remote command output beyond this is refused. */
#define MAX_OUTPUT (256u * 1024 * 1024)
/* Show progress for operations taking longer than this, in ms. */
#define PROGRESS_DELAY 300
#define POLL_MS 50
#define MAX_INCLUDE_DEPTH 8

struct Remote {
	Remote *next;		/* All machines of the session. */
	char *host;		/* Host name or ssh configuration alias. */
	char *user;		/* Login name, or NULL for ssh's default. */
	char *port;		/* Port, or NULL for ssh's default. */
	char *label;		/* [user@]host[:port], as shown. */
	char *home;		/* The remote home directory, once known. */
	char *nodename;		/* Its `uname -n', once a terminal was started. */
	char *control;		/* Control socket, once a connection was made. */
	unsigned long used;	/* Order of last successful use; 0 if never. */
};

static Remote *remotes;
static unsigned long use_counter;
static char *control_dir;
static unsigned control_count;

static char *last_error;
static bool last_cancelled;

static void
set_error (const char *fmt, ...) PRINTF_LIKE (1, 2);

static void
set_error (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	free (last_error);
	last_error = xvasprintf (fmt, ap);
	va_end (ap);
	debug_log ("remote", "error: %s", last_error);
}

static void
clear_error (void)
{
	free (last_error);
	last_error = NULL;
	last_cancelled = false;
}

const char *
remote_last_error (void)
{
	return last_error != NULL ? last_error : "Remote operation failed";
}

bool
remote_last_cancelled (void)
{
	return last_cancelled;
}


/*--------------------------------------------------------------------------
 * Machines.
 *--------------------------------------------------------------------------*/

static bool
valid_host (const char *s)
{
	size_t n = strlen (s);
	if (n == 0 || n > 255 || s[0] == '-')
		return false;
	for (size_t i = 0; i < n; i++)
		if (!isalnum ((unsigned char) s[i]) && strchr (".-_:%", s[i]) == NULL)
			return false;
	return true;
}

static bool
valid_user (const char *s)
{
	size_t n = strlen (s);
	if (n == 0 || n > 64 || s[0] == '-')
		return false;
	for (size_t i = 0; i < n; i++)
		if (!isalnum ((unsigned char) s[i]) && strchr (".-_", s[i]) == NULL)
			return false;
	return true;
}

static bool
valid_port (const char *s)
{
	long n;
	if (strlen (s) > 5 || !isdigit ((unsigned char) s[0]) || !parse_long (s, &n))
		return false;
	for (const char *c = s; *c != '\0'; c++)
		if (!isdigit ((unsigned char) *c))
			return false;
	return n >= 1 && n <= 65535;
}

static bool
opt_eq (const char *a, const char *b)
{
	return (a == NULL && b == NULL) || (a != NULL && b != NULL && strcmp (a, b) == 0);
}

/* The machine `host', with an optional user and port (NULL or ""
   for ssh's defaults); NULL, with the error set, if they are not
   valid. */
Remote *
remote_get (const char *host, const char *user, const char *port)
{
	clear_error ();
	if (user != NULL && user[0] == '\0')
		user = NULL;
	if (port != NULL && port[0] == '\0')
		port = NULL;
	if (host == NULL || !valid_host (host)) {
		set_error ("Invalid host name `%s'", host != NULL ? host : "");
		return NULL;
	}
	if (user != NULL && !valid_user (user)) {
		set_error ("Invalid user name `%s'", user);
		return NULL;
	}
	if (port != NULL && !valid_port (port)) {
		set_error ("Invalid port `%s': use a number from 1 to 65535", port);
		return NULL;
	}

	for (Remote *r = remotes; r != NULL; r = r->next)
		if (strcmp (r->host, host) == 0 && opt_eq (r->user, user) && opt_eq (r->port, port))
			return r;

	Remote *r = xzalloc (sizeof (Remote));
	r->host = xstrdup (host);
	r->user = xstrdup_or_null (user);
	r->port = xstrdup_or_null (port);
	StrBuf sb;
	sb_init (&sb);
	if (user != NULL)
		sb_addf (&sb, "%s@", user);
	sb_add (&sb, host);
	if (port != NULL)
		sb_addf (&sb, ":%s", port);
	r->label = sb_take (&sb);
	r->next = remotes;
	remotes = r;
	return r;
}

/* Parse `[user@]host[:port]'. */
Remote *
remote_parse (const char *spec)
{
	char *s = xstrdup (spec);
	char *host = s, *user = NULL, *port = NULL;
	char *at = strrchr (s, '@');
	if (at != NULL) {
		*at = '\0';
		user = s;
		host = at + 1;
	}
	char *colon = strchr (host, ':');
	if (colon != NULL && strchr (colon + 1, ':') == NULL) {
		*colon = '\0';
		port = colon + 1;
	}
	Remote *r = remote_get (host, user, port);
	free (s);
	return r;
}

const char *
remote_label (const Remote *r)
{
	return r->label;
}

/* The `i'th most recently used machine, or NULL. */
Remote *
remote_recent (size_t i)
{
	Remote *v[256];
	size_t n = 0;
	for (Remote *r = remotes; r != NULL && n < 256; r = r->next)
		if (r->used > 0)
			v[n++] = r;
	for (size_t a = 1; a < n; a++)
		for (size_t b = a; b > 0 && v[b - 1]->used < v[b]->used; b--) {
			Remote *t = v[b - 1];
			v[b - 1] = v[b];
			v[b] = t;
		}
	return i < n ? v[i] : NULL;
}

static void
note_used (Remote *r)
{
	r->used = ++use_counter;
}


/*--------------------------------------------------------------------------
 * The ssh configuration.
 *--------------------------------------------------------------------------*/

/* The ssh configuration file to use, or NULL for ssh's default. */
static char *
config_file (void)
{
	const char *v = get_variable ("ssh-config-file");
	if (v == NULL || v[0] == '\0' || strcmp (v, "nil") == 0)
		return NULL;
	return expand_path (v);
}

static void
read_config (const char *file, StrVec *out, int depth)
{
	char *s;
	size_t len;
	if (depth > MAX_INCLUDE_DEPTH || !read_file (file, &s, &len))
		return;
	char *home = get_home_dir ();
	char *ssh_dir = path_join (home, ".ssh");
	free (home);

	for (char *p = s, *line; (line = strsep (&p, "\n")) != NULL;) {
		char *c = line;
		while (isspace ((unsigned char) *c))
			c++;
		char *kw = c;
		while (*c != '\0' && !isspace ((unsigned char) *c) && *c != '=')
			c++;
		size_t kwlen = (size_t) (c - kw);
		while (isspace ((unsigned char) *c) || *c == '=')
			c++;
		bool is_host = kwlen == 4 && strncasecmp (kw, "host", 4) == 0;
		bool is_include = kwlen == 7 && strncasecmp (kw, "include", 7) == 0;
		if (!is_host && !is_include)
			continue;
		for (char *tok; (tok = strsep (&c, " \t\r")) != NULL;) {
			/* Separators may repeat: strsep gives empty tokens. */
			if (tok[0] == '\0')
				continue;
			if (tok[0] == '#')
				break;
			if (tok[0] == '"') {
				tok++;
				size_t n = strlen (tok);
				if (n > 0 && tok[n - 1] == '"')
					tok[n - 1] = '\0';
			}
			if (is_host) {
				if (strpbrk (tok, "*?!") == NULL && valid_host (tok))
					sv_add_sorted (out, tok);
				continue;
			}
			char *pattern = tok[0] == '~' || tok[0] == '/' ? expand_path (tok) : path_join (ssh_dir, tok);
			glob_t g;
			if (pattern != NULL && glob (pattern, 0, NULL, &g) == 0) {
				for (size_t i = 0; i < g.gl_pathc; i++)
					read_config (g.gl_pathv[i], out, depth + 1);
				globfree (&g);
			}
			free (pattern);
		}
	}
	free (ssh_dir);
	free (s);
}

/* The host aliases of the ssh configuration, without patterns. */
void
remote_config_hosts (StrVec *out)
{
	char *file = config_file ();
	if (file == NULL) {
		char *home = get_home_dir ();
		file = path_join (home, ".ssh/config");
		free (home);
	}
	read_config (file, out, 0);
	free (file);
}


/*--------------------------------------------------------------------------
 * Running ssh.
 *--------------------------------------------------------------------------*/

static long
timeout_seconds (void)
{
	long n;
	const char *v = get_variable ("remote-timeout");
	if (v == NULL || !parse_long (v, &n) || n <= 0)
		n = 30;
	return MIN (n, 24L * 60 * 60);
}

/* Quote `s' for a POSIX shell. */
static void
add_quoted (StrBuf *sb, const char *s)
{
	sb_addc (sb, '\'');
	for (; *s != '\0'; s++)
		if (*s == '\'')
			sb_add (sb, "'\\''");
		else
			sb_addc (sb, *s);
	sb_addc (sb, '\'');
}

static bool
ensure_control (Remote *r)
{
	if (r->control != NULL)
		return true;
	if (control_dir == NULL) {
		char tmpl[] = "/tmp/xzile-ssh-XXXXXX";
		if (mkdtemp (tmpl) == NULL)
			return false;
		control_dir = xstrdup (tmpl);
	}
	r->control = xasprintf ("%s/%u", control_dir, ++control_count);
	return true;
}

typedef struct {
	StrBuf out, err;
	int status;		/* Exit status; 255 for ssh's own errors. */
	bool ran;		/* ssh could be started and finished. */
} RunResult;

static void
kill_group (pid_t pid)
{
	kill (-pid, SIGTERM);
	kill (pid, SIGTERM);
	int64_t until = monotonic_ms () + 1000;
	int status;
	while (waitpid (pid, &status, WNOHANG) == 0) {
		if (monotonic_ms () >= until) {
			kill (-pid, SIGKILL);
			kill (pid, SIGKILL);
			while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
				;
			return;
		}
		poll (NULL, 0, 10);
	}
}

static void
argv_push (const char ***argv, size_t *n, const char *s)
{
	*argv = xrealloc (*argv, (*n + 1) * sizeof (char *));
	(*argv)[(*n)++] = s;
}

/* The arguments to run ssh to `r', with `extra' options, and `cmd'
   (NULL for none). The strings belong to `keep'. */
static const char **
ssh_argv (Remote *r, const char *const *extra, const char *cmd, StrVec *keep)
{
	const char **argv = NULL;
	size_t n = 0;
	long t = timeout_seconds ();
	char *cfg = config_file ();
	sv_push (keep, "ssh");
	sv_push (keep, "-o");
	sv_push (keep, "BatchMode=yes");
	char *ct = xasprintf ("ConnectTimeout=%ld", t);
	sv_push (keep, "-o");
	sv_push (keep, ct);
	free (ct);
	if (r->control != NULL) {
		char *cp = xasprintf ("ControlPath=%s", r->control);
		sv_push (keep, "-o");
		sv_push (keep, cp);
		free (cp);
	}
	if (cfg != NULL) {
		sv_push (keep, "-F");
		sv_push (keep, cfg);
		free (cfg);
	}
	if (r->user != NULL) {
		sv_push (keep, "-l");
		sv_push (keep, r->user);
	}
	if (r->port != NULL) {
		sv_push (keep, "-p");
		sv_push (keep, r->port);
	}
	for (size_t i = 0; extra != NULL && extra[i] != NULL; i++)
		sv_push (keep, extra[i]);
	sv_push (keep, "--");
	sv_push (keep, r->host);
	if (cmd != NULL)
		sv_push (keep, cmd);
	for (size_t i = 0; i < keep->n; i++)
		argv_push (&argv, &n, keep->v[i]);
	argv_push (&argv, &n, NULL);
	return argv;
}

static bool
set_nonblock (int fd)
{
	int fl = fcntl (fd, F_GETFL);
	return fl >= 0 && fcntl (fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

/* Run `argv', feeding it `input', until it exits, the time runs out
   or the user types C-g. Without `interactive', only the time is
   checked. Returns false with the error set on failure to run it,
   on timeout and on cancellation. */
static bool
run_argv (const char *const *argv, const char *input, size_t input_len, RunResult *res,
	  const char *what, const char *label, int64_t timeout_ms, bool interactive)
{
	sb_init (&res->out);
	sb_init (&res->err);
	res->status = -1;
	res->ran = false;

	int in_pipe[2], out_pipe[2], err_pipe[2];
	if (pipe (in_pipe) != 0)
		goto spawn_error;
	if (pipe (out_pipe) != 0) {
		close (in_pipe[0]);
		close (in_pipe[1]);
		goto spawn_error;
	}
	if (pipe (err_pipe) != 0) {
		close (in_pipe[0]);
		close (in_pipe[1]);
		close (out_pipe[0]);
		close (out_pipe[1]);
		goto spawn_error;
	}
	int ours[3] = { in_pipe[1], out_pipe[0], err_pipe[0] };
	for (int i = 0; i < 3; i++)
		fcntl (ours[i], F_SETFD, FD_CLOEXEC);

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init (&fa);
	posix_spawn_file_actions_adddup2 (&fa, in_pipe[0], 0);
	posix_spawn_file_actions_adddup2 (&fa, out_pipe[1], 1);
	posix_spawn_file_actions_adddup2 (&fa, err_pipe[1], 2);
	posix_spawn_file_actions_addclose (&fa, in_pipe[0]);
	posix_spawn_file_actions_addclose (&fa, out_pipe[1]);
	posix_spawn_file_actions_addclose (&fa, err_pipe[1]);
	posix_spawnattr_t attr;
	posix_spawnattr_init (&attr);
	sigset_t mask, def;
	sigemptyset (&mask);
	sigemptyset (&def);
	int sigs[] = { SIGPIPE, SIGINT, SIGQUIT, SIGTSTP, SIGCHLD, SIGWINCH, SIGHUP, SIGTERM };
	for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		sigaddset (&def, sigs[i]);
	posix_spawnattr_setsigmask (&attr, &mask);
	posix_spawnattr_setsigdefault (&attr, &def);
	short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP;
	posix_spawnattr_setpgroup (&attr, 0);
#ifdef POSIX_SPAWN_SETSID
	/* Without a controlling terminal ssh cannot prompt for anything. */
	flags = (short) ((flags & ~POSIX_SPAWN_SETPGROUP) | POSIX_SPAWN_SETSID);
#endif
	posix_spawnattr_setflags (&attr, flags);
	pid_t pid;
	int rc = posix_spawnp (&pid, argv[0], &fa, &attr, (char *const *) argv, environ);
	posix_spawn_file_actions_destroy (&fa);
	posix_spawnattr_destroy (&attr);
	close (in_pipe[0]);
	close (out_pipe[1]);
	close (err_pipe[1]);
	if (rc != 0) {
		for (int i = 0; i < 3; i++)
			close (ours[i]);
		errno = rc;
		goto spawn_error;
	}

	int in_fd = in_pipe[1], fds[2] = { out_pipe[0], err_pipe[0] };
	for (int i = 0; i < 3; i++)
		set_nonblock (ours[i]);
	size_t written = 0;
	if (input == NULL || input_len == 0) {
		close (in_fd);
		in_fd = -1;
	}

	int64_t start = monotonic_ms ();
	int64_t deadline = start + timeout_ms;
	bool progress = false, cancelled = false, timed_out = false, exited = false, too_big = false;
	int status = 0;
	KeyVec typed = { NULL, 0, 0 };

	/* Keys already waiting (typed ahead, or from a keyboard macro) are
	   not read, except for a C-g next in line. */
	bool queued = interactive && term_buf_len () > 0;
	if (queued) {
		Keystroke k = term_getkey (0);
		if (k == KBD_CANCEL)
			cancelled = true;
		else if (k != KBD_NOKEY)
			term_ungetkey (k);
	}

	char buf[65536];
	int64_t exit_time = 0;
	while (!cancelled) {
		if (!exited) {
			pid_t w = waitpid (pid, &status, WNOHANG);
			if (w == pid) {
				exited = true;
				exit_time = monotonic_ms ();
			}
		}
		/* A daemonized ssh master may hold the pipes a while. */
		if (exited && ((fds[0] < 0 && fds[1] < 0) || monotonic_ms () - exit_time > 200))
			break;

		int64_t now = monotonic_ms ();
		if (now >= deadline) {
			timed_out = true;
			break;
		}
		if (interactive && !progress && now - start >= PROGRESS_DELAY) {
			minibuf_write ("%s on ssh:%s... (C-g to cancel)", what, label);
			progress = true;
		}

		struct pollfd pfd[3];
		int which[3];
		nfds_t n = 0;
		for (int i = 0; i < 2; i++)
			if (fds[i] >= 0) {
				pfd[n].fd = fds[i];
				pfd[n].events = POLLIN;
				which[n++] = i;
			}
		if (in_fd >= 0) {
			pfd[n].fd = in_fd;
			pfd[n].events = POLLOUT;
			which[n++] = 2;
		}
		int ms = (int) MIN ((int64_t) POLL_MS, deadline - now);
		int pr = poll (pfd, n, ms);
		if (pr < 0 && errno != EINTR)
			break;
		for (nfds_t k = 0; pr > 0 && k < n; k++) {
			if (!(pfd[k].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR)))
				continue;
			int i = which[k];
			if (i == 2) {
				ssize_t wr = write (in_fd, input + written, input_len - written);
				if (wr > 0)
					written += (size_t) wr;
				if ((wr < 0 && errno != EAGAIN && errno != EINTR) || written == input_len) {
					close (in_fd);
					in_fd = -1;
				}
				continue;
			}
			ssize_t rd = read (fds[i], buf, sizeof buf);
			if (rd > 0) {
				StrBuf *sb = i == 0 ? &res->out : &res->err;
				if (sb->len + (size_t) rd > MAX_OUTPUT) {
					too_big = true;
					cancelled = true;
					break;
				}
				sb_addn (sb, buf, (size_t) rd);
			} else if (rd == 0 || (errno != EINTR && errno != EAGAIN)) {
				close (fds[i]);
				fds[i] = -1;
			}
		}

		if (interactive && !queued) {
			for (;;) {
				Keystroke key = term_getkey (0);
				if (key == KBD_NOKEY)
					break;
				if (key == KBD_CANCEL) {
					cancelled = true;
					break;
				}
				kv_push (&typed, key);
			}
		}
	}

	if (in_fd >= 0)
		close (in_fd);
	for (int i = 0; i < 2; i++)
		if (fds[i] >= 0)
			close (fds[i]);
	if (!exited && (cancelled || timed_out))
		kill_group (pid);
	else if (!exited)
		while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
			;
	for (size_t i = typed.n; i > 0; i--)
		term_ungetkey (typed.v[i - 1]);
	kv_free (&typed);
	if (progress)
		minibuf_clear ();

	debug_log ("remote", "%s on %s: %s after %lld ms", what, label,
		   cancelled ? "cancelled" : timed_out ? "timed out" :
		   WIFEXITED (status) ? "exited" : "killed", (long long) (monotonic_ms () - start));
	if (too_big) {
		set_error ("%s on ssh:%s: the answer is too large", what, label);
		return false;
	}
	if (cancelled) {
		set_error ("Cancelled: %s on ssh:%s", what, label);
		last_cancelled = true;
		return false;
	}
	if (timed_out) {
		set_error ("ssh %s: no answer within %ld seconds (see remote-timeout)", label, timeout_seconds ());
		return false;
	}
	res->ran = true;
	res->status = WIFEXITED (status) ? WEXITSTATUS (status) : 255;
	debug_log ("remote", "status %d", res->status);
	return true;

spawn_error:
	set_error ("Cannot run ssh: %s (XZile needs the OpenSSH client)", strerror (errno));
	return false;
}

/* The last non-empty line of `s'. */
static char *
last_line (const StrBuf *sb)
{
	size_t end = sb->len;
	while (end > 0 && (sb->s[end - 1] == '\n' || sb->s[end - 1] == '\r' || sb->s[end - 1] == ' '))
		end--;
	size_t start = end;
	while (start > 0 && sb->s[start - 1] != '\n')
		start--;
	return xstrndup (sb->s + start, end - start);
}

/* Explain a failure of ssh itself. */
static void
ssh_failed (Remote *r, const StrBuf *err)
{
	const char *e = err->s;
	const char *l = r->label;
	if (strstr (e, "Host key verification failed") != NULL || strstr (e, "IDENTIFICATION HAS CHANGED") != NULL ||
	    strstr (e, "No ED25519 host key is known") != NULL || strstr (e, "host key is known") != NULL)
		set_error ("ssh %s: the host key is unknown or has changed; check it by running `ssh %s' "
			   "in a terminal, then try again", l, l);
	else if (strstr (e, "Permission denied") != NULL || strstr (e, "Too many authentication failures") != NULL)
		set_error ("ssh %s: authentication failed; XZile does not ask for passwords: load your key "
			   "into ssh-agent (ssh-add), or set IdentityFile for the host in ~/.ssh/config", l);
	else if (strstr (e, "passphrase") != NULL)
		set_error ("ssh %s: the key needs a passphrase; add it to ssh-agent with ssh-add", l);
	else if (strstr (e, "Could not resolve hostname") != NULL)
		set_error ("ssh %s: unknown host; check the name, or its Host entry in ~/.ssh/config", l);
	else if (strstr (e, "Connection refused") != NULL)
		set_error ("ssh %s: connection refused; check that the SSH server runs and the port is right", l);
	else if (strstr (e, "timed out") != NULL)
		set_error ("ssh %s: the connection timed out", l);
	else {
		char *line = last_line (err);
		if (line[0] != '\0')
			set_error ("ssh %s: %s", l, line);
		else
			set_error ("ssh %s failed", l);
		free (line);
	}
}

/* Skip what the remote login shell printed before the marker. */
static bool
find_answer (const StrBuf *out, size_t *start)
{
	static const char marker[] = "XZ1\n";
	for (size_t i = 0; i + 4 <= out->len; i++)
		if ((i == 0 || out->s[i - 1] == '\n') && memcmp (out->s + i, marker, 4) == 0) {
			*start = i + 4;
			return true;
		}
	return false;
}

/* The remote command to run `script' with sh, with arguments `args'. */
static char *
script_command (const char *script, const char *const *args)
{
	StrBuf cmd;
	sb_init (&cmd);
	sb_add (&cmd, "sh -c ");
	add_quoted (&cmd, script);
	sb_add (&cmd, " xzile");
	for (size_t i = 0; args != NULL && args[i] != NULL; i++) {
		sb_addc (&cmd, ' ');
		add_quoted (&cmd, args[i]);
	}
	return sb_take (&cmd);
}

/* The arguments to run the remote command `cmd' on `r', through the
   shared connection if there can be one; with `tty', on a remote
   pseudo-terminal. The strings belong to `keep'. */
static const char **
command_argv (Remote *r, const char *cmd, bool tty, StrVec *keep)
{
	const char *mux_opts[] = { tty ? "-tt" : "-T", "-x", "-a", "-o", "ClearAllForwardings=yes",
				   "-o", "ControlMaster=auto", "-o", "ControlPersist=300", NULL };
	const char *plain_opts[] = { tty ? "-tt" : "-T", "-x", "-a", "-o", "ClearAllForwardings=yes", NULL };
	return ssh_argv (r, ensure_control (r) ? mux_opts : plain_opts, cmd, keep);
}

/* Run `script' with sh on `r', with arguments `args', for at most
   `timeout_ms' (with `interactive', C-g cancels it); false with the
   error set if ssh could not be run or did not finish. */
static bool
run_remote (Remote *r, const char *script, const char *const *args, const char *input, size_t input_len,
	    RunResult *res, const char *what, int64_t timeout_ms, bool interactive)
{
	char *cmd = script_command (script, args);
	StrVec keep = { NULL, 0, 0 };
	const char **argv = command_argv (r, cmd, false, &keep);
	free (cmd);
	debug_log ("remote", "%s on %s", what, r->label);
	bool ok = run_argv (argv, input, input_len, res, what, r->label, timeout_ms, interactive);
	free (argv);
	sv_clear (&keep);
	free (keep.v);
	return ok;
}

/* Run `script' with sh on `r', with arguments `args'. Returns the
   script's exit status (with `*answer' at its output after the
   marker), or -1 with the error set. */
static int
run_script (Remote *r, const char *script, const char *const *args, const char *input, size_t input_len,
	    RunResult *res, size_t *answer, const char *what)
{
	clear_error ();
	if (!run_remote (r, script, args, input, input_len, res, what, timeout_seconds () * 1000, true))
		return -1;
	if (res->status == 255) {
		ssh_failed (r, &res->err);
		return -1;
	}
	if (!find_answer (&res->out, answer)) {
		char *line = last_line (&res->err);
		if (line[0] != '\0')
			set_error ("%s on ssh:%s failed: %s", what, r->label, line);
		else
			set_error ("%s on ssh:%s failed: the remote shell gave no answer (exit status %d)",
				   what, r->label, res->status);
		free (line);
		return -1;
	}
	note_used (r);
	return res->status;
}

static void
result_free (RunResult *res)
{
	sb_free (&res->out);
	sb_free (&res->err);
}


/*--------------------------------------------------------------------------
 * Paths.
 *--------------------------------------------------------------------------*/

#define HOME_SCRIPT "printf \"XZ1\\n\"; cd 2>/dev/null; pwd"

/* Connect to `r', learning its home directory. */
bool
remote_connect (Remote *r)
{
	if (r->home != NULL) {
		clear_error ();
		return true;
	}
	RunResult res;
	size_t at;
	int st = run_script (r, HOME_SCRIPT, NULL, NULL, 0, &res, &at, "Connecting");
	if (st == 0) {
		char *home = xstrndup (res.out.s + at, res.out.len - at);
		size_t n = strlen (home);
		while (n > 0 && (home[n - 1] == '\n' || home[n - 1] == '\r'))
			home[--n] = '\0';
		if (home[0] == '/')
			r->home = home;
		else {
			free (home);
			r->home = xstrdup ("/");
		}
	} else if (st > 0)
		set_error ("ssh %s: the remote shell failed (exit status %d)", r->label, st);
	result_free (&res);
	return r->home != NULL;
}

/* Make `path' absolute and canonical on `r': `~' is its home,
   relative names are under it, and `//' starts again from the root,
   as expand_path does for local names. Returns NULL with the error set
   if the home directory cannot be found or for `~user'. */
char *
remote_expand_path (Remote *r, const char *path)
{
	bool needs_home = path[0] != '/';
	for (size_t i = 0; path[i] != '\0'; i++)
		if (path[i] == '~' && (i == 0 || path[i - 1] == '/'))
			needs_home = true;
	if (needs_home && !remote_connect (r))
		return NULL;

	StrBuf ep;
	sb_init (&ep);
	if (path[0] != '/' && path[0] != '~') {
		sb_add (&ep, r->home);
		if (ep.len == 0 || ep.s[ep.len - 1] != '/')
			sb_addc (&ep, '/');
	}
	for (size_t i = 0; path[i] != '\0';) {
		if (path[i] == '/') {
			if (path[++i] == '/') {
				while (path[i] == '/')
					i++;
				ep.len = 0;
				ep.s[0] = '\0';
			}
			if (ep.len == 0 || ep.s[ep.len - 1] != '/')
				sb_addc (&ep, '/');
		} else if (path[i] == '~' && (i == 0 || path[i - 1] == '/')) {
			ep.len = 0;
			ep.s[0] = '\0';
			++i;
			if (path[i] != '/' && path[i] != '\0') {
				set_error ("`~user' names are not supported on ssh:%s", r->label);
				sb_free (&ep);
				return NULL;
			}
			if (strcmp (r->home, "/") != 0)
				sb_add (&ep, r->home);
			if (path[i] == '\0')
				sb_addc (&ep, '/');
		} else if (path[i] == '.' && (path[i + 1] == '/' || path[i + 1] == '\0')) {
			++i;
		} else if (path[i] == '.' && path[i + 1] == '.' && (path[i + 2] == '/' || path[i + 2] == '\0')) {
			if (ep.len >= 1 && ep.s[ep.len - 1] == '/')
				ep.len--;
			while (ep.len >= 1 && ep.s[ep.len - 1] != '/')
				ep.len--;
			ep.s[ep.len] = '\0';
			i += 2;
		} else
			while (path[i] != '\0' && path[i] != '/')
				sb_addc (&ep, path[i++]);
	}
	if (ep.len == 0)
		sb_addc (&ep, '/');
	return sb_take (&ep);
}

/* `path' with the remote home directory shown as `~'. */
char *
remote_compact_path (Remote *r, const char *path)
{
	if (r->home != NULL && strcmp (r->home, "/") != 0) {
		size_t n = strlen (r->home);
		if (strncmp (path, r->home, n) == 0 && path[n] == '/')
			return xstrcat3 ("~", path + n, "");
		if (strcmp (path, r->home) == 0)
			return xstrdup ("~");
	}
	return xstrdup (path);
}


/*--------------------------------------------------------------------------
 * Files.
 *--------------------------------------------------------------------------*/

static uint32_t crc_table[256];

static void
init_crc (void)
{
	if (crc_table[1] != 0)
		return;
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i << 24;
		for (int k = 0; k < 8; k++)
			c = c & 0x80000000u ? (c << 1) ^ 0x04C11DB7u : c << 1;
		crc_table[i] = c;
	}
}

/* The checksum of `data', as printed by POSIX cksum: "CRC LENGTH". */
char *
remote_checksum (const char *data, size_t len)
{
	init_crc ();
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++)
		crc = (crc << 8) ^ crc_table[((crc >> 24) ^ (unsigned char) data[i]) & 0xff];
	for (size_t n = len; n > 0; n >>= 8)
		crc = (crc << 8) ^ crc_table[((crc >> 24) ^ (n & 0xff)) & 0xff];
	return xasprintf ("%lu %zu", (unsigned long) (~crc & 0xffffffffu), len);
}

#define READ_SCRIPT \
	"f=$1; printf \"XZ1\\n\"; " \
	"if [ -d \"$f\" ]; then echo D; exit 0; fi; " \
	"[ -e \"$f\" ] || { echo N; exit 0; }; " \
	"[ -f \"$f\" ] || { echo S; exit 0; }; " \
	"[ -r \"$f\" ] || { echo R; exit 0; }; " \
	"if [ -w \"$f\" ]; then echo F1; else echo F0; fi; " \
	"exec cat \"$f\""

/* Read the file `path' of `r' (absolute). For a file, its contents go
   to `content' and `*writable' says whether it may be written. */
enum RemoteKind
remote_read (Remote *r, const char *path, StrBuf *content, bool *writable)
{
	const char *args[] = { path, NULL };
	RunResult res;
	size_t at;
	char *what = xasprintf ("Reading %s", path);
	int st = run_script (r, READ_SCRIPT, args, NULL, 0, &res, &at, what);
	free (what);
	enum RemoteKind kind = REMOTE_ERROR;
	if (st >= 0) {
		const char *a = res.out.s + at;
		size_t alen = res.out.len - at;
		char tag = alen >= 2 ? a[0] : '\0';
		if (st != 0) {
			char *line = last_line (&res.err);
			set_error ("Reading %s on ssh:%s failed%s%s", path, r->label, line[0] != '\0' ? ": " : "", line);
			free (line);
		} else if (tag == 'D')
			kind = REMOTE_DIR;
		else if (tag == 'N')
			kind = REMOTE_MISSING;
		else if (tag == 'S')
			set_error ("%s on ssh:%s is not a regular file", path, r->label);
		else if (tag == 'R')
			set_error ("%s on ssh:%s: Permission denied", path, r->label);
		else if (tag == 'F' && alen >= 3 && a[2] == '\n') {
			*writable = a[1] == '1';
			sb_addn (content, a + 3, alen - 3);
			kind = REMOTE_FILE;
		} else
			set_error ("Reading %s on ssh:%s failed: unexpected answer", path, r->label);
	}
	result_free (&res);
	return kind;
}

#define WRITE_SCRIPT \
	"f=$1; printf \"XZ1\\n\"; " \
	"if [ -d \"$f\" ]; then exit 74; fi; " \
	"t=$(mktemp \"${TMPDIR:-/tmp}/xzile.XXXXXX\") || exit 70; " \
	"cat > \"$t\" || { rm -f \"$t\"; exit 71; }; " \
	"n=$(wc -c < \"$t\"); " \
	"[ $((n)) -eq \"$3\" ] || { rm -f \"$t\"; exit 72; }; " \
	"if [ -e \"$f\" ]; then c=$(cksum < \"$f\" | { read a b r; echo \"$a $b\"; }); else c=-; fi; " \
	"[ \"$2\" = \"*\" ] || [ \"$c\" = \"$2\" ] || { rm -f \"$t\"; echo \"$c\"; exit 73; }; " \
	"if [ \"$4\" = 1 ] && [ -f \"$f\" ]; then cp -p \"$f\" \"$f~\" || echo nobackup; fi; " \
	"cat \"$t\" > \"$f\" || { rm -f \"$t\"; exit 75; }; " \
	"rm -f \"$t\"; " \
	"cksum < \"$f\" | { read a b r; echo \"$a $b\"; }"

/* Write `data' to `path' on `r', if the file there still has the
   checksum `expect' (NULL if it did not exist, "*" to write anyway).
   With `backup', the old file is first copied to `path~'. On success,
   `*sum' is the checksum of the file written; on conflict, of the
   file found. */
enum RemoteWrite
remote_write (Remote *r, const char *path, const char *expect, bool backup,
	      const char *data, size_t len, char **sum, bool *backed_up)
{
	char *n = xasprintf ("%zu", len);
	const char *args[] = { path, expect != NULL ? expect : "-", n, backup ? "1" : "0", NULL };
	RunResult res;
	size_t at;
	char *what = xasprintf ("Writing %s", path);
	int st = run_script (r, WRITE_SCRIPT, args, data, len, &res, &at, what);
	free (what);
	free (n);
	*sum = NULL;
	*backed_up = false;
	enum RemoteWrite ret = REMOTE_WRITE_ERROR;
	if (st >= 0) {
		StrBuf ans;
		sb_init (&ans);
		sb_addn (&ans, res.out.s + at, res.out.len - at);
		char *line = last_line (&ans);
		char *err = last_line (&res.err);
		const char *sep = err[0] != '\0' ? ": " : "";
		switch (st) {
		case 0:
			if (line[0] != '\0' && isdigit ((unsigned char) line[0])) {
				*sum = xstrdup (line);
				*backed_up = backup && strstr (ans.s, "nobackup") == NULL;
				ret = REMOTE_WRITE_OK;
			} else
				set_error ("Writing %s on ssh:%s: the result could not be checked", path, r->label);
			break;
		case 73:
			*sum = xstrdup (line);
			ret = REMOTE_WRITE_CONFLICT;
			break;
		case 70:
			set_error ("Writing %s on ssh:%s: cannot make a temporary file%s%s", path, r->label, sep, err);
			break;
		case 72:
			set_error ("Writing %s on ssh:%s: the transfer was incomplete", path, r->label);
			break;
		case 74:
			set_error ("Writing %s on ssh:%s: it is a directory", path, r->label);
			break;
		default:
			set_error ("Writing %s on ssh:%s failed%s%s", path, r->label, sep, err);
			break;
		}
		free (line);
		free (err);
		sb_free (&ans);
	}
	result_free (&res);
	return ret;
}


/*--------------------------------------------------------------------------
 * Directories.
 *--------------------------------------------------------------------------*/

#define LIST_SCRIPT \
	"cd \"$1\" || exit 66; printf \"XZ1\\n\"; d=$2; set -- . ..; " \
	"for f in .* *; do case $f in .|..) continue;; esac; " \
	"if [ -e \"$f\" ] || [ -L \"$f\" ]; then set -- \"$@\" \"$f\"; fi; done; " \
	"for f in \"$@\"; do if [ -d \"$f\" ]; then printf \"d%s\\0\" \"$f\"; " \
	"else printf \"f%s\\0\" \"$f\"; fi; done; printf \"\\0\"; " \
	"if [ \"$d\" = l ]; then LC_ALL=C ls -ld -- \"$@\" 2>/dev/null; fi; exit 0"

void
remote_entries_free (RemoteEntry *v, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free (v[i].name);
		free (v[i].link);
		for (size_t k = 0; k < REMOTE_INFO_FIELDS; k++)
			free (v[i].info[k]);
	}
	free (v);
}

static const char *
token (const char *p, const char *end, char **out)
{
	while (p < end && *p == ' ')
		p++;
	const char *s = p;
	while (p < end && *p != ' ' && *p != '\n')
		p++;
	if (p == s)
		return NULL;
	*out = xstrndup (s, (size_t) (p - s));
	return p;
}

/* Parse the `ls -ld' line for `e' at `*pp', which is consumed if it is
   for that entry or cannot be read. */
static void
parse_ls_line (RemoteEntry *e, const char **pp, const char *end)
{
	const char *p = *pp;
	char *t[9] = { NULL };
	size_t nt = 0;
	bool ok = true;
	while (ok && nt < 8) {
		p = token (p, end, &t[nt]);
		if (p == NULL)
			ok = false;
		else if (nt == 4 && t[4][strlen (t[4]) - 1] == ',') {
			/* A device: major, minor. */
			char *minor;
			p = token (p, end, &minor);
			if (p == NULL)
				ok = false;
			else {
				char *both = xasprintf ("%s %s", t[4], minor);
				free (t[4]);
				free (minor);
				t[4] = both;
				nt++;
			}
		} else
			nt++;
	}
	size_t nlen = strlen (e->name);
	if (!ok || p >= end || *p != ' ' || strlen (t[0]) < 10) {
		const char *nl = memchr (*pp, '\n', (size_t) (end - *pp));
		*pp = nl != NULL ? nl + 1 : end;
	} else if ((size_t) (end - p - 1) >= nlen && memcmp (p + 1, e->name, nlen) == 0 &&
		   (p + 1 + nlen == end || p[1 + nlen] == '\n' ||
		    (t[0][0] == 'l' && end - (p + 1 + nlen) >= 4 && memcmp (p + 1 + nlen, " -> ", 4) == 0))) {
		p += 1 + nlen;
		if (end - p >= 4 && memcmp (p, " -> ", 4) == 0) {
			const char *nl = memchr (p, '\n', (size_t) (end - p));
			const char *le = nl != NULL ? nl : end;
			e->link = xstrndup (p + 4, (size_t) (le - p - 4));
			p = le;
		}
		const char *nl = memchr (p, '\n', (size_t) (end - p));
		*pp = nl != NULL ? nl + 1 : end;
		t[0][10] = '\0';
		e->info[0] = xstrdup (t[0]);
		for (size_t k = 1; k < 5; k++)
			e->info[k] = xstrdup (t[k]);
		e->info[5] = xasprintf ("%.3s %2s %5s", t[5], t[6], t[7]);
	}
	for (size_t k = 0; k < 9; k++)
		free (t[k]);
}

static int
compare_entry_names (const void *a, const void *b)
{
	return strcmp (((const RemoteEntry *) a)->name, ((const RemoteEntry *) b)->name);
}

/* List the directory `dir' (absolute) of `r', sorted by name, with
   `.' and `..'. With `details', also ls's view of each entry. */
bool
remote_list (Remote *r, const char *dir, bool details, RemoteEntry **out, size_t *nout)
{
	const char *args[] = { dir, details ? "l" : "-", NULL };
	RunResult res;
	size_t at;
	char *what = xasprintf ("Listing %s", dir);
	int st = run_script (r, LIST_SCRIPT, args, NULL, 0, &res, &at, what);
	free (what);
	*out = NULL;
	*nout = 0;
	if (st != 0) {
		if (st == 66)
			set_error ("%s on ssh:%s: No such directory, or it cannot be opened", dir, r->label);
		else if (st > 0)
			set_error ("Listing %s on ssh:%s failed (exit status %d)", dir, r->label, st);
		result_free (&res);
		return false;
	}

	RemoteEntry *v = NULL;
	size_t n = 0, cap = 0;
	const char *p = res.out.s + at, *end = res.out.s + res.out.len;
	bool terminated = false;
	while (p < end) {
		const char *z = memchr (p, '\0', (size_t) (end - p));
		if (z == NULL)
			break;
		if (z == p) {
			p++;
			terminated = true;
			break;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			v = xrealloc (v, cap * sizeof (RemoteEntry));
		}
		RemoteEntry *e = &v[n++];
		memset (e, 0, sizeof *e);
		e->dir = p[0] == 'd';
		e->name = xstrndup (p + 1, (size_t) (z - p - 1));
		p = z + 1;
	}
	if (!terminated) {
		set_error ("Listing %s on ssh:%s failed: the answer was cut short", dir, r->label);
		remote_entries_free (v, n);
		result_free (&res);
		return false;
	}
	qsort (v, n, sizeof (RemoteEntry), compare_entry_names);
	for (size_t i = 0; details && i < n && p < end; i++)
		parse_ls_line (&v[i], &p, end);
	result_free (&res);
	*out = v;
	*nout = n;
	return true;
}


/*--------------------------------------------------------------------------
 * Buffers.
 *--------------------------------------------------------------------------*/

/* "ssh:LABEL:PATH" for a remote buffer, or NULL. */
char *
remote_describe (const Buffer *bp)
{
	if (bp->remote == NULL)
		return NULL;
	const char *path = bp->remote_path != NULL ? bp->remote_path : dired_directory (bp);
	if (path == NULL)
		path = shell_directory (bp);
	if (path == NULL)
		path = magit_directory (bp);
	char *shown = path != NULL ? remote_compact_path (bp->remote, path) : xstrdup ("");
	char *s = xasprintf ("ssh:%s:%s", bp->remote->label, shown);
	free (shown);
	return s;
}

/* The remote directory of `bp' (its file's, the one it lists, its
   terminal's or its repository's), or NULL if it is not remote. */
char *
remote_buffer_dir (const Buffer *bp)
{
	if (bp->remote == NULL)
		return NULL;
	if (bp->remote_path != NULL)
		return path_dirname (bp->remote_path);
	const char *dir = dired_directory (bp);
	if (dir == NULL)
		dir = shell_directory (bp);
	if (dir == NULL)
		dir = magit_directory (bp);
	return xstrdup (dir != NULL ? dir : "~/");
}

/*--------------------------------------------------------------------------
 * Shell commands and terminals.
 *--------------------------------------------------------------------------*/

/* The command's output and error go to the output, between the marker
   and a trailer giving its exit status: an answer without the trailer
   was cut short, whatever ssh's exit status. Before the marker, the
   script names on standard error its process group, which sshd made
   for this session alone (the script or its parent leads it), so that
   a cancelled command can be sent SIGTERM there too. */
#define COMMAND_SCRIPT \
	"cd \"$1\" || exit 66; " \
	"g=$(ps -o pgid= -p $$ 2>/dev/null | tr -d \" \"); " \
	"if [ -n \"$g\" ] && { [ \"$g\" = $$ ] || [ \"$g\" = \"$PPID\" ]; }; then echo \"XZG $g\" >&2; fi; " \
	"printf \"XZ1\\n\"; " \
	"sh -c \"$2\" 2>&1; " \
	"printf \"\\nXZ2 %d\\n\" $?"

#define KILL_SCRIPT "kill -s TERM -- -\"$1\" 2>/dev/null; exit 0"

/* After a cancelled or abandoned command, try to stop what it left
   running on `r' with SIGTERM, if its process group is known. This
   may not reach it, and it may ignore the signal. The error is kept. */
static void
stop_remote_command (Remote *r, const StrBuf *err)
{
	const char *p = err->s;
	while (p != NULL && *p != '\0' && strncmp (p, "XZG ", 4) != 0) {
		p = strchr (p, '\n');
		if (p != NULL)
			p++;
	}
	if (p == NULL || *p == '\0')
		return;
	char *pgid = xstrndup (p + 4, strcspn (p + 4, "\r\n"));
	long n;
	if (parse_long (pgid, &n) && n > 1) {
		char *saved = xstrdup_or_null (last_error);
		bool cancelled = last_cancelled;
		const char *args[] = { pgid, NULL };
		RunResult res;
		run_remote (r, KILL_SCRIPT, args, NULL, 0, &res, "Stopping the command", 5000, false);
		result_free (&res);
		free (last_error);
		last_error = saved;
		last_cancelled = cancelled;
	}
	free (pgid);
}

/* The exit status of the command `what' that COMMAND_SCRIPT or
   RUN_SCRIPT ran on `r' in `adir', given what ssh gave back (`ran'
   if it finished) in `res'; its standard output up to the trailer goes
   to `output'. Returns -1 with the error set if the command could not
   be run in `adir' or did not finish, trying to stop it if it was
   abandoned. */
static int
command_status (Remote *r, bool ran, const RunResult *res, const char *adir, const char *what, StrBuf *output)
{
	size_t at;
	int ret = -1;
	if (!ran)
		stop_remote_command (r, &res->err);
	else if (!find_answer (&res->out, &at)) {
		if (res->status == 255)
			ssh_failed (r, &res->err);
		else if (res->status == 66)
			set_error ("%s on ssh:%s: No such directory, or it cannot be opened", adir, r->label);
		else {
			char *line = last_line (&res->err);
			set_error ("%s on ssh:%s failed: %s", what, r->label,
				   line[0] != '\0' ? line : "the remote shell gave no answer");
			free (line);
		}
	} else {
		/* The trailer is the last line, after a newline of its own. */
		const char *s = res->out.s + at;
		size_t len = res->out.len - at, e = len;
		if (e > 0 && s[e - 1] == '\n')
			e--;
		size_t b = e;
		while (b > 0 && s[b - 1] != '\n')
			b--;
		long st;
		char *tail = xstrndup (s + b, e - b);
		if (b > 0 && e < len && str_has_prefix (tail, "XZ2 ") && parse_long (tail + 4, &st) && st >= 0) {
			sb_addn (output, s, b - 1);
			ret = (int) st;
			note_used (r);
		} else if (res->status == 255)
			set_error ("%s on ssh:%s did not finish: it was killed, or the connection was lost",
				   what, r->label);
		else
			set_error ("%s on ssh:%s did not finish (exit status %d)", what, r->label, res->status);
		free (tail);
	}
	return ret;
}

/* Run the shell command `cmd' with sh on `r' in `dir', feeding it
   `input', with its standard output and error in `output'. Returns
   its exit status, or -1 with the error set (and `output' unchanged)
   if it could not be run in `dir' or did not finish. */
int
remote_shell_command (Remote *r, const char *dir, const char *cmd, const char *input, size_t input_len,
		      StrBuf *output)
{
	clear_error ();
	char *adir = remote_expand_path (r, dir);
	if (adir == NULL)
		return -1;
	const char *args[] = { adir, cmd, NULL };
	RunResult res;
	bool ok = run_remote (r, COMMAND_SCRIPT, args, input, input_len, &res, "Shell command",
			      timeout_seconds () * 1000, true);
	int ret = command_status (r, ok, &res, adir, "Shell command", output);
	debug_log ("remote", "shell command on %s: %s", r->label, ret >= 0 ? "finished" : remote_last_error ());
	result_free (&res);
	free (adir);
	return ret;
}

/* As COMMAND_SCRIPT, for the program and arguments after the
   directory, which no shell interprets. Its error output goes to
   standard error, after a marker there too, so that it is kept apart
   from its output. */
#define RUN_SCRIPT \
	"cd \"$1\" || exit 66; shift; " \
	"g=$(ps -o pgid= -p $$ 2>/dev/null | tr -d \" \"); " \
	"if [ -n \"$g\" ] && { [ \"$g\" = $$ ] || [ \"$g\" = \"$PPID\" ]; }; then echo \"XZG $g\" >&2; fi; " \
	"printf \"XZ1\\n\"; printf \"XZ1\\n\" >&2; " \
	"\"$@\"; " \
	"printf \"\\nXZ2 %d\\n\" $?"

/* Run the program `argv' (found in the remote PATH) on `r' in `dir',
   with its arguments as they are, feeding it `input'. Its standard
   output goes to `out' and its standard error to `err' (either may be
   NULL), byte for byte. Returns its exit status, or -1 with the error
   set (and `out' and `err' unchanged) if it could not be run in `dir'
   or did not finish; as for remote_shell_command, C-g cancels it,
   remote-timeout bounds it, and XZile then tries to stop it there.
   `what' names it in messages. */
int
remote_run (Remote *r, const char *dir, const char *const *argv, const char *input, size_t input_len,
	    StrBuf *out, StrBuf *err, const char *what)
{
	clear_error ();
	char *adir = remote_expand_path (r, dir);
	if (adir == NULL)
		return -1;
	size_t n = 0;
	while (argv[n] != NULL)
		n++;
	const char **args = xmalloc ((n + 2) * sizeof (char *));
	args[0] = adir;
	memcpy (args + 1, argv, (n + 1) * sizeof (char *));
	RunResult res;
	bool ok = run_remote (r, RUN_SCRIPT, args, input, input_len, &res, what, timeout_seconds () * 1000, true);
	free (args);
	StrBuf output;
	sb_init (&output);
	int ret = command_status (r, ok, &res, adir, what, &output);
	if (ret >= 0) {
		size_t at;
		if (out != NULL)
			sb_addn (out, output.s, output.len);
		if (err != NULL && find_answer (&res.err, &at))
			sb_addn (err, res.err.s + at, res.err.len - at);
	}
	if (ret >= 0)
		debug_log ("remote", "%s on %s: exit status %d", what, r->label, ret);
	else
		debug_log ("remote", "%s on %s: %s", what, r->label, remote_last_error ());
	sb_free (&output);
	result_free (&res);
	free (adir);
	return ret;
}

#define START_SCRIPT "cd \"$1\" || exit 66; shift; exec \"$@\""

/* The arguments to run the program `argv' on `r' in `dir' (absolute),
   with its arguments as they are, as a process of its own whose
   lifetime remote-timeout does not bound. Its standard input is
   ssh's; its standard output and error are ssh's. The strings belong
   to `keep'. */
const char **
remote_command_argv (Remote *r, const char *dir, const char *const *argv, StrVec *keep)
{
	size_t n = 0;
	while (argv[n] != NULL)
		n++;
	const char **args = xmalloc ((n + 2) * sizeof (char *));
	args[0] = dir;
	memcpy (args + 1, argv, (n + 1) * sizeof (char *));
	char *cmd = script_command (START_SCRIPT, args);
	free (args);
	const char **ssh = command_argv (r, cmd, false, keep);
	free (cmd);
	debug_log ("remote", "starting %s on %s in %s", argv[0], r->label, dir);
	return ssh;
}

#define TERMINAL_CHECK_SCRIPT \
	"cd \"$1\" || exit 66; printf \"XZ1\\n%s\\n\" \"${SHELL:-/bin/sh}\"; uname -n 2>/dev/null; exit 0"

/* Check that a terminal can be started on `r' in `dir': its absolute
   name goes to `*adir', and the remote user's $SHELL to `*shell'. */
bool
remote_terminal_check (Remote *r, const char *dir, char **adir, char **shell)
{
	clear_error ();
	*adir = *shell = NULL;
	char *d = remote_expand_path (r, dir);
	if (d == NULL)
		return false;
	const char *args[] = { d, NULL };
	RunResult res;
	size_t at;
	int st = run_script (r, TERMINAL_CHECK_SCRIPT, args, NULL, 0, &res, &at, "Starting a terminal");
	if (st == 0) {
		char *a = xstrndup (res.out.s + at, res.out.len - at);
		char *p = a, *sh = strsep (&p, "\n"), *node = p != NULL ? strsep (&p, "\n") : NULL;
		*shell = xstrdup (sh[0] == '/' ? sh : "/bin/sh");
		if (node != NULL && node[0] != '\0') {
			free (r->nodename);
			r->nodename = xstrdup (node);
		}
		free (a);
		*adir = d;
		d = NULL;
	} else if (st == 66)
		set_error ("%s on ssh:%s: No such directory, or it cannot be opened", d, r->label);
	else if (st > 0)
		set_error ("ssh %s: the remote shell failed (exit status %d)", r->label, st);
	result_free (&res);
	free (d);
	return *adir != NULL;
}

/* The program runs as a local one does (see terminal.c); without one,
   the remote user's $SHELL runs as a login shell. */
#define TERMINAL_SCRIPT \
	"cd \"$1\" || exit 66; INSIDE_XZILE=1; COLORTERM=truecolor; export INSIDE_XZILE COLORTERM; " \
	"if [ $# -gt 1 ]; then exec sh -c \"exec $2\"; fi; exec \"${SHELL:-/bin/sh}\" -l"

/* The arguments to run `program' (NULL for the login shell) on a
   pseudo-terminal of `r', in `dir' (absolute). Its lifetime is not
   bounded by remote-timeout. The strings belong to `keep'. */
const char **
remote_terminal_argv (Remote *r, const char *dir, const char *program, StrVec *keep)
{
	const char *args[] = { dir, program, NULL };
	char *cmd = script_command (TERMINAL_SCRIPT, args);
	const char **argv = command_argv (r, cmd, true, keep);
	free (cmd);
	debug_log ("remote", "terminal on %s in %s: %s", r->label, dir, program != NULL ? program : "login shell");
	return argv;
}

/* Whether `a' and `b' name the same host: equal, ignoring case, or
   one a short name (without a dot) and the other that name with a
   domain. Two names with domains must be equal. */
static bool
host_eq (const char *a, const char *b)
{
	if (strcasecmp (a, b) == 0)
		return true;
	size_t na = strlen (a), nb = strlen (b);
	const char *s = na < nb ? a : b, *l = na < nb ? b : a;
	size_t ns = MIN (na, nb);
	return ns > 0 && strchr (s, '.') == NULL && strncasecmp (s, l, ns) == 0 && l[ns] == '.';
}

/* Whether `host', as a terminal on `r' reports it, is that machine.
   An empty host or localhost is the terminal's own. */
bool
remote_same_host (const Remote *r, const char *host)
{
	if (host[0] == '\0' || strcasecmp (host, "localhost") == 0)
		return true;
	return host_eq (host, r->host) || (r->nodename != NULL && host_eq (host, r->nodename));
}

/* Whether `host', as a terminal here reports it, is this machine. */
bool
remote_host_is_local (const char *host)
{
	char name[256];
	if (host[0] == '\0' || host_eq (host, "localhost") || gethostname (name, sizeof name) != 0)
		return true;
	name[sizeof name - 1] = '\0';
	return host_eq (host, name);
}

/* If `bp' is remote, say that `what' is not available for it. */
bool
remote_refuse (const Buffer *bp, const char *what)
{
	if (bp == NULL || bp->remote == NULL)
		return false;
	minibuf_error ("%s is not available in remote buffers (ssh:%s)", what, bp->remote->label);
	debug_log ("remote", "refused %s in %s", what, bp->name);
	return true;
}

/* Close the master connections. */
void
remote_shutdown (void)
{
	for (Remote *r = remotes; r != NULL; r = r->next) {
		if (r->control == NULL)
			continue;
		struct stat st;
		if (lstat (r->control, &st) == 0) {
			const char *opts[] = { "-O", "exit", NULL };
			StrVec keep = { NULL, 0, 0 };
			const char **argv = ssh_argv (r, opts, NULL, &keep);
			RunResult res;
			run_argv (argv, NULL, 0, &res, "Disconnecting", r->label, 3000, false);
			result_free (&res);
			free (argv);
			sv_clear (&keep);
			free (keep.v);
			unlink (r->control);
		}
	}
	if (control_dir != NULL) {
		rmdir (control_dir);
		free (control_dir);
		control_dir = NULL;
	}
}

/*--------------------------------------------------------------------------
 * Saved locations.
 *
 * ~/.xzile.d/locations remembers, across sessions, the machines
 * connected to and the directory last browsed on each, and here. It
 * holds only what was typed or chosen: never passwords, keys or
 * control sockets. The first line is the format version; each other
 * line is a record of tab-separated fields, with backslash, tab,
 * newline and carriage return written as \\, \t, \n and \r:
 *
 *   local DIR
 *   ssh HOST USER PORT DIR      (USER, PORT or DIR empty if not known)
 *
 * Most recent first. Records that do not parse are ignored; a file of
 * another version is left alone. Each change is made holding an
 * fcntl(2) lock on locations.lock: the file is read again, the one
 * location changed, and the file replaced by renaming a new one over
 * it, so that sessions sharing it neither lose each other's changes
 * nor see a partial file.
 *--------------------------------------------------------------------------*/

#define STATE_DIR "." PACKAGE ".d"
#define STATE_FILE "locations"
#define STATE_HEADER PACKAGE "-locations 1"
#define MAX_SAVED_CONNECTIONS 32
#define MAX_STATE_SIZE (256u * 1024)
#define MAX_STATE_FIELD 4096
/* How long to wait for another session to finish saving, in ms. */
#define STATE_LOCK_WAIT 2000

typedef struct {
	char *host;		/* NULL for this machine. */
	char *user, *port;	/* NULL for ssh's defaults. */
	char *dir;		/* Absolute, or NULL if not known. */
} SavedLocation;

typedef struct {
	SavedLocation *v;
	size_t n;
} SavedList;

static SavedList saved;		/* Most recent first. */
static bool state_enabled;

static void
saved_clear (SavedList *l)
{
	for (size_t i = 0; i < l->n; i++) {
		free (l->v[i].host);
		free (l->v[i].user);
		free (l->v[i].port);
		free (l->v[i].dir);
	}
	free (l->v);
	l->v = NULL;
	l->n = 0;
}

static long
saved_find (const SavedList *l, const char *host, const char *user, const char *port)
{
	for (size_t i = 0; i < l->n; i++)
		if (opt_eq (l->v[i].host, host) && opt_eq (l->v[i].user, user) && opt_eq (l->v[i].port, port))
			return (long) i;
	return -1;
}

static void
saved_append (SavedList *l, const char *host, const char *user, const char *port, const char *dir)
{
	l->v = xrealloc (l->v, (l->n + 1) * sizeof (SavedLocation));
	SavedLocation *loc = &l->v[l->n++];
	loc->host = xstrdup_or_null (host);
	loc->user = xstrdup_or_null (user);
	loc->port = xstrdup_or_null (port);
	loc->dir = xstrdup_or_null (dir);
}

/* Make the location first, with `dir' if not NULL. */
static void
saved_touch (SavedList *l, const char *host, const char *user, const char *port, const char *dir)
{
	SavedLocation loc;
	long i = saved_find (l, host, user, port);
	if (i >= 0) {
		loc = l->v[i];
		memmove (l->v + i, l->v + i + 1, (l->n - (size_t) i - 1) * sizeof (SavedLocation));
		l->n--;
	} else {
		loc.host = xstrdup_or_null (host);
		loc.user = xstrdup_or_null (user);
		loc.port = xstrdup_or_null (port);
		loc.dir = NULL;
	}
	if (dir != NULL) {
		free (loc.dir);
		loc.dir = xstrdup (dir);
	}
	l->v = xrealloc (l->v, (l->n + 1) * sizeof (SavedLocation));
	memmove (l->v + 1, l->v, l->n * sizeof (SavedLocation));
	l->v[0] = loc;
	l->n++;

	size_t nssh = 0;
	for (size_t k = 0; k < l->n; k++)
		if (l->v[k].host != NULL && ++nssh > MAX_SAVED_CONNECTIONS) {
			free (l->v[k].host);
			free (l->v[k].user);
			free (l->v[k].port);
			free (l->v[k].dir);
			memmove (l->v + k, l->v + k + 1, (l->n - k - 1) * sizeof (SavedLocation));
			l->n--;
			break;
		}
}

static void
escape_field (StrBuf *sb, const char *s)
{
	for (; s != NULL && *s != '\0'; s++)
		switch (*s) {
		case '\\': sb_add (sb, "\\\\"); break;
		case '\t': sb_add (sb, "\\t"); break;
		case '\n': sb_add (sb, "\\n"); break;
		case '\r': sb_add (sb, "\\r"); break;
		default: sb_addc (sb, *s);
		}
}

/* Decode the field [s, end); NULL if it is malformed. An empty field
   gives "". */
static char *
unescape_field (const char *s, const char *end)
{
	if (end - s > MAX_STATE_FIELD * 2)
		return NULL;
	StrBuf sb;
	sb_init (&sb);
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

/* The record `line' [line, end) added to `l'; false if malformed. */
static bool
parse_record (SavedList *l, const char *line, const char *end)
{
	char *f[6];
	size_t n = 0;
	bool ok = true;
	for (const char *p = line; ok;) {
		const char *tab = memchr (p, '\t', (size_t) (end - p));
		const char *fe = tab != NULL ? tab : end;
		if (n == 6 || (f[n] = unescape_field (p, fe)) == NULL)
			ok = false;
		else
			n++;
		if (tab == NULL)
			break;
		p = tab + 1;
	}

	if (ok) {
		ok = false;
		if (n == 2 && strcmp (f[0], "local") == 0 && f[1][0] == '/') {
			if (saved_find (l, NULL, NULL, NULL) < 0)
				saved_append (l, NULL, NULL, NULL, f[1]);
			ok = true;
		} else if (n == 5 && strcmp (f[0], "ssh") == 0 && valid_host (f[1]) &&
			   (f[2][0] == '\0' || valid_user (f[2])) && (f[3][0] == '\0' || valid_port (f[3])) &&
			   (f[4][0] == '\0' || f[4][0] == '/')) {
			const char *user = f[2][0] != '\0' ? f[2] : NULL, *port = f[3][0] != '\0' ? f[3] : NULL;
			size_t nssh = 0;
			for (size_t k = 0; k < l->n; k++)
				nssh += l->v[k].host != NULL;
			if (saved_find (l, f[1], user, port) < 0 && nssh < MAX_SAVED_CONNECTIONS)
				saved_append (l, f[1], user, port, f[4][0] != '\0' ? f[4] : NULL);
			ok = true;
		}
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

/* Lock the state in `dir' for writing, waiting at most
   STATE_LOCK_WAIT ms. The lock is on a file of its own, as the state
   file is replaced, never written in place. Returns the descriptor
   to close to unlock, or -1 with `*why' set. */
static int
lock_state (const char *dir, char **why)
{
	char *path = path_join (dir, STATE_FILE ".lock");
	int fd = open (path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
	free (path);
	if (fd < 0) {
		*why = xasprintf ("cannot open ~/" STATE_DIR "/" STATE_FILE ".lock: %s", strerror (errno));
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
			*why = busy ? xstrdup ("another session kept ~/" STATE_DIR "/" STATE_FILE " locked")
				: xasprintf ("cannot lock ~/" STATE_DIR "/" STATE_FILE ".lock: %s", strerror (e));
			close (fd);
			return -1;
		}
		struct timespec ts = { 0, 20L * 1000 * 1000 };
		nanosleep (&ts, NULL);
	}
	return fd;
}

/* Read the state file into `l' (empty if there is none), with
   `*text' its contents. Returns false, with `*why' set if not NULL,
   if the file is there but must not be replaced: unreadable, too
   large, not a regular file, or of another version. */
static bool
read_state (SavedList *l, char **text, char **why)
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
					debug_log ("remote", "saved locations: ignored line %zu of %s", lineno + 1, file);
				p = e + 1;
			}
		}
	}
	if (reason != NULL) {
		debug_log ("remote", "saved locations: not replacing %s: it is %s", file, reason);
		if (why != NULL)
			*why = xasprintf ("~/" STATE_DIR "/" STATE_FILE " is %s", reason);
	}
	*text = s;
	free (file);
	return reason == NULL;
}

static char *
state_text (const SavedList *l)
{
	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, STATE_HEADER "\n");
	for (size_t i = 0; i < l->n; i++) {
		const SavedLocation *loc = &l->v[i];
		if (loc->host == NULL) {
			if (loc->dir == NULL)
				continue;
			sb_add (&sb, "local\t");
		} else {
			sb_add (&sb, "ssh\t");
			escape_field (&sb, loc->host);
			sb_addc (&sb, '\t');
			escape_field (&sb, loc->user);
			sb_addc (&sb, '\t');
			escape_field (&sb, loc->port);
			sb_addc (&sb, '\t');
		}
		escape_field (&sb, loc->dir);
		sb_addc (&sb, '\n');
	}
	return sb_take (&sb);
}

/* Replace the state file in `dir' with `text', through a private
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
		*why = xasprintf ("cannot write ~/" STATE_DIR "/" STATE_FILE ": %s", strerror (errno));
	free (tmp);
	free (file);
	return ok;
}

/* Remember locations across sessions, as `-q' does not. */
void
locations_load (void)
{
	state_enabled = true;
	char *text;
	saved_clear (&saved);
	read_state (&saved, &text, NULL);
	free (text);
	debug_log ("remote", "saved locations: %zu loaded", saved.n);
}

static bool state_warned;
static char *state_warning;	/* Not yet shown by a picker. */

/* Warn, once until a save works again, that the state could not be
   saved; what was opened stays open. */
static void
state_unsaved (const char *why)
{
	debug_log ("remote", "saved locations: not saved: %s", why);
	if (!state_warned) {
		state_warned = true;
		state_warning = xasprintf ("Warning: locations not saved: %s", why);
		debug_log ("remote", "%s", state_warning);
		minibuf_write ("%s", state_warning);
	}
}

/* The warning given by the last location_note, if any, for a picker
   about to cover the minibuffer to show instead. */
char *
location_take_warning (void)
{
	char *s = state_warning;
	state_warning = NULL;
	return s;
}

/* Note that `dir' (absolute; NULL if not known) was browsed on `r'
   (NULL for this machine), or for a machine, that it was connected
   to. Saved at once if the state is kept across sessions: under the
   lock, the file is read again and only this location changed in it,
   so that sessions sharing it keep each other's changes. */
void
location_note (Remote *r, const char *dir)
{
	if (dir != NULL && dir[0] != '/')
		dir = NULL;
	if (r == NULL && dir == NULL)
		return;
	const char *host = r != NULL ? r->host : NULL, *user = r != NULL ? r->user : NULL,
		*port = r != NULL ? r->port : NULL;
	saved_touch (&saved, host, user, port, dir);
	free (state_warning);
	state_warning = NULL;
	if (!state_enabled)
		return;

	char *why = NULL;
	char *sdir = private_state_dir (&why);
	int lock = sdir != NULL ? lock_state (sdir, &why) : -1;
	if (lock >= 0) {
		SavedList cur = { NULL, 0 };
		char *old;
		if (read_state (&cur, &old, &why)) {
			saved_touch (&cur, host, user, port, dir);
			char *text = state_text (&cur);
			bool same = old != NULL && strcmp (old, text) == 0;
			if (same || write_state (sdir, text, &why)) {
				if (!same)
					debug_log ("remote", "saved location %s %s", r != NULL ? r->label : "local",
						   dir != NULL ? dir : "(connection)");
				saved_clear (&saved);
				saved = cur;
				cur.v = NULL;
				cur.n = 0;
			}
			free (text);
		}
		free (old);
		saved_clear (&cur);
		close (lock);
	}
	if (why != NULL)
		state_unsaved (why);
	else
		state_warned = false;
	free (why);
	free (sdir);
}

/* The directory last browsed on `r' (NULL for here), or NULL. */
const char *
location_dir (const Remote *r)
{
	long i = r != NULL ? saved_find (&saved, r->host, r->user, r->port) : saved_find (&saved, NULL, NULL, NULL);
	return i >= 0 ? saved.v[i].dir : NULL;
}

/* The `i'th saved machine, most recent first, or NULL. */
Remote *
remote_saved (size_t i)
{
	for (size_t k = 0; k < saved.n; k++)
		if (saved.v[k].host != NULL && i-- == 0) {
			Remote *r = remote_get (saved.v[k].host, saved.v[k].user, saved.v[k].port);
			if (r != NULL)
				return r;
			i++;
		}
	return NULL;
}
