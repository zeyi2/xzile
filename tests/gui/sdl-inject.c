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
 *   move ROW COL      SDL_MOUSEMOTION there with no button down
 *   release ROW COL   SDL_MOUSEBUTTONUP of the left button
 *   click ROW COL     press then release
 *   leave             SDL_WINDOWEVENT_LEAVE: the pointer left the window
 *   cursor NAME       check that the mouse pointer shown is the system
 *                     cursor NAME: arrow, ibeam, hand, sizens or sizewe
 *   sample NAME ROW COL  note how the cell looks in the last frame shown
 *   same NAME NAME    check that two cells noted looked the same
 *   differ NAME NAME  check that they did not
 * A failed check is reported, and makes the editor exit with status 1.
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
#include <unistd.h>
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
static int failures;

static SDL_Renderer *renderer;
static Uint32 *frame;		/* The last frame presented, in ARGB8888. */
static int frame_w, frame_h;

/* The system cursors made, standing in for those that the video driver
   cannot make (as the dummy one cannot), so that the pointer shown can
   still be checked. */
static struct { SDL_Cursor *cursor; SDL_SystemCursor id; } cursors[SDL_NUM_SYSTEM_CURSORS];
static char stand_ins[SDL_NUM_SYSTEM_CURSORS];
static int ncursors;
static int shown_cursor = -1;	/* The SDL_SystemCursor shown, or -1. */

static struct { char name[32]; Uint32 hash; } samples[64];
static int nsamples;

static void
check (int ok, const char *cmd, const char *arg)
{
	if (!ok) {
		fprintf (stderr, "sdl-inject: failed: %s %s\n", cmd, arg != NULL ? arg : "");
		failures++;
	}
}

__attribute__ ((destructor)) static void
report (void)
{
	if (failures > 0)
		_exit (1);
}

static int
cursor_named (const char *name)
{
	static const struct { const char *name; SDL_SystemCursor id; } names[] = {
		{ "arrow", SDL_SYSTEM_CURSOR_ARROW }, { "ibeam", SDL_SYSTEM_CURSOR_IBEAM },
		{ "hand", SDL_SYSTEM_CURSOR_HAND }, { "sizens", SDL_SYSTEM_CURSOR_SIZENS },
		{ "sizewe", SDL_SYSTEM_CURSOR_SIZEWE },
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (name != NULL && strcmp (name, names[i].name) == 0)
			return (int) names[i].id;
	return -2;
}

static Uint32 *
sample_named (const char *name)
{
	for (int i = 0; i < nsamples; i++)
		if (name != NULL && strcmp (samples[i].name, name) == 0)
			return &samples[i].hash;
	return NULL;
}

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

/* FNV-1a of the pixels of the cell at `arg' ("ROW COL") in the last
   frame presented. */
static Uint32
cell_hash (char *arg)
{
	char *col = arg != NULL ? strchr (arg, ' ') : NULL;
	if (col != NULL)
		*col++ = '\0';
	int x0 = cell_pixel (col, cursor.x, cursor.w) - cursor.w / 2;
	int y0 = cell_pixel (arg, cursor.y, cursor.h) - cursor.h / 2;
	Uint32 h = 2166136261u;
	for (int y = y0; y < y0 + cursor.h; y++)
		for (int x = x0; x < x0 + cursor.w; x++) {
			Uint32 p = frame != NULL && x >= 0 && y >= 0 && x < frame_w && y < frame_h ?
				frame[y * frame_w + x] : 0;
			h = (h ^ p) * 16777619u;
		}
	return h;
}

static void
mouse_event (SDL_Event *ev, const char *cmd, char *arg)
{
	char *col = arg != NULL ? strchr (arg, ' ') : NULL;
	if (col != NULL)
		*col++ = '\0';
	int x = cell_pixel (col, cursor.x, cursor.w), y = cell_pixel (arg, cursor.y, cursor.h);
	if (strcmp (cmd, "drag") == 0 || strcmp (cmd, "move") == 0) {
		ev->type = SDL_MOUSEMOTION;
		ev->motion.state = cmd[0] == 'd' ? SDL_BUTTON_LMASK : 0;
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
			   strcmp (line, "move") == 0 || strcmp (line, "release") == 0 ||
			   strcmp (line, "click") == 0) {
			mouse_event (ev, line, arg);
		} else if (strcmp (line, "leave") == 0) {
			ev->type = SDL_WINDOWEVENT;
			ev->window.event = SDL_WINDOWEVENT_LEAVE;
		} else if (strcmp (line, "cursor") == 0) {
			check (shown_cursor == cursor_named (arg), line, arg);
			continue;
		} else if (strcmp (line, "sample") == 0) {
			char *cell = arg != NULL ? strchr (arg, ' ') : NULL;
			if (cell == NULL || nsamples == (int) (sizeof samples / sizeof samples[0])) {
				check (0, line, arg);
				continue;
			}
			*cell++ = '\0';
			snprintf (samples[nsamples].name, sizeof samples[nsamples].name, "%s", arg);
			samples[nsamples++].hash = cell_hash (cell);
			continue;
		} else if (strcmp (line, "same") == 0 || strcmp (line, "differ") == 0) {
			char *b = arg != NULL ? strchr (arg, ' ') : NULL;
			if (b != NULL)
				*b++ = '\0';
			Uint32 *ha = sample_named (arg), *hb = sample_named (b);
			check (ha != NULL && hb != NULL && (*ha == *hb) == (line[0] == 's'), line, arg);
			continue;
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

SDL_Renderer *
SDL_CreateRenderer (SDL_Window *window, int index, Uint32 flags)
{
	static SDL_Renderer *(*real) (SDL_Window *, int, Uint32);
	if (real == NULL)
		real = (SDL_Renderer *(*) (SDL_Window *, int, Uint32)) dlsym (RTLD_NEXT, "SDL_CreateRenderer");
	SDL_Renderer *r = real (window, index, flags);
	if (r != NULL)
		renderer = r;
	return r;
}

/* Keep each frame, for sample. */
void
SDL_RenderPresent (SDL_Renderer *r)
{
	static void (*real) (SDL_Renderer *);
	if (real == NULL)
		real = (void (*) (SDL_Renderer *)) dlsym (RTLD_NEXT, "SDL_RenderPresent");
	int w, h;
	if (r == renderer && SDL_GetRendererOutputSize (r, &w, &h) == 0 && w > 0 && h > 0) {
		if (w != frame_w || h != frame_h) {
			free (frame);
			frame = malloc ((size_t) w * (size_t) h * sizeof *frame);
			frame_w = frame != NULL ? w : 0;
			frame_h = frame != NULL ? h : 0;
		}
		if (frame != NULL)
			SDL_RenderReadPixels (r, NULL, SDL_PIXELFORMAT_ARGB8888, frame, w * (int) sizeof *frame);
	}
	real (r);
}

SDL_Cursor *
SDL_CreateSystemCursor (SDL_SystemCursor id)
{
	static SDL_Cursor *(*real) (SDL_SystemCursor);
	if (real == NULL)
		real = (SDL_Cursor *(*) (SDL_SystemCursor)) dlsym (RTLD_NEXT, "SDL_CreateSystemCursor");
	SDL_Cursor *c = real (id);
	if (c == NULL && id >= 0 && id < SDL_NUM_SYSTEM_CURSORS)
		c = (SDL_Cursor *) &stand_ins[id];
	if (ncursors < SDL_NUM_SYSTEM_CURSORS) {
		cursors[ncursors].cursor = c;
		cursors[ncursors++].id = id;
	}
	return c;
}

void
SDL_SetCursor (SDL_Cursor *c)
{
	static void (*real) (SDL_Cursor *);
	if (real == NULL)
		real = (void (*) (SDL_Cursor *)) dlsym (RTLD_NEXT, "SDL_SetCursor");
	if (c != NULL) {
		shown_cursor = -1;
		for (int i = 0; i < ncursors; i++)
			if (cursors[i].cursor == c)
				shown_cursor = (int) cursors[i].id;
	}
	int stand_in = 0;
	for (int i = 0; i < SDL_NUM_SYSTEM_CURSORS; i++)
		stand_in |= c == (SDL_Cursor *) &stand_ins[i];
	if (!stand_in)
		real (c);
}
