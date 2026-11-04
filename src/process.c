/* Asynchronous subprocesses

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

/* Processes run on a pseudo-terminal or on pipes. Their output is read
   from the editor's input loop (see term.c), which calls
   process_handle_events when a process descriptor is ready, and
   process_reap when SIGCHLD arrives. */

#include "xzile.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

/* Read at most this much output in one go before redisplaying. */
#define MAX_READ_PER_EVENT (256 * 1024)

struct Process {
	Process *next;
	pid_t pid;
	int fd;			/* pty master or pipe; -1 at EOF. */
	bool exited;
	int status;
	ProcessOutputFn on_output;
	ProcessExitFn on_exit;
	void *data;
	bool detached;		/* Freed by its owner: reap silently. */
	bool finished;		/* The exit callback has run. */
};

static Process *processes;

static void
set_nonblock_cloexec (int fd)
{
	fcntl (fd, F_SETFL, fcntl (fd, F_GETFL) | O_NONBLOCK);
	fcntl (fd, F_SETFD, FD_CLOEXEC);
}

static NORETURN void
exec_child (const char *const *argv, const char *exec_path, const char *dir, const char *const *env)
{
	sigset_t all;
	sigemptyset (&all);
	sigprocmask (SIG_SETMASK, &all, NULL);
	int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH, SIGPIPE,
		       SIGCHLD, SIGHUP, SIGTERM, SIGSEGV, SIGBUS };
	for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
		signal (sigs[i], SIG_DFL);

	if (dir != NULL && chdir (dir) != 0) { /* Stay where we are. */ }
	unsetenv ("LINES");
	unsetenv ("COLUMNS");
	for (; env != NULL && *env != NULL; env++) {
		const char *eq = strchr (*env, '=');
		if (eq != NULL) {
			char *name = xstrndup (*env, (size_t) (eq - *env));
			setenv (name, eq + 1, 1);
			free (name);
		}
	}

	/* argv[0] may differ from the program, e.g. "-zsh" for a login shell. */
	const char *file = exec_path != NULL ? exec_path : argv[0];
	execvp (file, (char *const *) argv);
	fprintf (stderr, "%s: %s\n", file, strerror (errno));
	_exit (127);
}

Process *
process_start (const char *const *argv, const char *exec_path, const char *dir, bool use_pty,
	       int rows, int cols, const char *const *env,
	       ProcessOutputFn on_output, ProcessExitFn on_exit, void *data)
{
	int master = -1, out_pipe[2] = { -1, -1 };
	char *slave_name = NULL;

	if (use_pty) {
		master = posix_openpt (O_RDWR | O_NOCTTY);
		if (master < 0 || grantpt (master) != 0 || unlockpt (master) != 0 ||
		    ptsname (master) == NULL) {
			if (master >= 0)
				close (master);
			return NULL;
		}
		slave_name = xstrdup (ptsname (master));
		set_nonblock_cloexec (master);
	} else if (pipe (out_pipe) != 0)
		return NULL;

	pid_t pid = fork ();
	if (pid < 0) {
		free (slave_name);
		if (master >= 0)
			close (master);
		if (out_pipe[0] >= 0) {
			close (out_pipe[0]);
			close (out_pipe[1]);
		}
		return NULL;
	}

	if (pid == 0) {
		setsid ();
		if (use_pty) {
			int slave = open (slave_name, O_RDWR);
			if (slave < 0)
				_exit (127);
#ifdef TIOCSCTTY
			ioctl (slave, TIOCSCTTY, 0);
#endif
			struct termios t;
			if (tcgetattr (slave, &t) == 0) {
				t.c_cc[VERASE] = 0177;
#ifdef IUTF8
				t.c_iflag |= IUTF8;
#endif
				tcsetattr (slave, TCSANOW, &t);
			}
			struct winsize ws = { (unsigned short) rows, (unsigned short) cols, 0, 0 };
			ioctl (slave, TIOCSWINSZ, &ws);
			dup2 (slave, 0);
			dup2 (slave, 1);
			dup2 (slave, 2);
			if (slave > 2)
				close (slave);
		} else {
			int null = open ("/dev/null", O_RDONLY);
			if (null >= 0) {
				dup2 (null, 0);
				close (null);
			}
			dup2 (out_pipe[1], 1);
			dup2 (out_pipe[1], 2);
			close (out_pipe[0]);
			close (out_pipe[1]);
		}
		exec_child (argv, exec_path, dir, env);
	}

	free (slave_name);
	int fd = master;
	if (!use_pty) {
		close (out_pipe[1]);
		fd = out_pipe[0];
		set_nonblock_cloexec (fd);
	}

	Process *p = xzalloc (sizeof (Process));
	p->pid = pid;
	p->fd = fd;
	p->on_output = on_output;
	p->on_exit = on_exit;
	p->data = data;
	p->next = processes;
	processes = p;
	return p;
}

bool
process_alive (const Process *p)
{
	return p != NULL && !p->exited;
}

pid_t
process_pid (const Process *p)
{
	return p->pid;
}

void
process_send (Process *p, const char *s, size_t len)
{
	if (debug_enabled ("process")) {
		StrBuf sb;
		sb_init (&sb);
		for (size_t i = 0; i < len; i++)
			if ((unsigned char) s[i] < 0x20 || s[i] == 0x7f)
				sb_addf (&sb, "^%c", s[i] == 0x7f ? '?' : s[i] + '@');
			else
				sb_addc (&sb, s[i]);
		debug_log ("process", "send pid=%ld %s", p != NULL ? (long) p->pid : -1L, sb.s);
		sb_free (&sb);
	}
	while (p != NULL && p->fd >= 0 && len > 0) {
		ssize_t n = write (p->fd, s, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd pfd = { p->fd, POLLOUT, 0 };
				poll (&pfd, 1, 100);
				continue;
			}
			return;
		}
		s += n;
		len -= (size_t) n;
	}
}

void
process_signal (Process *p, int sig)
{
	if (process_alive (p) && kill (-p->pid, sig) != 0)
		kill (p->pid, sig);
}

void
process_resize (Process *p, int rows, int cols)
{
	if (p != NULL && p->fd >= 0) {
		struct winsize ws = { (unsigned short) rows, (unsigned short) cols, 0, 0 };
		ioctl (p->fd, TIOCSWINSZ, &ws);
	}
}

static void
close_fd (Process *p)
{
	if (p->fd >= 0) {
		close (p->fd);
		p->fd = -1;
	}
}

static void
unlink_process (Process *p)
{
	for (Process **pp = &processes; *pp != NULL; pp = &(*pp)->next)
		if (*pp == p) {
			*pp = p->next;
			return;
		}
}

/* Detach a process from its owner, killing it if still running. */
void
process_free (Process *p)
{
	if (p == NULL)
		return;
	close_fd (p);
	if (!p->exited) {
		process_signal (p, SIGHUP);
		p->detached = true;
		p->on_output = NULL;
		p->on_exit = NULL;
		return;
	}
	unlink_process (p);
	free (p);
}

bool
process_any_alive (void)
{
	for (Process *p = processes; p != NULL; p = p->next)
		if (!p->exited && !p->detached)
			return true;
	return false;
}

void
process_kill_all (void)
{
	for (Process *p = processes; p != NULL; p = p->next)
		process_signal (p, SIGHUP);
}

size_t
process_poll_fds (struct pollfd *fds, size_t max)
{
	size_t n = 0;
	for (Process *p = processes; p != NULL && n < max; p = p->next)
		if (p->fd >= 0) {
			fds[n].fd = p->fd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}
	return n;
}

/* Once the process has exited and its output is read, tell the owner
   (or free a detached process). */
static void
finish (Process *p)
{
	if (!p->exited || p->fd >= 0 || p->finished)
		return;
	p->finished = true;
	if (p->detached) {
		unlink_process (p);
		free (p);
	} else if (p->on_exit != NULL)
		p->on_exit (p, p->status, p->data);
}

/* Read available output, up to `limit' bytes; close the descriptor at EOF. */
static void
drain (Process *p, size_t limit)
{
	char buf[16384];
	size_t total = 0;
	while (p->fd >= 0 && total < limit) {
		ssize_t r = read (p->fd, buf, sizeof buf);
		if (r > 0) {
			total += (size_t) r;
			if (p->on_output != NULL)
				p->on_output (p, buf, (size_t) r, p->data);
			continue;
		}
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0 && errno == EAGAIN)
			break;
		/* EOF, or EIO on a pty whose other side is closed. */
		close_fd (p);
	}
}

/* Handle ready descriptors; returns true if anything was read. */
bool
process_handle_events (const struct pollfd *fds, size_t n)
{
	bool any = false;
	for (size_t i = 0; i < n; i++) {
		if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		Process *p;
		for (p = processes; p != NULL && p->fd != fds[i].fd; p = p->next)
			;
		if (p == NULL)
			continue;

		drain (p, MAX_READ_PER_EVENT);
		any = true;
		if (p->fd < 0) {
			/* Catch an exit that happened before the output was read. */
			int status;
			if (!p->exited && waitpid (p->pid, &status, WNOHANG) == p->pid) {
				p->exited = true;
				p->status = status;
			}
			finish (p);
		}
	}
	return any;
}

/* Reap exited children; returns true if any process changed state. */
bool
process_reap (void)
{
	bool any = false;
	for (Process *p = processes, *next; p != NULL; p = next) {
		next = p->next;
		int status;
		if (!p->exited && waitpid (p->pid, &status, WNOHANG) == p->pid) {
			p->exited = true;
			p->status = status;
			any = true;
			/* Collect remaining output; a grandchild may hold the
			   descriptor open, so stop reading after that. */
			drain (p, SIZE_MAX);
			close_fd (p);
			finish (p);
		}
	}
	return any;
}
