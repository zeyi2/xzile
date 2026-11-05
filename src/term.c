/* Terminal handling using termios and ANSI escape sequences

   Copyright (c) 1997-2021 Free Software Foundation, Inc.
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
#include "screen.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/ioctl.h>

#define TERM_IN 0
#define TERM_OUT 1

/* Time to wait for the rest of an escape sequence, in ms. */
#define ESCAPE_DELAY 100

/*--------------------------------------------------------------------------
 * Raw input codes.
 *
 * Codes up to 0xff are bytes read from the terminal; decoded escape
 * sequences produce the codes below, possibly with modifier bits.
 *--------------------------------------------------------------------------*/

enum {
	CODE_UP = 0x10000,
	CODE_DOWN,
	CODE_LEFT,
	CODE_RIGHT,
	CODE_HOME,
	CODE_END,
	CODE_PPAGE,
	CODE_NPAGE,
	CODE_IC,
	CODE_DC,
	CODE_F1,
	CODE_F2,
	CODE_F3,
	CODE_F4,
	CODE_F5,
	CODE_F6,
	CODE_F7,
	CODE_F8,
	CODE_F9,
	CODE_F10,
	CODE_F11,
	CODE_F12,
	CODE_PASTE,		/* Start of a bracketed paste. */
	CODE_IGNORE,		/* An unrecognised escape sequence. */
	CODE_RESIZE,		/* The terminal was resized. */
	CODE_MOUSE,		/* Mouse keystrokes from KBD_MOUSE_1 on. */
};

#define CODE_MOD_CTRL 0x100000u
#define CODE_MOD_META 0x200000u
#define CODE_MODS (CODE_MOD_CTRL | CODE_MOD_META)

#define CODE_NONE UINT_MAX

static const char BRACKETED_PASTE_ENABLE[] = "\033[?2004h";
static const char BRACKETED_PASTE_DISABLE[] = "\033[?2004l";
static const char BRACKETED_PASTE_END[] = "\033[201~";
/* Button and drag reports, in the SGR encoding. Most terminals still
   select text with Shift (Option in some macOS terminals) held. */
static const char MOUSE_ENABLE[] = "\033[?1002h\033[?1006h";
static const char MOUSE_DISABLE[] = "\033[?1002l\033[?1006l";


/*--------------------------------------------------------------------------
 * Screen state.
 *--------------------------------------------------------------------------*/


static bool initialized = false;
static bool tty_in = false;
static struct termios orig_termios;
static Cell *phys = NULL;	/* What we believe is displayed. */
static bool phys_valid = false;
static TerminalCapabilities capabilities;
static bool truecolor = false;

static int signal_pipe[2] = { -1, -1 };
static volatile sig_atomic_t winch_pending = 0;
static volatile sig_atomic_t cont_pending = 0;
static volatile sig_atomic_t chld_pending = 0;
static bool input_eof = false;

bool
term_input_eof (void)
{
	return input_eof;
}

/* Pushed-back key codes (a stack: the last element is read first). */
static unsigned *key_buf = NULL;
static size_t key_buf_n = 0, key_buf_cap = 0;

/* Bytes read from the terminal but not yet consumed. */
static unsigned char inq[64];
static size_t inq_n = 0;

static char *pending_paste_text = NULL;

static bool have_mouse = false;
static size_t mouse_y, mouse_x;
static int mouse_button;	/* The last button pressed, 0-2. */
static bool mouse_to_shell;	/* It was pressed in a program reading the mouse. */


/*--------------------------------------------------------------------------
 * Low-level output.
 *--------------------------------------------------------------------------*/

static void
write_all_fd (int fd, const char *s, size_t len)
{
	while (len > 0) {
		ssize_t n = write (fd, s, len);
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return;
		}
		s += n;
		len -= (size_t) n;
	}
}

static void
out_str (const char *s)
{
	write_all_fd (TERM_OUT, s, strlen (s));
}

/* Send bytes to the terminal outside the screen model. */
void
term_write_raw (const char *s, size_t len)
{
	write_all_fd (TERM_OUT, s, len);
}

static void
enter_screen_mode (void)
{
	/* Alternate screen, application cursor keys, bracketed paste. */
	out_str ("\033[?1049h\033[?1h\033=");
	out_str (BRACKETED_PASTE_ENABLE);
	out_str (MOUSE_ENABLE);
}

static void
leave_screen_mode (void)
{
	out_str ("\033[0m");
	out_str (BRACKETED_PASTE_DISABLE);
	out_str (MOUSE_DISABLE);
	out_str ("\033[?1l\033>\033[?25h\033[?1049l");
}

static void
set_raw_mode (void)
{
	if (!tty_in)
		return;
	struct termios raw = orig_termios;
	raw.c_iflag &= ~(tcflag_t) (BRKINT | ICRNL | INLCR | IGNCR | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(tcflag_t) OPOST;
	raw.c_cflag |= CS8;
	raw.c_lflag &= ~(tcflag_t) (ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr (TERM_IN, TCSAFLUSH, &raw);
}

static void
restore_mode (void)
{
	if (tty_in)
		tcsetattr (TERM_IN, TCSAFLUSH, &orig_termios);
}


/*--------------------------------------------------------------------------
 * Signals.
 *--------------------------------------------------------------------------*/

static void
wake_input (void)
{
	if (signal_pipe[1] >= 0) {
		int saved = errno;
		char c = 0;
		if (write (signal_pipe[1], &c, 1) < 0) { /* Ignore error. */ }
		errno = saved;
	}
}

static void
winch_handler (int signo)
{
	(void) signo;
	winch_pending = 1;
	wake_input ();
}

static void
chld_handler (int signo)
{
	(void) signo;
	chld_pending = 1;
	wake_input ();
}

static void tstp_handler (int signo);

static void
install_tstp_handler (void)
{
	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = tstp_handler;
	sigemptyset (&sa.sa_mask);
	sigaction (SIGTSTP, &sa, NULL);
}

static void
tstp_handler (int signo)
{
	(void) signo;
	int saved = errno;

	leave_screen_mode ();
	restore_mode ();

	/* Stop for real, then carry on from here when continued. */
	signal (SIGTSTP, SIG_DFL);
	sigset_t mask;
	sigemptyset (&mask);
	sigaddset (&mask, SIGTSTP);
	sigprocmask (SIG_UNBLOCK, &mask, NULL);
	kill (getpid (), SIGTSTP);

	install_tstp_handler ();
	set_raw_mode ();
	enter_screen_mode ();
	cont_pending = 1;
	wake_input ();
	errno = saved;
}


/*--------------------------------------------------------------------------
 * Screen buffers.
 *--------------------------------------------------------------------------*/

static void
query_size (size_t *out_rows, size_t *out_cols)
{
	size_t r = 0, c = 0;
	long n;
	const char *s;

	if ((s = getenv ("LINES")) != NULL && parse_long (s, &n) && n > 0)
		r = (size_t) n;
	if ((s = getenv ("COLUMNS")) != NULL && parse_long (s, &n) && n > 0)
		c = (size_t) n;

	struct winsize ws;
	if ((r == 0 || c == 0) &&
	    (ioctl (TERM_OUT, TIOCGWINSZ, &ws) == 0 || ioctl (TERM_IN, TIOCGWINSZ, &ws) == 0)) {
		if (r == 0)
			r = ws.ws_row;
		if (c == 0)
			c = ws.ws_col;
	}

	*out_rows = MAX (r > 0 ? r : 24, 3);
	*out_cols = MAX (c > 0 ? c : 80, 10);
}

static void
alloc_screens (void)
{
	size_t rows, cols;
	query_size (&rows, &cols);
	screen_resize (rows, cols);
	free (phys);
	phys = xmalloc (rows * cols * sizeof (Cell));
	phys_valid = false;
}

static void
detect_capabilities (void)
{
	const char *term = getenv ("TERM");
	const char *colorterm = getenv ("COLORTERM");

	capabilities.supports_reverse = true;
	capabilities.supports_underline = true;
	capabilities.has_colors = false;
	capabilities.supports_default_colors = false;
	capabilities.color_count = 0;

	if (term == NULL || *term == '\0' || strcmp (term, "dumb") == 0 ||
	    str_has_prefix (term, "vt") || strstr (term, "-mono") != NULL ||
	    str_has_suffix (term, "-m"))
		return;

	capabilities.has_colors = true;
	capabilities.supports_default_colors = true;
	truecolor = colorterm != NULL && (strcmp (colorterm, "truecolor") == 0 || strcmp (colorterm, "24bit") == 0);
	if (strstr (term, "256color") != NULL || strstr (term, "truecolor") != NULL ||
	    strstr (term, "direct") != NULL || (colorterm != NULL && *colorterm != '\0'))
		capabilities.color_count = 256;
	else if (strstr (term, "16color") != NULL)
		capabilities.color_count = 16;
	else
		capabilities.color_count = 8;
}

void
term_init (void)
{
	tty_in = isatty (TERM_IN) && tcgetattr (TERM_IN, &orig_termios) == 0;

	if (pipe (signal_pipe) == 0) {
		for (int i = 0; i < 2; i++) {
			fcntl (signal_pipe[i], F_SETFL, fcntl (signal_pipe[i], F_GETFL) | O_NONBLOCK);
			fcntl (signal_pipe[i], F_SETFD, FD_CLOEXEC);
		}
	} else
		signal_pipe[0] = signal_pipe[1] = -1;

	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = winch_handler;
	sigemptyset (&sa.sa_mask);
	sigaction (SIGWINCH, &sa, NULL);
	sa.sa_handler = chld_handler;
	sa.sa_flags = SA_NOCLDSTOP;
	sigaction (SIGCHLD, &sa, NULL);
	install_tstp_handler ();

	alloc_screens ();
	detect_capabilities ();

	set_raw_mode ();
	enter_screen_mode ();
	initialized = true;
}

void
term_close (void)
{
	if (!initialized)
		return;
	initialized = false;
	signal (SIGTSTP, SIG_DFL);
	signal (SIGWINCH, SIG_DFL);
	leave_screen_mode ();
	restore_mode ();
}

bool
term_initialized (void)
{
	return initialized;
}

const TerminalCapabilities *
term_get_capabilities (void)
{
	return &capabilities;
}


void
term_clear (void)
{
	for (size_t i = 0; i < scr_rows * scr_cols; i++)
		screen_blank_cell (&scr_cells[i]);
	phys_valid = false;
}

void
term_beep (void)
{
	if (initialized)
		out_str ("\a");
}


/*--------------------------------------------------------------------------
 * Refresh.
 *--------------------------------------------------------------------------*/

static bool
attr_eq (Attr a, Attr b)
{
	return a.fg == b.fg && a.bg == b.bg && a.flags == b.flags;
}

static bool
cell_eq (const Cell *a, const Cell *b)
{
	return attr_eq (a->attr, b->attr) && strcmp (a->ch, b->ch) == 0;
}

/* The RGB value of a colour in the xterm 256-colour palette. */
static void
palette_rgb (int c, int *r, int *g, int *b)
{
	static const int base[16][3] = {
		{ 0, 0, 0 }, { 205, 0, 0 }, { 0, 205, 0 }, { 205, 205, 0 },
		{ 0, 0, 238 }, { 205, 0, 205 }, { 0, 205, 205 }, { 229, 229, 229 },
		{ 127, 127, 127 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 },
		{ 92, 92, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 },
	};
	static const int levels[] = { 0, 95, 135, 175, 215, 255 };
	if (c < 16) {
		*r = base[c][0];
		*g = base[c][1];
		*b = base[c][2];
	} else if (c < 232) {
		c -= 16;
		*r = levels[c / 36];
		*g = levels[(c / 6) % 6];
		*b = levels[c % 6];
	} else
		*r = *g = *b = 8 + 10 * (c - 232);
}

static void
add_color (StrBuf *sb, int color, int base, int bright_base, int ext)
{
	if (color < 0 || !capabilities.has_colors)
		return;

	int r, g, b;
	bool rgb = (color & 0x1000000) != 0;
	if (rgb) {
		r = (color >> 16) & 0xff;
		g = (color >> 8) & 0xff;
		b = color & 0xff;
		if (truecolor) {
			sb_addf (sb, ";%d;2;%d;%d;%d", ext, r, g, b);
			return;
		}
		color = capabilities.color_count >= 256 ? color_rgb_to_256 (r, g, b) : color_rgb_to_16 (r, g, b);
	} else if (color >= 16 && capabilities.color_count < 256) {
		palette_rgb (color, &r, &g, &b);
		color = color_rgb_to_16 (r, g, b);
	}

	if (color >= 8 && capabilities.color_count < 16)
		color -= 8;
	if (color < 8)
		sb_addf (sb, ";%d", base + color);
	else if (color < 16)
		sb_addf (sb, ";%d", bright_base + color - 8);
	else
		sb_addf (sb, ";%d;5;%d", ext, color);
}

static void
add_sgr (StrBuf *sb, Attr a)
{
	sb_add (sb, "\033[0");
	if (a.flags & ATTR_BOLD)
		sb_add (sb, ";1");
	if (a.flags & ATTR_DIM)
		sb_add (sb, ";2");
	if (a.flags & ATTR_ITALIC)
		sb_add (sb, ";3");
	if (a.flags & ATTR_STRIKE)
		sb_add (sb, ";9");
	if (a.flags & ATTR_UNDERLINE)
		sb_add (sb, ";4");
	if (a.flags & ATTR_REVERSE)
		sb_add (sb, ";7");
	add_color (sb, a.fg, 30, 90, 38);
	add_color (sb, a.bg, 40, 100, 48);
	sb_addc (sb, 'm');
}

void
term_refresh (void)
{
	if (!initialized)
		return;

	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, "\033[?25l");

	if (!phys_valid) {
		sb_add (&sb, "\033[0m\033[H\033[2J");
		for (size_t i = 0; i < scr_rows * scr_cols; i++)
			screen_blank_cell (&phys[i]);
	}

	Attr attr = { TERM_COLOR_DEFAULT, TERM_COLOR_DEFAULT, 0 };
	bool attr_known = false;
	size_t py = NOPOS, px = NOPOS;	/* Physical cursor position, if known. */

	for (size_t y = 0; y < scr_rows; y++)
		for (size_t x = 0; x < scr_cols; x++) {
			Cell *want = &scr_cells[y * scr_cols + x];
			Cell *have = &phys[y * scr_cols + x];
			if (cell_eq (want, have))
				continue;
			if (want->ch[0] == '\0') {
				/* Right half of a wide character drawn by its left half. */
				*have = *want;
				continue;
			}
			if (py != y || px != x)
				sb_addf (&sb, "\033[%zu;%zuH", y + 1, x + 1);
			if (!attr_known || !attr_eq (attr, want->attr)) {
				add_sgr (&sb, want->attr);
				attr = want->attr;
				attr_known = true;
			}
			sb_add (&sb, want->ch);
			*have = *want;
			bool wide = x + 1 < scr_cols && scr_cells[y * scr_cols + x + 1].ch[0] == '\0';
			py = y;
			px = x + (wide ? 2 : 1);
			if (px >= scr_cols)
				px = NOPOS;
		}
	phys_valid = true;

	sb_add (&sb, "\033[0m");
	sb_addf (&sb, "\033[%zu;%zuH%s", scr_y + 1, scr_x + 1, scr_cursor_visible ? "\033[?25h" : "");
	write_all_fd (TERM_OUT, sb.s, sb.len);
	sb_free (&sb);
}


/*--------------------------------------------------------------------------
 * Input.
 *--------------------------------------------------------------------------*/

size_t
term_buf_len (void)
{
	return key_buf_n;
}

static void
key_buf_push (unsigned c)
{
	if (key_buf_n == key_buf_cap) {
		key_buf_cap = key_buf_cap ? key_buf_cap * 2 : 32;
		key_buf = xrealloc (key_buf, key_buf_cap * sizeof (unsigned));
	}
	key_buf[key_buf_n++] = c;
}

static void
inq_unread (const unsigned char *bytes, size_t n)
{
	if (inq_n + n > sizeof inq)
		n = sizeof inq - inq_n;
	memmove (inq + n, inq, inq_n);
	memcpy (inq, bytes, n);
	inq_n += n;
}

/* Returns true if a subprocess changed state. */
static bool
handle_signal_wakeup (void)
{
	char buf[64];
	while (signal_pipe[0] >= 0 && read (signal_pipe[0], buf, sizeof buf) > 0)
		;
	if (cont_pending) {
		cont_pending = 0;
		phys_valid = false;
		term_refresh ();
	}
	if (chld_pending) {
		chld_pending = 0;
		return process_reap ();
	}
	return false;
}

/* Read one byte from the terminal, waiting up to `delay' ms (forever if
   negative). Returns CODE_NONE on timeout and CODE_RESIZE on resize. */
static unsigned
read_byte (int delay)
{
	if (inq_n > 0) {
		unsigned c = inq[0];
		memmove (inq, inq + 1, --inq_n);
		return c;
	}

	int64_t deadline = delay >= 0 ? monotonic_ms () + delay : 0;
	for (;;) {
		if (winch_pending) {
			winch_pending = 0;
			return CODE_RESIZE;
		}

		int timeout = -1;
		if (delay >= 0) {
			int64_t left = deadline - monotonic_ms ();
			timeout = left > 0 ? (int) left : 0;
		} else if (input_eof) {
			fprintf (stderr, "%s: end of input\n", program_name);
			zile_exit (EXIT_SIGNAL);
		}

		struct pollfd fds[2 + MAX_PROCESS_FDS] = {
			{ TERM_IN, POLLIN, 0 },
			{ signal_pipe[0], POLLIN, 0 },
		};
		size_t nproc = process_poll_fds (fds + 2, MAX_PROCESS_FDS);
		if (signal_pipe[0] < 0)
			fds[1].fd = -1;
		if (input_eof)
			fds[0].fd = -1;
		int r = poll (fds, (nfds_t) (2 + nproc), timeout);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return CODE_NONE;
		}
		if (r == 0)
			return CODE_NONE;

		bool changed = false;
		if (signal_pipe[0] >= 0 && (fds[1].revents & POLLIN))
			changed = handle_signal_wakeup ();
		if (nproc > 0 && process_handle_events (fds + 2, nproc))
			changed = true;
		if (changed)
			redisplay_after_output ();
		/* macOS cannot poll some devices, such as /dev/null. */
		if (fds[0].revents & POLLNVAL)
			input_eof = true;
		if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			if (delay >= 0 && monotonic_ms () >= deadline)
				return CODE_NONE;
			continue;
		}

		if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			unsigned char c;
			ssize_t n = read (TERM_IN, &c, 1);
			if (n == 1)
				return c;
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				continue;
			/* End of input: give up if we would wait forever. */
			if (delay < 0) {
				fprintf (stderr, "%s: end of input\n", program_name);
				zile_exit (EXIT_SIGNAL);
			}
			/* Keep serving subprocesses until the deadline. */
			input_eof = true;
			continue;
			return CODE_NONE;
		}
	}
}

static unsigned
csi_modifiers (long m)
{
	unsigned mods = 0;
	if (m > 1) {
		m--;
		if (m & 2)
			mods |= CODE_MOD_META;
		if (m & 4)
			mods |= CODE_MOD_CTRL;
		if (m & 8)
			mods |= CODE_MOD_META;
	}
	return mods;
}

static unsigned
final_to_code (char final)
{
	switch (final) {
	case 'A': return CODE_UP;
	case 'B': return CODE_DOWN;
	case 'C': return CODE_RIGHT;
	case 'D': return CODE_LEFT;
	case 'H': return CODE_HOME;
	case 'F': return CODE_END;
	case 'P': return CODE_F1;
	case 'Q': return CODE_F2;
	case 'R': return CODE_F3;
	case 'S': return CODE_F4;
	default: return CODE_NONE;
	}
}

static unsigned
tilde_to_code (long n)
{
	switch (n) {
	case 1: case 7: return CODE_HOME;
	case 2: return CODE_IC;
	case 3: return CODE_DC;
	case 4: case 8: return CODE_END;
	case 5: return CODE_PPAGE;
	case 6: return CODE_NPAGE;
	case 11: return CODE_F1;
	case 12: return CODE_F2;
	case 13: return CODE_F3;
	case 14: return CODE_F4;
	case 15: return CODE_F5;
	case 17: return CODE_F6;
	case 18: return CODE_F7;
	case 19: return CODE_F8;
	case 20: return CODE_F9;
	case 21: return CODE_F10;
	case 23: return CODE_F11;
	case 24: return CODE_F12;
	case 200: return CODE_PASTE;
	default: return CODE_IGNORE;
	}
}

/* The code for a mouse report of button code `cb' at 1-based column
   `x', row `y'. Releases, drags and presses of buttons 2 and 3 are
   only for programs reading the mouse; elsewhere they are ignored. */
static unsigned
mouse_code (unsigned cb, unsigned long x, unsigned long y, bool release)
{
	if (x == 0 || y == 0)
		return CODE_IGNORE;
	mouse_y = y - 1;
	mouse_x = x - 1;
	have_mouse = true;

	cb &= ~(4u | 8u | 16u);	/* Modifiers. */
	if (cb & 64)
		return cb == 64 ? CODE_MOUSE + (KBD_WHEEL_UP - KBD_MOUSE_1)
			: cb == 65 ? CODE_MOUSE + (KBD_WHEEL_DOWN - KBD_MOUSE_1) : CODE_IGNORE;
	static const Keystroke press[] = { KBD_MOUSE_1, KBD_MOUSE_2, KBD_MOUSE_3 };
	static const Keystroke drag[] = { KBD_DRAG_MOUSE_1, KBD_DRAG_MOUSE_2, KBD_DRAG_MOUSE_3 };
	static const Keystroke up[] = { KBD_UP_MOUSE_1, KBD_UP_MOUSE_2, KBD_UP_MOUSE_3 };
	unsigned b = cb & 3;
	Keystroke key;
	if (release || b == 3) {	/* The X10 encoding releases button 3. */
		if (!release && (cb & 32))
			return CODE_IGNORE;	/* Motion with no button. */
		key = up[release && b < 3 ? b : (unsigned) mouse_button];
	} else if (cb & 32)
		key = drag[b];
	else {
		mouse_button = (int) b;
		mouse_to_shell = shell_mouse_at (mouse_y, mouse_x);
		key = press[b];
	}
	if (key != KBD_MOUSE_1 && key != KBD_DRAG_MOUSE_1 && !mouse_to_shell)
		return CODE_IGNORE;
	return CODE_MOUSE + (key - KBD_MOUSE_1);
}

/* Having read ESC from the terminal, try to decode an escape sequence.
   If the following bytes do not form one, they are pushed back and ESC
   is returned. */
static unsigned
decode_escape (void)
{
	unsigned char seq[32];
	size_t n = 0;
	unsigned c = read_byte (ESCAPE_DELAY);
	if (c == CODE_NONE || c == CODE_RESIZE) {
		if (c == CODE_RESIZE)
			winch_pending = 1;
		return 033;
	}
	seq[n++] = (unsigned char) c;

	if (c == 'O') {
		unsigned f = read_byte (ESCAPE_DELAY);
		if (f <= 0xff) {
			if (f == 'M')
				return 015;
			unsigned code = final_to_code ((char) f);
			if (code != CODE_NONE)
				return code;
			seq[n++] = (unsigned char) f;
		} else if (f == CODE_RESIZE)
			winch_pending = 1;
		inq_unread (seq, n);
		return 033;
	}

	if (c != '[') {
		inq_unread (seq, n);
		return 033;
	}

	/* CSI: parameters, intermediates, then a final byte. */
	long params[4] = { 0, 0, 0, 0 };
	size_t nparams = 0;
	bool have_digit = false;
	for (;;) {
		unsigned b = read_byte (ESCAPE_DELAY);
		if (b > 0xff || n >= sizeof seq) {
			if (b == CODE_RESIZE)
				winch_pending = 1;
			inq_unread (seq, n);
			return 033;
		}
		seq[n++] = (unsigned char) b;
		if (b >= '0' && b <= '9') {
			if (nparams < 4)
				params[nparams] = params[nparams] * 10 + (long) (b - '0');
			have_digit = true;
		} else if (b == ';') {
			if (nparams < 4)
				nparams++;
			have_digit = false;
		} else if (b >= 0x20 && b <= 0x3f) {
			/* Other parameter or intermediate bytes. */
		} else if (b >= 0x40 && b <= 0x7e) {
			if (have_digit && nparams < 4)
				nparams++;
			if (b == '[' && n == 2) {
				/* Linux console function keys: ESC [ [ A-E. */
				unsigned f = read_byte (ESCAPE_DELAY);
				if (f >= 'A' && f <= 'E')
					return CODE_F1 + (f - 'A');
				return CODE_IGNORE;
			}
			if ((b == 'M' || b == 'm') && seq[1] == '<' && nparams == 3)
				return mouse_code ((unsigned) params[0], (unsigned long) params[1],
						   (unsigned long) params[2], b == 'm');
			if (b == 'M' && n == 2) {
				/* X10 encoding: three bytes, each offset by 32. */
				unsigned v[3];
				for (int i = 0; i < 3; i++)
					if ((v[i] = read_byte (ESCAPE_DELAY)) < 32 || v[i] > 0xff)
						return CODE_IGNORE;
				return mouse_code (v[0] - 32, v[1] - 32, v[2] - 32, false);
			}
			unsigned mods = nparams >= 2 ? csi_modifiers (params[1]) : 0;
			unsigned code;
			if (b == '~')
				code = tilde_to_code (params[0]);
			else if (b == 'Z')
				return 011;
			else
				code = final_to_code ((char) b);
			if (code == CODE_NONE || code == CODE_IGNORE)
				return CODE_IGNORE;
			return code == CODE_PASTE ? code : code | mods;
		} else {
			inq_unread (seq, n);
			return 033;
		}
	}
}

static unsigned
read_code (int delay)
{
	unsigned c = read_byte (delay);
	if (c == 033)
		return decode_escape ();
	return c;
}

static unsigned
get_char (int delay)
{
	if (key_buf_n > 0)
		return key_buf[--key_buf_n];

	for (;;) {
		unsigned c = read_code (delay);
		if (c == CODE_IGNORE)
			continue;
		if (c == CODE_RESIZE) {
			alloc_screens ();
			resize_windows ();
			continue;
		}
		return c;
	}
}

static Keystroke
codetokey (unsigned c)
{
	if (c == CODE_NONE)
		return KBD_NOKEY;

	if (c >= CODE_MOUSE && c <= CODE_MOUSE + (KBD_DRAG_MOUSE_3 - KBD_MOUSE_1))
		return KBD_MOUSE_1 + (c - CODE_MOUSE);
	if (c >= CODE_UP) {
		Keystroke mods = 0;
		if (c & CODE_MOD_CTRL)
			mods |= KBD_CTRL;
		if (c & CODE_MOD_META)
			mods |= KBD_META;
		switch (c & ~CODE_MODS) {
		case CODE_UP: return KBD_UP | mods;
		case CODE_DOWN: return KBD_DOWN | mods;
		case CODE_LEFT: return KBD_LEFT | mods;
		case CODE_RIGHT: return KBD_RIGHT | mods;
		case CODE_HOME: return KBD_HOME | mods;
		case CODE_END: return KBD_END | mods;
		case CODE_PPAGE: return KBD_PGUP | mods;
		case CODE_NPAGE: return KBD_PGDN | mods;
		case CODE_IC: return KBD_INS | mods;
		case CODE_DC: return KBD_DEL | mods;
		case CODE_F1: return KBD_F1 | mods;
		case CODE_F2: return KBD_F2 | mods;
		case CODE_F3: return KBD_F3 | mods;
		case CODE_F4: return KBD_F4 | mods;
		case CODE_F5: return KBD_F5 | mods;
		case CODE_F6: return KBD_F6 | mods;
		case CODE_F7: return KBD_F7 | mods;
		case CODE_F8: return KBD_F8 | mods;
		case CODE_F9: return KBD_F9 | mods;
		case CODE_F10: return KBD_F10 | mods;
		case CODE_F11: return KBD_F11 | mods;
		case CODE_F12: return KBD_F12 | mods;
		default: return KBD_NOKEY;
		}
	}

	switch (c) {
	case '\0':		/* C-@ */
		return KBD_CTRL | '@';
	case 011:
		return KBD_TAB;
	case 015:
		return KBD_RET;
	case 033:		/* META */
		return KBD_META;
	case 034:
		return KBD_CTRL | '\\';
	case 035:
		return KBD_CTRL | ']';
	case 036:
		return KBD_CTRL | '^';
	case 037:
		return KBD_CTRL | '_';
	case 0177:		/* BS */
		return KBD_BS;
	default:
		if (c >= 01 && c <= 032)	/* C-a ... C-z */
			return KBD_CTRL | ('a' + c - 1);
		return c;
	}
}

static unsigned
special_code (Keystroke key)
{
	switch (key) {
	case KBD_PGUP: return CODE_PPAGE;
	case KBD_PGDN: return CODE_NPAGE;
	case KBD_HOME: return CODE_HOME;
	case KBD_END: return CODE_END;
	case KBD_DEL: return CODE_DC;
	case KBD_INS: return CODE_IC;
	case KBD_LEFT: return CODE_LEFT;
	case KBD_RIGHT: return CODE_RIGHT;
	case KBD_UP: return CODE_UP;
	case KBD_DOWN: return CODE_DOWN;
	case KBD_F1: return CODE_F1;
	case KBD_F2: return CODE_F2;
	case KBD_F3: return CODE_F3;
	case KBD_F4: return CODE_F4;
	case KBD_F5: return CODE_F5;
	case KBD_F6: return CODE_F6;
	case KBD_F7: return CODE_F7;
	case KBD_F8: return CODE_F8;
	case KBD_F9: return CODE_F9;
	case KBD_F10: return CODE_F10;
	case KBD_F11: return CODE_F11;
	case KBD_F12: return CODE_F12;
	default: return CODE_NONE;
	}
}

/* Convert a keystroke into the codes that would produce it, in reading
   order. Returns the number of codes (at most 2). */
static size_t
keytocodes (Keystroke key, unsigned codes[2])
{
	size_t n = 0;

	if (key == KBD_NOKEY)
		return 0;
	if (KBD_IS_MOUSE (key)) {
		codes[n++] = CODE_MOUSE + (key - KBD_MOUSE_1);
		return n;
	}

	if (key & KBD_META)	/* META */
		codes[n++] = 033;
	key &= ~KBD_META;

	unsigned special = special_code (key & ~KBD_CTRL);
	if (special != CODE_NONE) {
		codes[n++] = special | ((key & KBD_CTRL) ? CODE_MOD_CTRL : 0);
		return n;
	}

	switch (key) {
	case KBD_CTRL | '@':
		codes[n++] = '\0';
		break;
	case KBD_TAB:
		codes[n++] = 011;
		break;
	case KBD_RET:
		codes[n++] = 015;
		break;
	case KBD_CTRL | '\\':
		codes[n++] = 034;
		break;
	case KBD_CTRL | ']':
		codes[n++] = 035;
		break;
	case KBD_CTRL | '^':
		codes[n++] = 036;
		break;
	case KBD_CTRL | '_':
		codes[n++] = 037;
		break;
	case KBD_BS:
		codes[n++] = 0177;
		break;
	default:
		if ((key & KBD_CTRL) && (key & 0xff) >= 'a' && (key & 0xff) <= 'z' &&
		    (key & ~KBD_CTRL) <= 0xff)
			codes[n++] = (key & 0xff) + 1 - 'a';
		else if ((key & 0xff) == key)
			codes[n++] = key;
		break;
	}

	return n;
}

static char *
read_bracketed_paste_payload (void)
{
	StrBuf text;
	sb_init (&text);
	size_t matched = 0, end_len = strlen (BRACKETED_PASTE_END);

	for (;;) {
		unsigned c = read_byte (GETKEY_DEFAULT);
		if (c == CODE_RESIZE) {
			winch_pending = 1;
			continue;
		}
		if (c > 0xff)
			break;
		if (c == (unsigned char) BRACKETED_PASTE_END[matched]) {
			if (++matched == end_len)
				break;
			continue;
		}

		sb_addn (&text, BRACKETED_PASTE_END, matched);
		matched = 0;

		if (c == (unsigned char) BRACKETED_PASTE_END[0])
			matched = 1;
		else
			sb_addc (&text, (char) c);
	}

	return sb_take (&text);
}

Keystroke
term_getkey (int delay)
{
	bool from_tty = key_buf_n == 0;
	unsigned c = get_char (delay);

	if (c == CODE_PASTE) {
		free (pending_paste_text);
		pending_paste_text = read_bracketed_paste_payload ();
		return KBD_BRACKETED_PASTE;
	}

	if (c == 033) {
		/* In a terminal, ESC typed on its own (nothing followed it
		   within the escape delay) is itself, not a Meta prefix. */
		if (from_tty && inq_n == 0 && cur_bp != NULL && shell_char_mode (cur_bp) &&
		    !term_minibuf_active ())
			return KBD_ESCAPE;
		Keystroke key = codetokey (get_char (GETKEY_DEFAULT));
		while (key == KBD_META)
			key = codetokey (get_char (GETKEY_DEFAULT)) | KBD_META;
		return key | KBD_META;
	}

	return codetokey (c);
}

unsigned
term_getkey_unfiltered (int delay)
{
	if (key_buf_n > 0)
		return key_buf[--key_buf_n];

	for (;;) {
		unsigned c = read_byte (delay);
		if (c == CODE_RESIZE) {
			alloc_screens ();
			resize_windows ();
			continue;
		}
		return c;
	}
}

void
term_ungetkey (Keystroke key)
{
	unsigned codes[2];
	size_t n = keytocodes (key, codes);
	for (size_t i = n; i > 0; i--)
		key_buf_push (codes[i - 1]);
}

char *
term_take_pending_paste (void)
{
	char *text = pending_paste_text;
	pending_paste_text = NULL;
	return text;
}

void
term_suspend (void)
{
	raise (SIGTSTP);
}

/* A terminal reaches the clipboard through clipboard programs or OSC 52
   (see clipboard.c). */
bool
term_clipboard_set (const char *text, size_t len)
{
	(void) text;
	(void) len;
	return false;
}

char *
term_clipboard_get (size_t *len)
{
	(void) len;
	return NULL;
}

bool
term_mouse_position (size_t *y, size_t *x)
{
	*y = mouse_y;
	*x = mouse_x;
	return have_mouse;
}
