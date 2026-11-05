/* LD_PRELOAD library for the xzile-gui tests: SDL_PollEvent returns
   the events scripted in the file named by $XZ_INJECT before any real
   ones.

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

/*
 * One command a line; `#' starts a comment:
 *   sleep MS          deliver nothing more for MS milliseconds
 *   down KEY [MODS]   SDL_KEYDOWN; KEY is an SDL key name ("a", "Return")
 *   up KEY [MODS]     SDL_KEYUP
 *   text STRING       SDL_TEXTINPUT with the rest of the line
 *   edit [STRING]     SDL_TEXTEDITING; empty when a composition ends
 *   press ROW COL     SDL_MOUSEBUTTONDOWN of the left button on a cell
 *   drag ROW COL      SDL_MOUSEMOTION there with the left button down
 *   release ROW COL   SDL_MOUSEBUTTONUP of the left button
 *   click ROW COL     press then release
 * MODS is a comma-separated list of ctrl, shift, alt, ralt, gui, mode.
 * ROW and COL count cells from 0 at the top left of the frame, or from
 * the cursor's cell when written `.', `.+N' or `.-N'. The cell size and
 * the cursor are those last given to SDL_SetTextInputRect, so the
 * cursor must have been drawn first.
 * Events with no sleep between them arrive together, as SDL delivers
 * those of one key press.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

/* The frame's padding, in pixels at a scale of 1 (as with the dummy
   video driver); PADDING in gui.c. */
#define PAD 10

static FILE *script;
static int finished;
static Uint32 resume_at;
static SDL_Rect cursor = { PAD, PAD, 8, 16 };
static SDL_Event pending;	/* The release of a click. */
static int have_pending;

static Uint16
parse_mods (char *s)
{
	static const struct { const char *name; Uint16 mod; } mods[] = {
		{ "ctrl", KMOD_LCTRL }, { "shift", KMOD_LSHIFT }, { "alt", KMOD_LALT },
		{ "ralt", KMOD_RALT }, { "gui", KMOD_LGUI }, { "mode", KMOD_MODE },
	};
	Uint16 m = 0;
	for (char *tok = s != NULL ? strtok (s, ",") : NULL; tok != NULL; tok = strtok (NULL, ",")) {
		size_t i = 0;
		while (i < sizeof mods / sizeof mods[0] && strcmp (tok, mods[i].name) != 0)
			i++;
		if (i < sizeof mods / sizeof mods[0])
			m |= mods[i].mod;
		else
			fprintf (stderr, "sdl-inject: unknown modifier `%s'\n", tok);
	}
	return m;
}

/* The pixel at the middle of the cell at row or column `s', the cells
   being `size' pixels, the cursor's `at'. */
static int
cell_pixel (const char *s, int at, int size)
{
	if (s == NULL || s[0] == '\0') {
		fprintf (stderr, "sdl-inject: missing cell coordinate\n");
		return 0;
	}
	int origin = PAD;
	if (s[0] == '.') {
		origin = at;
		s++;
	}
	return origin + atoi (s) * size + size / 2;
}

static void
mouse_event (SDL_Event *ev, const char *cmd, char *arg)
{
	char *col = arg != NULL ? strchr (arg, ' ') : NULL;
	if (col != NULL)
		*col++ = '\0';
	int x = cell_pixel (col, cursor.x, cursor.w), y = cell_pixel (arg, cursor.y, cursor.h);
	if (strcmp (cmd, "drag") == 0) {
		ev->type = SDL_MOUSEMOTION;
		ev->motion.state = SDL_BUTTON_LMASK;
		ev->motion.x = x;
		ev->motion.y = y;
		return;
	}
	ev->type = cmd[0] == 'r' ? SDL_MOUSEBUTTONUP : SDL_MOUSEBUTTONDOWN;
	ev->button.button = SDL_BUTTON_LEFT;
	ev->button.state = cmd[0] == 'r' ? SDL_RELEASED : SDL_PRESSED;
	ev->button.clicks = 1;
	ev->button.x = x;
	ev->button.y = y;
	if (cmd[0] == 'c') {
		pending = *ev;
		pending.type = SDL_MOUSEBUTTONUP;
		pending.button.state = SDL_RELEASED;
		have_pending = 1;
	}
}

/* Fill `ev' with the next scripted event; 0 if none is due. */
static int
next_event (SDL_Event *ev)
{
	char line[1024];
	if (have_pending) {
		*ev = pending;
		ev->common.timestamp = SDL_GetTicks ();
		have_pending = 0;
		return 1;
	}
	if (script == NULL && !finished) {
		const char *path = getenv ("XZ_INJECT");
		if (path == NULL || (script = fopen (path, "r")) == NULL)
			finished = 1;
	}
	while (!finished) {
		if (resume_at != 0 && !SDL_TICKS_PASSED (SDL_GetTicks (), resume_at))
			return 0;
		resume_at = 0;
		if (fgets (line, sizeof line, script) == NULL) {
			fclose (script);
			finished = 1;
			break;
		}
		line[strcspn (line, "\n")] = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		char *arg = strchr (line, ' ');
		if (arg != NULL)
			*arg++ = '\0';
		memset (ev, 0, sizeof *ev);
		if (strcmp (line, "sleep") == 0) {
			resume_at = SDL_GetTicks () + (Uint32) atoi (arg != NULL ? arg : "0");
			continue;
		} else if (strcmp (line, "down") == 0 || strcmp (line, "up") == 0) {
			char *mods = arg != NULL ? strchr (arg, ' ') : NULL;
			if (mods != NULL)
				*mods++ = '\0';
			SDL_Keycode sym = SDL_GetKeyFromName (arg != NULL ? arg : "");
			if (sym == SDLK_UNKNOWN)
				fprintf (stderr, "sdl-inject: unknown key `%s'\n", arg != NULL ? arg : "");
			ev->type = line[0] == 'd' ? SDL_KEYDOWN : SDL_KEYUP;
			ev->key.state = line[0] == 'd' ? SDL_PRESSED : SDL_RELEASED;
			ev->key.keysym.sym = sym;
			ev->key.keysym.scancode = SDL_GetScancodeFromKey (sym);
			ev->key.keysym.mod = parse_mods (mods);
		} else if (strcmp (line, "text") == 0) {
			ev->type = SDL_TEXTINPUT;
			snprintf (ev->text.text, sizeof ev->text.text, "%s", arg != NULL ? arg : "");
		} else if (strcmp (line, "edit") == 0) {
			ev->type = SDL_TEXTEDITING;
			snprintf (ev->edit.text, sizeof ev->edit.text, "%s", arg != NULL ? arg : "");
			ev->edit.start = ev->edit.length = (Sint32) SDL_utf8strlen (ev->edit.text);
		} else if (strcmp (line, "press") == 0 || strcmp (line, "drag") == 0 ||
			   strcmp (line, "release") == 0 || strcmp (line, "click") == 0) {
			mouse_event (ev, line, arg);
		} else {
			fprintf (stderr, "sdl-inject: unknown command `%s'\n", line);
			continue;
		}
		ev->common.timestamp = SDL_GetTicks ();
		return 1;
	}
	return 0;
}

int
SDL_PollEvent (SDL_Event *ev)
{
	static int (*real) (SDL_Event *);
	if (real == NULL)
		real = (int (*) (SDL_Event *)) dlsym (RTLD_NEXT, "SDL_PollEvent");
	SDL_Event tmp;
	return next_event (ev != NULL ? ev : &tmp) || real (ev);
}

void
SDL_SetTextInputRect (const SDL_Rect *rect)
{
	static void (*real) (const SDL_Rect *);
	if (real == NULL)
		real = (void (*) (const SDL_Rect *)) dlsym (RTLD_NEXT, "SDL_SetTextInputRect");
	if (rect != NULL && rect->w > 0 && rect->h > 0)
		cursor = *rect;
	real (rect);
}

/* Wake up often while events are still to come. */
int
SDL_WaitEventTimeout (SDL_Event *ev, int timeout)
{
	static int (*real) (SDL_Event *, int);
	if (real == NULL)
		real = (int (*) (SDL_Event *, int)) dlsym (RTLD_NEXT, "SDL_WaitEventTimeout");
	if (!finished && (timeout < 0 || timeout > 5))
		timeout = 5;
	return real (ev, timeout);
}
