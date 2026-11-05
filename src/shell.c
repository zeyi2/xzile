/* Shell command functions.

   Copyright (c) 1997-2020 Free Software Foundation, Inc.

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
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* Run `cmd' with `/bin/sh -c', feeding it `input' (if not NULL) and
   collecting its standard output and error. */
static bool
run_command (const char *cmd, const char *input, size_t input_len, StrBuf *output)
{
	int in_pipe[2] = { -1, -1 }, out_pipe[2];
	if (pipe (out_pipe) != 0)
		return false;
	if (input != NULL && pipe (in_pipe) != 0) {
		close (out_pipe[0]);
		close (out_pipe[1]);
		return false;
	}

	pid_t pid = fork ();
	if (pid < 0) {
		close (out_pipe[0]);
		close (out_pipe[1]);
		if (input != NULL) {
			close (in_pipe[0]);
			close (in_pipe[1]);
		}
		return false;
	}

	if (pid == 0) {
		signal (SIGPIPE, SIG_DFL);
		signal (SIGINT, SIG_DFL);
		signal (SIGTSTP, SIG_DFL);
		signal (SIGWINCH, SIG_DFL);
		if (input != NULL) {
			dup2 (in_pipe[0], 0);
			close (in_pipe[0]);
			close (in_pipe[1]);
		} else {
			int null = open ("/dev/null", O_RDONLY);
			if (null >= 0) {
				dup2 (null, 0);
				close (null);
			}
		}
		dup2 (out_pipe[1], 1);
		dup2 (out_pipe[1], 2);
		close (out_pipe[0]);
		close (out_pipe[1]);
		execl ("/bin/sh", "sh", "-c", cmd, (char *) NULL);
		_exit (127);
	}

	close (out_pipe[1]);
	int in_fd = -1;
	if (input != NULL) {
		close (in_pipe[0]);
		in_fd = in_pipe[1];
		fcntl (in_fd, F_SETFL, fcntl (in_fd, F_GETFL) | O_NONBLOCK);
		if (input_len == 0) {
			close (in_fd);
			in_fd = -1;
		}
	}

	void (*old_sigpipe) (int) = signal (SIGPIPE, SIG_IGN);
	size_t written = 0;
	int out_fd = out_pipe[0];
	char buf[4096];
	while (out_fd >= 0) {
		struct pollfd fds[2];
		nfds_t nfds = 0;
		fds[nfds].fd = out_fd;
		fds[nfds].events = POLLIN;
		nfds++;
		if (in_fd >= 0) {
			fds[nfds].fd = in_fd;
			fds[nfds].events = POLLOUT;
			nfds++;
		}
		if (poll (fds, nfds, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (in_fd >= 0 && (fds[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
			ssize_t n = write (in_fd, input + written, input_len - written);
			if (n > 0)
				written += (size_t) n;
			if ((n < 0 && errno != EAGAIN && errno != EINTR) || written == input_len) {
				close (in_fd);
				in_fd = -1;
			}
		}

		if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
			ssize_t n = read (out_fd, buf, sizeof buf);
			if (n > 0)
				sb_addn (output, buf, (size_t) n);
			else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
				close (out_fd);
				out_fd = -1;
			}
		}
	}
	if (in_fd >= 0)
		close (in_fd);
	signal (SIGPIPE, old_sigpipe);

	int status;
	while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
		;
	return true;
}

static void
insert_output (void *data)
{
	buffer_insert_estr (cur_bp, estr_view ((Estr *) data));
}

static bool
pipe_command (const char *cmd, const Estr *instr, bool do_insert, bool do_replace)
{
	StrBuf output;
	sb_init (&output);
	if (cur_bp->remote != NULL) {
		/* On the buffer's machine, in its directory there. A command
		   that cannot be run or does not finish changes nothing. */
		char *dir = remote_buffer_dir (cur_bp);
		int status = remote_shell_command (cur_bp->remote, dir, cmd, instr != NULL ? instr->text : NULL,
						   instr != NULL ? instr->len : 0, &output);
		free (dir);
		if (status < 0) {
			minibuf_error ("%s", remote_last_error ());
			sb_free (&output);
			return false;
		}
	} else if (!run_command (cmd, instr != NULL ? instr->text : NULL, instr != NULL ? strlen (instr->text) : 0, &output)) {
		sb_free (&output);
		return false;
	}

	Estr *res = estr_copy (ev_of (output.s, output.len, eol_lf));
	sb_free (&output);
	estr_set_eol_from_text (res);

	if (res->len == 0)
		minibuf_write ("(Shell command succeeded with no output)");
	else {
		if (do_insert) {
			size_t del = 0;
			if (do_replace && !buffer_warn_if_no_mark (cur_bp)) {
				Region r = region_calculate ();
				buffer_goto_offset (cur_bp, r.start);
				del = region_size (r);
			}
			buffer_replace_estr (cur_bp, del, estr_view (res));
		} else {
			const char *eol_pos = strrchr (res->text, '\n');
			bool more_than_one_line = eol_pos != NULL && (size_t) (eol_pos - res->text) != res->len - 1;
			write_temp_buffer ("*Shell Command Output*", more_than_one_line, insert_output, res);
			if (!more_than_one_line)
				minibuf_write ("%s", res->text);
		}
	}

	estr_free (res);
	return true;
}

static char *
minibuf_read_shell_command (void)
{
	char *ms = minibuf_read ("Shell command: ", "");
	if (ms == NULL) {
		funcall ("keyboard-quit");
		return NULL;
	}

	if (ms[0] == '\0') {
		free (ms);
		return NULL;
	}
	return ms;
}

static bool
F_shell_command (long uniarg, Args *args)
{
	(void) uniarg;
	bool ok = true;
	char *cmd = xstrdup_or_null (args_poll (args));
	if (cmd == NULL)
		cmd = minibuf_read_shell_command ();
	bool insert;
	if (!bool_arg (args, &insert))
		insert = lastflag & FLAG_SET_UNIARG;

	if (cmd != NULL)
		ok = pipe_command (cmd, NULL, insert, false);
	free (cmd);
	return ok;
}

static bool
F_shell_command_on_region (long uniarg, Args *args)
{
	(void) uniarg;
	char *cmd = NULL;
	if (args != NULL) {
		/* Skip arguments `start' and `end' for Emacs compatibility. */
		args_poll (args);
		args_poll (args);
		cmd = xstrdup_or_null (args_poll (args));
	}
	if (cmd == NULL)
		cmd = minibuf_read_shell_command ();
	bool insert;
	if (!bool_arg (args, &insert))
		insert = lastflag & FLAG_SET_UNIARG;

	bool ok = true;
	if (cmd != NULL) {
		if (buffer_warn_if_no_mark (cur_bp))
			ok = false;
		else {
			Estr *region = buffer_get_region (cur_bp, region_calculate ());
			ok = pipe_command (cmd, region, insert, true);
			estr_free (region);
		}
	}
	free (cmd);
	return ok;
}

void
shell_init (void)
{
	lisp_func_new ("shell-command", F_shell_command, true,
		       "Execute string COMMAND in inferior shell; display output, if any.\n"
		       "With prefix argument, insert the command's output at point.\n"
		       "\n"
		       "Command is executed synchronously.  The output appears in the buffer\n"
		       "`*Shell Command Output*'.  If the output is short enough to display\n"
		       "in the echo area, it is shown there, but it is nonetheless available\n"
		       "in buffer `*Shell Command Output*' even though that buffer is not\n"
		       "automatically displayed.\n"
		       "\n"
		       "The optional second argument OUTPUT-BUFFER, if non-nil,\n"
		       "says to insert the output in the current buffer.\n"
		       "\n"
		       "In a remote buffer, the command runs with sh on its machine, in its\n"
		       "directory there. C-g and remote-timeout stop waiting for it, and try\n"
		       "to stop it there with SIGTERM; this needs ps there and a working\n"
		       "connection, and a command can ignore it or detach and go on running.\n"
		       "A command that does not finish inserts nothing.");

	lisp_func_new ("shell-command-on-region", F_shell_command_on_region, true,
		       "Execute string command in inferior shell with region as input.\n"
		       "Normally display output (if any) in temp buffer `*Shell Command Output*';\n"
		       "Prefix arg means replace the region with it.  Return the exit code of\n"
		       "command.\n"
		       "\n"
		       "If the command generates output, the output may be displayed\n"
		       "in the echo area or in a buffer.\n"
		       "If the output is short enough to display in the echo area, it is shown\n"
		       "there.  Otherwise it is displayed in the buffer `*Shell Command Output*'.\n"
		       "The output is available in that buffer in both cases.\n"
		       "\n"
		       "In a remote buffer, the command runs on its machine, as for\n"
		       "`shell-command'; if it does not finish, the region is kept.");
}
