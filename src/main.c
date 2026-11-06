/* Program invocation, startup and shutdown

   Copyright (c) 1997-2020 Free Software Foundation, Inc.
   Copyright (c) 2025-2026 Zeyi2 <zeyi2@nekoarch.cc>

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

#include "xzile.h"

#include <ctype.h>
#include <locale.h>
#include <signal.h>
#include <sys/stat.h>

#define ZILE_VERSION_STRING PACKAGE_NAME " " VERSION
#define ZILE_COPYRIGHT_STRING \
	"Copyright (C) 2020 Free Software Foundation, Inc.\n" \
	"Copyright (C) 2026 Zeyi2 <zeyi2@nekoarch.cc>"

char *program_name = PACKAGE;

/* The global editor flags. */
int thisflag = 0;
int lastflag = 0;

/* The universal argument repeat count. */
long last_uniarg = 1;

/*
 * Exit Zile.
 * If `reason' is `EXIT_SUCCESS', close the terminal; otherwise, attempt to
 * save modified buffers.
 * If `reason' is `EXIT_COREDUMP`, abort to allow core dump generation;
 * otherwise, exit with given `reason' code.
 */
void
zile_exit (int reason)
{
	if (reason == EXIT_SUCCESS) {
		recentf_save ();
		term_finish ();
		remote_shutdown ();
	}
	else {
		term_close ();
		fprintf (stderr, "Trying to save modified buffers (if any)...\n");
		char package_upper[sizeof PACKAGE];
		for (size_t i = 0; i < sizeof PACKAGE; i++)
			package_upper[i] = (char) toupper ((unsigned char) PACKAGE[i]);
		for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
			if (bp->modified && !bp->nosave) {
				char *name = xasprintf ("%s.%sSAVE", buffer_get_filename_or_name (bp), package_upper);
				fprintf (stderr, "Saving %s...\n", name);
				write_to_disk (bp, name, S_IRUSR | S_IWUSR);
				free (name);
			}
		if (reason == EXIT_COREDUMP)
			abort ();
		remote_shutdown ();
	}

	process_kill_all ();
	debug_shutdown ();
	exit (reason);
}

static void
segv_sig_handler (int signo)
{
	(void) signo;
	signal (SIGSEGV, SIG_DFL);
	signal (SIGBUS, SIG_DFL);
	fprintf (stderr, "%s: " PACKAGE_NAME " crashed.  Please send a bug report to <"
		 PACKAGE_BUGREPORT ">.\n", program_name);
	zile_exit (EXIT_COREDUMP);
}

static void
other_sig_handler (int signo)
{
	fprintf (stderr, "%s: terminated with signal %d.\n", program_name, signo);
	zile_exit (EXIT_SIGNAL);
}

static void
signal_init (void)
{
	signal (SIGSEGV, segv_sig_handler);
	signal (SIGBUS, segv_sig_handler);
	signal (SIGHUP, other_sig_handler);
	signal (SIGINT, other_sig_handler);
	signal (SIGTERM, other_sig_handler);
	/* Writes to closed pipes fail with EPIPE instead. */
	signal (SIGPIPE, SIG_IGN);
}


/* Command-line options. */

enum OptionId {
	OPT_NO_INIT_FILE,
	OPT_FUNCALL,
	OPT_LOAD,
	OPT_DEBUG_LOG,
	OPT_DEBUG_CATEGORIES,
	OPT_HELP,
	OPT_VERSION,
};

typedef struct {
	enum OptionId id;
	const char *longname;
	char shortname;
	bool has_arg;
	const char *argstring;
	const char *docstring;
} Option;

static const Option options[] = {
	{ OPT_NO_INIT_FILE, "no-init-file", 'q', false, "", "do not load ~/." PACKAGE },
	{ OPT_FUNCALL, "funcall", 'f', true, "FUNC", "call " PACKAGE_NAME " Lisp function FUNC with no arguments" },
	{ OPT_LOAD, "load", 'l', true, "FILE", "load " PACKAGE_NAME " Lisp FILE using the load function" },
	{ OPT_DEBUG_LOG, "debug-log", '\0', true, "FILE", "append debug logs to FILE" },
	{ OPT_DEBUG_CATEGORIES, "debug-categories", '\0', true, "CATS", "comma-separated debug categories, e.g. syntax,filetype" },
	{ OPT_HELP, "help", '\0', false, "", "display this help message and exit" },
	{ OPT_VERSION, "version", '\0', false, "", "display version information and exit" },
};

#define NOPTIONS (sizeof options / sizeof options[0])

enum ArgType { ARG_FUNCTION = 1, ARG_LOADFILE, ARG_FILE };

typedef struct {
	enum ArgType type;
	char *arg;
	long line;
} CmdArg;

static CmdArg *cmd_args;
static size_t ncmd_args;

static void
add_cmd_arg (enum ArgType type, char *arg, long line)
{
	cmd_args = xrealloc (cmd_args, (ncmd_args + 1) * sizeof (CmdArg));
	cmd_args[ncmd_args].type = type;
	cmd_args[ncmd_args].arg = arg;
	cmd_args[ncmd_args].line = line;
	ncmd_args++;
}

static NORETURN void
print_help (const char *argv0)
{
	printf ("Usage: %s [OPTION-OR-FILENAME]...\n"
		"\n"
		"Run " PACKAGE_NAME ", the lightweight Emacs clone.\n"
		"\n", argv0);
	printf ("Initialization options:\n\n");
	for (size_t i = 0; i < NOPTIONS; i++) {
		const Option *opt = &options[i];
		char *optstring;
		if (opt->shortname != '\0')
			optstring = xasprintf ("--%s, -%c %s", opt->longname, opt->shortname, opt->argstring);
		else
			optstring = xasprintf ("--%s %s", opt->longname, opt->argstring);
		printf ("%-24s%s\n", optstring, opt->docstring);
		free (optstring);
	}
	printf ("\nAction options:\n\n");
	printf ("%-24s%s\n", "FILE", "visit FILE using find-file");
	printf ("%-24s%s\n", "+LINE FILE", "visit FILE using find-file, then go to line LINE");
	printf ("\nReport bugs to " PACKAGE_BUGREPORT ".\n");
	exit (EXIT_SUCCESS);
}

static NORETURN void
print_version (void)
{
	printf (ZILE_VERSION_STRING "\n"
		ZILE_COPYRIGHT_STRING "\n"
		PACKAGE_NAME " comes with ABSOLUTELY NO WARRANTY.\n"
		"You may redistribute copies of " PACKAGE_NAME "\n"
		"under the terms of the GNU General Public License.\n"
		"For more information about these matters, see the file named COPYING.\n");
	exit (EXIT_SUCCESS);
}

/* Find a long option by name or unambiguous prefix. */
static const Option *
find_long_option (const char *name, size_t len)
{
	const Option *found = NULL;
	for (size_t i = 0; i < NOPTIONS; i++)
		if (strncmp (options[i].longname, name, len) == 0) {
			if (strlen (options[i].longname) == len)
				return &options[i];
			if (found != NULL)
				return NULL;
			found = &options[i];
		}
	return found;
}

static const Option *
find_short_option (char c)
{
	for (size_t i = 0; i < NOPTIONS; i++)
		if (options[i].shortname == c && c != '\0')
			return &options[i];
	return NULL;
}

static NORETURN void
missing_argument (const char *arg)
{
	fprintf (stderr, "%s: Option `%s' requires an argument\n", program_name, arg);
	exit (EXIT_FAILURE);
}

/* Act on an option; returns false to stop processing. */
static void
handle_option (const Option *opt, const char *optarg, const char *argv0, bool *qflag)
{
	switch (opt->id) {
	case OPT_NO_INIT_FILE:
		*qflag = true;
		break;
	case OPT_FUNCALL:
		add_cmd_arg (ARG_FUNCTION, xstrdup (optarg), 0);
		break;
	case OPT_LOAD: {
		char *a = expand_path (optarg);
		add_cmd_arg (ARG_LOADFILE, a != NULL ? a : xstrdup (optarg), 0);
		break;
	}
	case OPT_DEBUG_LOG:
		debug_set_log_path (optarg);
		break;
	case OPT_DEBUG_CATEGORIES:
		debug_set_categories (optarg);
		break;
	case OPT_HELP:
		print_help (argv0);
	case OPT_VERSION:
		print_version ();
	}
}

static void
parse_command_line (int argc, char **argv, bool *qflag)
{
	long line = 1;
	bool options_done = false;

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!options_done && strcmp (arg, "--") == 0) {
			options_done = true;
			continue;
		}

		if (!options_done && arg[0] == '-' && arg[1] == '-') {
			const char *name = arg + 2;
			const char *eq = strchr (name, '=');
			size_t len = eq != NULL ? (size_t) (eq - name) : strlen (name);
			const Option *opt = find_long_option (name, len);
			if (opt == NULL) {
				minibuf_error ("Unknown option `%s'", arg);
				continue;
			}
			const char *optarg = NULL;
			if (opt->has_arg) {
				if (eq != NULL)
					optarg = eq + 1;
				else if (i + 1 < argc)
					optarg = argv[++i];
				else
					missing_argument (arg);
			}
			handle_option (opt, optarg, argv[0], qflag);
		} else if (!options_done && arg[0] == '-' && arg[1] != '\0') {
			for (size_t k = 1; arg[k] != '\0'; k++) {
				const Option *opt = find_short_option (arg[k]);
				if (opt == NULL) {
					minibuf_error ("Unknown option `%s'", arg);
					break;
				}
				if (opt->has_arg) {
					const char *optarg;
					if (arg[k + 1] != '\0')
						optarg = arg + k + 1;
					else if (i + 1 < argc)
						optarg = argv[++i];
					else
						missing_argument (arg);
					handle_option (opt, optarg, argv[0], qflag);
					break;
				}
				handle_option (opt, NULL, argv[0], qflag);
			}
		} else if (arg[0] == '+' && !options_done) {
			long n;
			if (parse_long (arg + 1, &n))
				line = n;
		} else {
			char *a = expand_path (arg);
			add_cmd_arg (ARG_FILE, a != NULL ? a : xstrdup (arg), line);
			line = 1;
		}
	}
}

static const char splash_str[] =
	"\n"
	"Welcome to " PACKAGE_NAME ".\n"
	"\n"
	"Undo changes       C-x u        Exit " PACKAGE_NAME "         C-x C-c\n"
	"(`C-' means use the CTRL key.  `M-' means hold the Meta (or Alt) key.\n"
	"If you have no Meta key, you may type ESC followed by the character.)\n"
	"Combinations like `C-x u' mean first press `C-x', then `u'.\n"
	"\n"
	"Keys not working properly?  See file://" PATH_DOCDIR "/FAQ\n"
	"\n"
	ZILE_VERSION_STRING "\n"
	ZILE_COPYRIGHT_STRING "\n"
	"\n"
	PACKAGE_NAME " comes with ABSOLUTELY NO WARRANTY.\n"
	PACKAGE_NAME " is Free Software--Free as in Freedom--so you can redistribute copies\n"
	"of " PACKAGE_NAME " and modify it; see the file COPYING.  Otherwise, a copy can be\n"
	"downloaded from https://www.gnu.org/licenses/gpl.html.\n";

int
main (int argc, char **argv)
{
	program_name = path_basename (argv[0]);
	debug_init_from_env ();

	/* Set up Lisp environment now so it's available to files and
	   expressions specified on the command-line. */
	lisp_init ();
	init_variables ();
	basic_init ();
	bind_init ();
	bookmark_init ();
	dired_init ();
	terminal_init ();
	compile_init ();
	magit_init ();
	preview_init ();
	image_mode_init ();
	pdf_mode_init ();
	html_mode_init ();
	markdown_mode_init ();
	selectors_init ();
	eval_init ();
	file_init ();
	funcs_init ();
	help_init ();
	killring_init ();
	line_init ();
	macro_init ();
	redisplay_init ();
	registers_init ();
	search_init ();
	shell_init ();
	theme_init ();
	undo_init ();
	variables_init ();
	window_init ();
	winhist_init ();

	bool qflag = false;
	parse_command_line (argc, argv, &qflag);

	signal_init ();

	setlocale (LC_ALL, "");

	term_init ();

	/* Create the `*scratch*' buffer, so that initialisation commands
	   that act on a buffer have something to act on. */
	minibuf_init ();
	create_scratch_window ();
	Buffer *scratch_bp = cur_bp;
	bprintf ("%s",
		 ";; This buffer is for notes you don't want to save.\n"
		 ";; If you want to create a file, visit that file with C-x C-f,\n"
		 ";; then enter the text in that file's own buffer.\n"
		 "\n");
	cur_bp->modified = false;

	init_default_bindings ();

	if (!qflag) {
		bookmarks_load ();
		char *home = get_home_dir ();
		char *init_file = path_join (home, "." PACKAGE);
		lisp_loadfile (init_file);
		free (init_file);
		free (home);
		recentf_load ();
		locations_load ();
	}

	/* Create the splash buffer & message only if no files, function or
	   load file is specified on the command line, and there has been no
	   error. */
	if (ncmd_args == 0 && minibuf_no_error () && !get_variable_bool ("inhibit-splash-screen")) {
		Buffer *bp = create_auto_buffer ("*" PACKAGE_NAME "*");
		buffer_switch_to (bp, true);
		bprintf ("%s", splash_str);
		bp->readonly = true;
		funcall ("beginning-of-buffer");
	}

	/* Load files and load files and run functions given on the command
	   line. */
	bool ok = true;
	for (size_t i = 0; ok && i < ncmd_args; i++) {
		const char *arg = cmd_args[i].arg;

		switch (cmd_args[i].type) {
		case ARG_FUNCTION:
			ok = execute_function (arg, 1, false);
			if (!ok)
				minibuf_error ("Function `%s' not defined", arg);
			break;
		case ARG_LOADFILE:
			ok = lisp_loadfile (arg);
			if (!ok)
				minibuf_error ("Cannot open load file: %s\n", arg);
			break;
		case ARG_FILE:
			ok = find_file (arg);
			if (ok && cur_bp->dired == NULL)
				funcall_n ("goto-line", cmd_args[i].line);
			break;
		}
	}
	lastflag |= FLAG_NEED_RESYNC;

	/* Set up screen according to number of files loaded. */
	Buffer *last_bp = NULL;
	int files = 0;
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next) {
		/* Last buffer that isn't *scratch*. */
		if (bp->next != NULL && bp->next->next == NULL)
			last_bp = bp;
		files++;
	}
	if (files == 3) { /* *scratch* and two files. */
		funcall ("split-window");
		buffer_switch_to (last_bp, true);
		funcall ("other-window");
	} else if (files > 3) /* More than two files. */
		funcall ("list-buffers");

	/* Reinitialise the scratch buffer to catch settings */
	if (buffer_is_live (scratch_bp))
		buffer_init (scratch_bp);

	/* Refresh minibuffer in case there was an error that couldn't be
	   written during startup */
	minibuf_refresh ();

	/* Run the main loop. */
	for (;;) {
		if (lastflag & FLAG_NEED_RESYNC)
			window_resync (cur_wp);
		get_and_run_command ();
	}
}
