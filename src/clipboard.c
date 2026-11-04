/* System clipboard

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

#include "xzile.h"

#include <unistd.h>

/* Terminals limit OSC 52 payloads; skip copies too big to be useful. */
#define MAX_OSC52_BYTES (1024 * 1024)

/* The text last put on or taken from the clipboard, to tell whether
   another program has changed it since. */
static char *last_text;
static size_t last_len;
static bool pending;

static bool
enabled (void)
{
	return get_variable_bool ("select-enable-clipboard") && term_initialized () &&
		isatty (STDIN_FILENO) && isatty (STDOUT_FILENO);
}

static bool
remote (void)
{
	return getenv ("SSH_CONNECTION") != NULL || getenv ("SSH_TTY") != NULL;
}

#ifndef __APPLE__
static bool
in_path (const char *prog)
{
	const char *path = getenv ("PATH");
	if (path == NULL)
		return false;
	for (const char *p = path;;) {
		size_t n = strcspn (p, ":");
		char *file = xasprintf ("%.*s/%s", (int) n, n > 0 ? p : ".", prog);
		bool ok = access (file, X_OK) == 0;
		free (file);
		if (ok)
			return true;
		if (p[n] == '\0')
			return false;
		p += n + 1;
	}
}
#endif

/* The local clipboard program for copying (`paste' false) or pasting,
   or NULL to use the terminal. */
static const char *const *
local_command (bool paste)
{
	if (remote ())
		return NULL;
#ifdef __APPLE__
	static const char *const pbcopy[] = { "pbcopy", NULL }, *const pbpaste[] = { "pbpaste", NULL };
	return paste ? pbpaste : pbcopy;
#else
	static const char *const wl_copy[] = { "wl-copy", NULL };
	static const char *const wl_paste[] = { "wl-paste", "--no-newline", NULL };
	static const char *const xclip_in[] = { "xclip", "-selection", "clipboard", "-in", NULL };
	static const char *const xclip_out[] = { "xclip", "-selection", "clipboard", "-out", NULL };
	static const char *const xsel_in[] = { "xsel", "--clipboard", "--input", NULL };
	static const char *const xsel_out[] = { "xsel", "--clipboard", "--output", NULL };
	if (getenv ("WAYLAND_DISPLAY") != NULL && in_path ("wl-copy"))
		return paste ? wl_paste : wl_copy;
	if (getenv ("DISPLAY") != NULL) {
		if (in_path ("xclip"))
			return paste ? xclip_out : xclip_in;
		if (in_path ("xsel"))
			return paste ? xsel_out : xsel_in;
	}
	return NULL;
#endif
}

static void
remember (const char *text, size_t len)
{
	free (last_text);
	last_text = xmalloc (len + 1);
	memcpy (last_text, text, len);
	last_text[len] = '\0';
	last_len = len;
}

static void
send_osc52 (const char *text, size_t len)
{
	static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	if (len > MAX_OSC52_BYTES)
		return;
	StrBuf sb;
	sb_init (&sb);
	sb_add (&sb, "\033]52;c;");
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = (unsigned char) text[i] << 16;
		if (i + 1 < len)
			v |= (unsigned char) text[i + 1] << 8;
		if (i + 2 < len)
			v |= (unsigned char) text[i + 2];
		sb_addc (&sb, digits[v >> 18 & 63]);
		sb_addc (&sb, digits[v >> 12 & 63]);
		sb_addc (&sb, i + 1 < len ? digits[v >> 6 & 63] : '=');
		sb_addc (&sb, i + 2 < len ? digits[v & 63] : '=');
	}
	sb_add (&sb, "\a");
	term_write_raw (sb.s, sb.len);
	free (sb.s);
}

/* The kill ring text has changed; it goes to the clipboard once the
   command finishes, so that consecutive kills make one copy. */
void
clipboard_changed (void)
{
	pending = true;
}

void
clipboard_flush (void)
{
	if (!pending)
		return;
	pending = false;
	if (kill_ring_text == NULL || !enabled ())
		return;
	EstrView es = estr_view (kill_ring_text);
	if (last_text != NULL && last_len == es.len && memcmp (last_text, es.text, es.len) == 0)
		return;
	remember (es.text, es.len);
	const char *const *argv = local_command (false);
	if (argv == NULL || process_run (argv, NULL, es.text, es.len, NULL, NULL) != 0)
		send_osc52 (es.text, es.len);
}

/* Text another program has put on the clipboard since we last looked,
   or NULL. Only local clipboards can be read. */
char *
clipboard_fetch (size_t *len)
{
	clipboard_flush ();
	const char *const *argv = enabled () ? local_command (true) : NULL;
	if (argv == NULL)
		return NULL;
	StrBuf out;
	sb_init (&out);
	if (process_run (argv, NULL, NULL, 0, &out, NULL) != 0 || out.len == 0 ||
	    (last_text != NULL && last_len == out.len && memcmp (last_text, out.s, out.len) == 0)) {
		free (out.s);
		return NULL;
	}
	remember (out.s, out.len);
	*len = out.len;
	return out.s;
}
