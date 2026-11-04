/* Getting and ungetting key strokes

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

/* Maximum time to avoid screen updates when catching up with buffered
   input, in milliseconds. */
#define MAX_RESYNC_MS 500

static Keystroke last_key;

/* Return last key pressed */
Keystroke
lastkey (void)
{
	return last_key;
}

/*
 * Get a keystroke, waiting for up to delay ms, and translate it into
 * a keycode.
 */
Keystroke
getkeystroke (int delay)
{
	last_key = term_getkey (delay);

	if (last_key != KBD_NOKEY && (thisflag & FLAG_DEFINING_MACRO))
		add_key_to_cmd (last_key);

	return last_key;
}

/*
 * Return the next keystroke, refreshing the screen only when the input
 * buffer is empty, or MAX_RESYNC_MS have elapsed since the last
 * screen refresh.
 */
static int64_t next_refresh = 0;

Keystroke
getkey (int delay)
{
	Keystroke keycode = getkeystroke (0);
	int64_t now = monotonic_ms ();

	if (keycode == KBD_NOKEY || now >= next_refresh) {
		term_redisplay ();
		term_refresh ();
		next_refresh = now + MAX_RESYNC_MS;
	}

	if (keycode == KBD_NOKEY)
		keycode = getkeystroke (delay);

	return keycode;
}

unsigned
getkey_unfiltered (int delay)
{
	unsigned key = term_getkey_unfiltered (delay);

	last_key = key;
	if (key != KBD_NOKEY && (thisflag & FLAG_DEFINING_MACRO))
		add_key_to_cmd (key);

	return key;
}

/*
 * Wait for GETKEY_DELAYED ms or until a key is pressed.  The key is
 * then available with [x]getkey.
 */
void
waitkey (void)
{
	Keystroke key = getkey (GETKEY_DELAYED);
	if (key != KBD_NOKEY)
		ungetkey (key);
}

/*
 * Push a key into the input buffer.
 */
void
pushkey (Keystroke key)
{
	term_ungetkey (key);
}

/*
 * Unget a key as if it had not been fetched.
 */
void
ungetkey (Keystroke key)
{
	pushkey (key);

	if (thisflag & FLAG_DEFINING_MACRO)
		remove_key_from_cmd ();
}
