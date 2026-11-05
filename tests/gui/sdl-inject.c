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
 * MODS is a comma-separated list of ctrl, shift, alt, ralt, gui, mode.
 * Events with no sleep between them arrive together, as SDL delivers
 * those of one key press.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

static FILE *script;
static int finished;
static Uint32 resume_at;

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

/* Fill `ev' with the next scripted event; 0 if none is due. */
static int
next_event (SDL_Event *ev)
{
	char line[1024];
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
