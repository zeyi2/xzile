/* Terminal emulator

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

/* An xterm-compatible screen, as described by TERM=xterm-256color:
   cursor addressing, scrolling regions, insertion and deletion,
   deferred wrapping at the right margin, the alternate screen, 256 and
   24-bit colours, DEC line drawing, bracketed paste, mouse tracking,
   cursor and device reports, and a scrollback of lines scrolled off the main screen. */

#include "xzile.h"

#include <ctype.h>
#include <wchar.h>

#define MAX_PARAMS 32
#define MAX_OSC 4096
#define SCROLLBACK_LINES 5000

enum ParseState {
	S_GROUND,
	S_ESCAPE,
	S_ESCAPE_INTER,		/* ESC followed by an intermediate, e.g. ESC ( */
	S_CSI,
	S_OSC,
	S_OSC_ESC,
	S_STRING,		/* DCS, SOS, PM, APC: ignored up to ST */
	S_STRING_ESC,
};

typedef struct {
	VCell *cells;
	size_t len;
} ScrollLine;

typedef struct {
	int x, y;
	VAttr attr;
	bool origin, wrap_pending;
	bool graphics;
} SavedCursor;

struct VTerm {
	int rows, cols;
	VCell *main, *alt, *screen;
	bool on_alt;
	int cx, cy;
	bool wrap_pending;
	VAttr attr;
	int top, bot;		/* Scrolling region, inclusive. */
	bool origin, autowrap, insert;
	bool app_cursor, app_keypad, cursor_visible, bracketed_paste;
	int mouse_mode;		/* 0, or the DECSET mode: 9, 1000, 1002 or 1003. */
	int mouse_enc;		/* 0 (X10 bytes), or 1005, 1006 or 1015. */
	bool *tabs;
	bool g0_graphics, g1_graphics, shift_out;
	SavedCursor saved_main, saved_alt;
	uint32_t last_char;

	ScrollLine *sb;		/* Scrollback ring. */
	size_t sb_start, sb_len;
	size_t scrolled;	/* Lines ever pushed to the scrollback. */

	enum ParseState state;
	char inter;
	char private_mark;
	int params[MAX_PARAMS];
	bool param_colon[MAX_PARAMS];	/* The parameter is followed by ':'. */
	int nparams;
	bool param_started;
	char osc[MAX_OSC];
	size_t osc_len;

	unsigned char utf8[4];
	int utf8_len, utf8_need;

	VTermWriteFn write;
	VTermOscFn osc_fn;
	void *data;
};

static const VAttr default_attr = { VCOLOR_DEFAULT, VCOLOR_DEFAULT, 0 };


/* Cells and lines. */

static VCell *
cell (VTerm *t, int y, int x)
{
	return &t->screen[(size_t) y * (size_t) t->cols + (size_t) x];
}

static void
blank_cells (VTerm *t, VCell *c, int n)
{
	/* Erased cells take the current background (back colour erase). */
	for (int i = 0; i < n; i++) {
		c[i].cp = ' ';
		c[i].width = 1;
		c[i].attr.fg = VCOLOR_DEFAULT;
		c[i].attr.bg = t->attr.bg;
		c[i].attr.flags = 0;
	}
}

static void
erase (VTerm *t, int y, int x0, int x1)
{
	if (y < 0 || y >= t->rows)
		return;
	x0 = MAX (x0, 0);
	x1 = MIN (x1, t->cols);
	if (x0 < x1)
		blank_cells (t, cell (t, y, x0), x1 - x0);
	/* Do not leave half a wide character behind. */
	if (x0 > 0 && x0 < t->cols && cell (t, y, x0 - 1)->width == 2)
		blank_cells (t, cell (t, y, x0 - 1), 1);
	if (x1 < t->cols && cell (t, y, x1)->width == 0)
		blank_cells (t, cell (t, y, x1), 1);
}

static void
push_scrollback (VTerm *t, const VCell *line)
{
	size_t len = (size_t) t->cols;
	while (len > 0 && line[len - 1].cp == ' ' && line[len - 1].attr.bg == VCOLOR_DEFAULT &&
	       !(line[len - 1].attr.flags & VATTR_REVERSE))
		len--;

	ScrollLine *sl;
	if (t->sb_len < SCROLLBACK_LINES)
		sl = &t->sb[(t->sb_start + t->sb_len++) % SCROLLBACK_LINES];
	else {
		sl = &t->sb[t->sb_start];
		t->sb_start = (t->sb_start + 1) % SCROLLBACK_LINES;
		free (sl->cells);
	}
	sl->len = len;
	sl->cells = xmalloc ((len ? len : 1) * sizeof (VCell));
	memcpy (sl->cells, line, len * sizeof (VCell));
	t->scrolled++;
}

/* Scroll lines [top, bot] up by n. */
static void
scroll_up (VTerm *t, int top, int bot, int n)
{
	if (n <= 0 || top > bot)
		return;
	n = MIN (n, bot - top + 1);
	if (top == 0 && !t->on_alt)
		for (int i = 0; i < n; i++)
			push_scrollback (t, cell (t, i, 0));
	size_t w = (size_t) t->cols;
	memmove (cell (t, top, 0), cell (t, top + n, 0), (size_t) (bot - top + 1 - n) * w * sizeof (VCell));
	for (int y = bot - n + 1; y <= bot; y++)
		blank_cells (t, cell (t, y, 0), t->cols);
}

/* Scroll lines [top, bot] down by n. */
static void
scroll_down (VTerm *t, int top, int bot, int n)
{
	if (n <= 0 || top > bot)
		return;
	n = MIN (n, bot - top + 1);
	size_t w = (size_t) t->cols;
	memmove (cell (t, top + n, 0), cell (t, top, 0), (size_t) (bot - top + 1 - n) * w * sizeof (VCell));
	for (int y = top; y < top + n; y++)
		blank_cells (t, cell (t, y, 0), t->cols);
}


/* Creation and sizing. */

static void
reset_tabs (VTerm *t)
{
	free (t->tabs);
	t->tabs = xzalloc ((size_t) t->cols * sizeof (bool));
	for (int x = 8; x < t->cols; x += 8)
		t->tabs[x] = true;
}

static VCell *
new_screen (VTerm *t)
{
	VCell *s = xmalloc ((size_t) t->rows * (size_t) t->cols * sizeof (VCell));
	VAttr saved = t->attr;
	t->attr = default_attr;
	blank_cells (t, s, t->rows * t->cols);
	t->attr = saved;
	return s;
}

static void
reset (VTerm *t)
{
	t->attr = default_attr;
	t->cx = t->cy = 0;
	t->wrap_pending = false;
	t->top = 0;
	t->bot = t->rows - 1;
	t->origin = t->insert = false;
	t->autowrap = true;
	t->app_cursor = t->app_keypad = false;
	t->cursor_visible = true;
	t->bracketed_paste = false;
	t->mouse_mode = t->mouse_enc = 0;
	t->g0_graphics = t->g1_graphics = t->shift_out = false;
	t->on_alt = false;
	t->screen = t->main;
	t->saved_main.x = t->saved_main.y = 0;
	t->saved_main.attr = default_attr;
	t->saved_alt = t->saved_main;
	reset_tabs (t);
}

VTerm *
vterm_new (int rows, int cols, VTermWriteFn write, VTermOscFn osc_fn, void *data)
{
	VTerm *t = xzalloc (sizeof (VTerm));
	t->rows = MAX (rows, 1);
	t->cols = MAX (cols, 1);
	t->attr = default_attr;
	t->main = new_screen (t);
	t->alt = new_screen (t);
	t->sb = xzalloc (SCROLLBACK_LINES * sizeof (ScrollLine));
	t->write = write;
	t->osc_fn = osc_fn;
	t->data = data;
	reset (t);
	return t;
}

void
vterm_free (VTerm *t)
{
	if (t == NULL)
		return;
	for (size_t i = 0; i < t->sb_len; i++)
		free (t->sb[(t->sb_start + i) % SCROLLBACK_LINES].cells);
	free (t->sb);
	free (t->main);
	free (t->alt);
	free (t->tabs);
	free (t);
}

static VCell *
resized_copy (VTerm *t, VCell *old, int old_rows, int old_cols, int skip)
{
	VCell *s = new_screen (t);
	for (int y = 0; y < t->rows && y + skip < old_rows; y++)
		for (int x = 0; x < t->cols && x < old_cols; x++)
			s[(size_t) y * (size_t) t->cols + (size_t) x] =
				old[(size_t) (y + skip) * (size_t) old_cols + (size_t) x];
	/* Fix a wide character cut in half by the new right margin. */
	for (int y = 0; y < t->rows; y++) {
		VCell *last = &s[(size_t) y * (size_t) t->cols + (size_t) t->cols - 1];
		if (last->width == 2) {
			last->cp = ' ';
			last->width = 1;
		}
	}
	return s;
}

void
vterm_resize (VTerm *t, int rows, int cols)
{
	rows = MAX (rows, 1);
	cols = MAX (cols, 1);
	if (rows == t->rows && cols == t->cols)
		return;

	int old_rows = t->rows, old_cols = t->cols;
	/* Keep the cursor line on the main screen, pushing lines off the top. */
	int skip = 0;
	if (t->cy >= rows)
		skip = t->cy - rows + 1;
	if (!t->on_alt)
		for (int i = 0; i < skip; i++)
			push_scrollback (t, &t->main[(size_t) i * (size_t) old_cols]);

	t->rows = rows;
	t->cols = cols;
	VCell *main = resized_copy (t, t->main, old_rows, old_cols, t->on_alt ? 0 : skip);
	VCell *alt = resized_copy (t, t->alt, old_rows, old_cols, t->on_alt ? skip : 0);
	free (t->main);
	free (t->alt);
	t->main = main;
	t->alt = alt;
	t->screen = t->on_alt ? alt : main;

	t->cy -= skip;
	t->cy = MIN (MAX (t->cy, 0), rows - 1);
	t->cx = MIN (t->cx, cols - 1);
	t->wrap_pending = false;
	t->top = 0;
	t->bot = rows - 1;
	reset_tabs (t);
}


/* Accessors. */

int
vterm_rows (const VTerm *t)
{
	return t->rows;
}

int
vterm_cols (const VTerm *t)
{
	return t->cols;
}

const VCell *
vterm_cell (const VTerm *t, int y, int x)
{
	return &t->screen[(size_t) y * (size_t) t->cols + (size_t) x];
}

void
vterm_cursor (const VTerm *t, int *y, int *x)
{
	*y = t->cy;
	*x = t->cx;
}

bool
vterm_cursor_visible (const VTerm *t)
{
	return t->cursor_visible;
}

bool
vterm_app_cursor (const VTerm *t)
{
	return t->app_cursor;
}

bool
vterm_bracketed_paste (const VTerm *t)
{
	return t->bracketed_paste;
}

bool
vterm_mouse_tracking (const VTerm *t)
{
	return t->mouse_mode != 0;
}

size_t
vterm_scrollback_len (const VTerm *t)
{
	return t->sb_len;
}

size_t
vterm_scrolled (const VTerm *t)
{
	return t->scrolled;
}

const VCell *
vterm_scrollback_line (const VTerm *t, size_t i, size_t *len)
{
	const ScrollLine *sl = &t->sb[(t->sb_start + i) % SCROLLBACK_LINES];
	*len = sl->len;
	return sl->cells;
}


/* Output. */

static void
reply (VTerm *t, const char *s)
{
	if (t->write != NULL)
		t->write (s, strlen (s), t->data);
}

bool
vterm_mouse (VTerm *t, int button, enum VMouseAction action, int y, int x)
{
	bool wheel = button >= 64;
	if (t->mouse_mode == 0 ||
	    (t->mouse_mode == 9 && (action != VMOUSE_PRESS || wheel)) ||
	    (action == VMOUSE_DRAG && t->mouse_mode < 1002) ||
	    (action == VMOUSE_RELEASE && wheel))
		return false;
	x = MIN (MAX (x, 0), t->cols - 1) + 1;
	y = MIN (MAX (y, 0), t->rows - 1) + 1;
	int cb = button + (action == VMOUSE_DRAG ? 32 : 0);

	char buf[32];
	if (t->mouse_enc == 1006)
		snprintf (buf, sizeof buf, "\033[<%d;%d;%d%c", cb, x, y, action == VMOUSE_RELEASE ? 'm' : 'M');
	else {
		/* The other encodings cannot say which button was released. */
		if (action == VMOUSE_RELEASE)
			cb = 3;
		if (t->mouse_enc == 1015)
			snprintf (buf, sizeof buf, "\033[%d;%d;%dM", cb + 32, x, y);
		else if (t->mouse_enc == 1005) {
			if (x > 2015 || y > 2015)
				return true;
			size_t n = (size_t) snprintf (buf, sizeof buf, "\033[M");
			n += utf8_encode ((uint32_t) cb + 32, buf + n);
			n += utf8_encode ((uint32_t) x + 32, buf + n);
			n += utf8_encode ((uint32_t) y + 32, buf + n);
			buf[n] = '\0';
		} else {
			if (x > 223 || y > 223)
				return true;
			snprintf (buf, sizeof buf, "\033[M%c%c%c", cb + 32, x + 32, y + 32);
		}
	}
	reply (t, buf);
	return true;
}

static void
index_line (VTerm *t)
{
	if (t->cy == t->bot)
		scroll_up (t, t->top, t->bot, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

static void
reverse_index (VTerm *t)
{
	if (t->cy == t->top)
		scroll_down (t, t->top, t->bot, 1);
	else if (t->cy > 0)
		t->cy--;
}

/* DEC special graphics, for line drawing. */
static uint32_t
dec_graphics (uint32_t c)
{
	static const uint16_t map[] = {
		/* ` a b c d e f g h i j k l m n o */
		0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
		0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
		/* p q r s t u v w x y z { | } ~ */
		0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
		0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
	};
	if (c >= '`' && c <= '~')
		return map[c - '`'];
	return c;
}

static void
put_char (VTerm *t, uint32_t c)
{
	if ((t->shift_out ? t->g1_graphics : t->g0_graphics) && c < 0x80)
		c = dec_graphics (c);

	int w = wcwidth ((wchar_t) c);
	if (w < 0)
		w = 1;
	if (w == 0) {
		/* Combining characters are not kept. */
		return;
	}
	if (w > 2)
		w = 2;

	if (t->wrap_pending && t->autowrap) {
		t->cx = 0;
		index_line (t);
	}
	t->wrap_pending = false;

	if (w == 2 && t->cx == t->cols - 1) {
		if (t->autowrap) {
			blank_cells (t, cell (t, t->cy, t->cx), 1);
			t->cx = 0;
			index_line (t);
		} else
			w = 1;
	}
	if (w == 2 && t->cols < 2)
		w = 1;

	if (t->insert) {
		int n = t->cols - t->cx - w;
		if (n > 0)
			memmove (cell (t, t->cy, t->cx + w), cell (t, t->cy, t->cx), (size_t) n * sizeof (VCell));
	}

	/* Overwriting half of a wide character clears the other half. */
	VCell *c0 = cell (t, t->cy, t->cx);
	if (c0->width == 0 && t->cx > 0)
		blank_cells (t, c0 - 1, 1);
	if (c0[w - 1].width == 2 && t->cx + w < t->cols)
		blank_cells (t, &c0[w], 1);

	c0->cp = c;
	c0->width = (uint8_t) w;
	c0->attr = t->attr;
	if (w == 2) {
		c0[1].cp = 0;
		c0[1].width = 0;
		c0[1].attr = t->attr;
	}
	t->last_char = c;

	if (t->cx + w >= t->cols) {
		t->cx = t->cols - 1;
		t->wrap_pending = t->autowrap;
	} else
		t->cx += w;
}

static void
move_to (VTerm *t, int y, int x)
{
	int top = t->origin ? t->top : 0, bot = t->origin ? t->bot : t->rows - 1;
	t->cy = MIN (MAX (y + top, top), bot);
	t->cx = MIN (MAX (x, 0), t->cols - 1);
	t->wrap_pending = false;
}

static void
save_cursor (VTerm *t)
{
	SavedCursor *s = t->on_alt ? &t->saved_alt : &t->saved_main;
	s->x = t->cx;
	s->y = t->cy;
	s->attr = t->attr;
	s->origin = t->origin;
	s->wrap_pending = t->wrap_pending;
	s->graphics = t->g0_graphics;
}

static void
restore_cursor (VTerm *t)
{
	SavedCursor *s = t->on_alt ? &t->saved_alt : &t->saved_main;
	t->cx = MIN (s->x, t->cols - 1);
	t->cy = MIN (s->y, t->rows - 1);
	t->attr = s->attr;
	t->origin = s->origin;
	t->wrap_pending = s->wrap_pending;
	t->g0_graphics = s->graphics;
}

static void
set_alt_screen (VTerm *t, bool on, bool clear)
{
	if (on == t->on_alt)
		return;
	t->on_alt = on;
	t->screen = on ? t->alt : t->main;
	if (on && clear) {
		VAttr saved = t->attr;
		t->attr = default_attr;
		blank_cells (t, t->alt, t->rows * t->cols);
		t->attr = saved;
	}
	t->wrap_pending = false;
}

static int
param (VTerm *t, int i, int def)
{
	return i < t->nparams && t->params[i] > 0 ? t->params[i] : def;
}

static int
param0 (VTerm *t, int i)
{
	return i < t->nparams ? t->params[i] : 0;
}

static void
set_mode (VTerm *t, bool on)
{
	for (int i = 0; i < MAX (t->nparams, 1); i++) {
		int m = param0 (t, i);
		if (t->private_mark == '?')
			switch (m) {
			case 1: t->app_cursor = on; break;
			case 6: t->origin = on; move_to (t, 0, 0); break;
			case 7: t->autowrap = on; if (!on) t->wrap_pending = false; break;
			case 25: t->cursor_visible = on; break;
			case 47:
			case 1047: set_alt_screen (t, on, on); break;
			case 1048: if (on) save_cursor (t); else restore_cursor (t); break;
			case 1049:
				if (on) {
					save_cursor (t);
					set_alt_screen (t, true, true);
				} else {
					set_alt_screen (t, false, false);
					restore_cursor (t);
				}
				break;
			case 2004: t->bracketed_paste = on; break;
			case 9:
			case 1000:
			case 1002:
			case 1003: t->mouse_mode = on ? m : 0; break;
			case 1005:
			case 1006:
			case 1015:
				if (on)
					t->mouse_enc = m;
				else if (t->mouse_enc == m)
					t->mouse_enc = 0;
				break;
			default: break;
			}
		else if (t->private_mark == 0 && m == 4)
			t->insert = on;
	}
}

/* Parse an extended colour (38/48) starting at parameter `*i'. */
static int32_t
extended_color (VTerm *t, int *i)
{
	int k = *i + 1;
	if (k >= t->nparams)
		return VCOLOR_DEFAULT - 1;
	if (t->params[k] == 5 && k + 1 < t->nparams) {
		*i = k + 1;
		return MIN (MAX (t->params[k + 1], 0), 255);
	}
	if (t->params[k] == 2) {
		/* 38;2;r;g;b, or 38:2:cs:r:g:b with a colour space. */
		int first = k + 1;
		if (t->param_colon[k] && first + 3 < t->nparams && t->param_colon[first + 2])
			first++;
		if (first + 2 < t->nparams) {
			*i = first + 2;
			int r = MIN (MAX (t->params[first], 0), 255);
			int g = MIN (MAX (t->params[first + 1], 0), 255);
			int b = MIN (MAX (t->params[first + 2], 0), 255);
			return VCOLOR_RGB (r, g, b);
		}
	}
	*i = t->nparams;
	return VCOLOR_DEFAULT - 1;
}

static void
sgr (VTerm *t)
{
	if (t->nparams == 0) {
		t->attr = default_attr;
		return;
	}
	for (int i = 0; i < t->nparams; i++) {
		int p = t->params[i];
		int32_t c;
		switch (p) {
		case 0: t->attr = default_attr; break;
		case 1: t->attr.flags |= VATTR_BOLD; break;
		case 2: t->attr.flags |= VATTR_DIM; break;
		case 3: t->attr.flags |= VATTR_ITALIC; break;
		case 4:
			/* 4:0 turns underlining off. */
			if (t->param_colon[i] && i + 1 < t->nparams) {
				if (t->params[++i] == 0)
					t->attr.flags &= (uint16_t) ~VATTR_UNDERLINE;
				else
					t->attr.flags |= VATTR_UNDERLINE;
			} else
				t->attr.flags |= VATTR_UNDERLINE;
			break;
		case 7: t->attr.flags |= VATTR_REVERSE; break;
		case 8: t->attr.flags |= VATTR_INVISIBLE; break;
		case 9: t->attr.flags |= VATTR_STRIKE; break;
		case 21: t->attr.flags |= VATTR_UNDERLINE; break;
		case 22: t->attr.flags &= (uint16_t) ~(VATTR_BOLD | VATTR_DIM); break;
		case 23: t->attr.flags &= (uint16_t) ~VATTR_ITALIC; break;
		case 24: t->attr.flags &= (uint16_t) ~VATTR_UNDERLINE; break;
		case 27: t->attr.flags &= (uint16_t) ~VATTR_REVERSE; break;
		case 28: t->attr.flags &= (uint16_t) ~VATTR_INVISIBLE; break;
		case 29: t->attr.flags &= (uint16_t) ~VATTR_STRIKE; break;
		case 38:
			if ((c = extended_color (t, &i)) >= VCOLOR_DEFAULT)
				t->attr.fg = c;
			break;
		case 39: t->attr.fg = VCOLOR_DEFAULT; break;
		case 48:
			if ((c = extended_color (t, &i)) >= VCOLOR_DEFAULT)
				t->attr.bg = c;
			break;
		case 49: t->attr.bg = VCOLOR_DEFAULT; break;
		default:
			if (p >= 30 && p <= 37)
				t->attr.fg = p - 30;
			else if (p >= 40 && p <= 47)
				t->attr.bg = p - 40;
			else if (p >= 90 && p <= 97)
				t->attr.fg = p - 90 + 8;
			else if (p >= 100 && p <= 107)
				t->attr.bg = p - 100 + 8;
			/* 5, 6 (blink) and others are ignored. */
			break;
		}
	}
}

static void
csi_dispatch (VTerm *t, char f)
{
	char buf[64];
	int n;

	if (t->inter != 0) {
		if (t->inter == '!' && f == 'p')	/* DECSTR: soft reset */
			reset (t);
		/* DECSCUSR (SP q) and others are ignored. */
		return;
	}
	if (t->private_mark != 0 && t->private_mark != '?' && f != 'c')
		return;

	switch (f) {
	case '@':		/* ICH */
		n = MIN (param (t, 0, 1), t->cols - t->cx);
		memmove (cell (t, t->cy, t->cx + n), cell (t, t->cy, t->cx),
			 (size_t) (t->cols - t->cx - n) * sizeof (VCell));
		blank_cells (t, cell (t, t->cy, t->cx), n);
		t->wrap_pending = false;
		break;
	case 'A':		/* CUU */
		t->cy = MAX (t->cy - param (t, 0, 1), t->cy >= t->top ? t->top : 0);
		t->wrap_pending = false;
		break;
	case 'B':		/* CUD */
	case 'e':		/* VPR */
		t->cy = MIN (t->cy + param (t, 0, 1), t->cy <= t->bot ? t->bot : t->rows - 1);
		t->wrap_pending = false;
		break;
	case 'C':		/* CUF */
	case 'a':		/* HPR */
		t->cx = MIN (t->cx + param (t, 0, 1), t->cols - 1);
		t->wrap_pending = false;
		break;
	case 'D':		/* CUB */
		t->cx = MAX (t->cx - param (t, 0, 1), 0);
		t->wrap_pending = false;
		break;
	case 'E':		/* CNL */
		t->cy = MIN (t->cy + param (t, 0, 1), t->bot);
		t->cx = 0;
		t->wrap_pending = false;
		break;
	case 'F':		/* CPL */
		t->cy = MAX (t->cy - param (t, 0, 1), t->top);
		t->cx = 0;
		t->wrap_pending = false;
		break;
	case 'G':		/* CHA */
	case '`':		/* HPA */
		t->cx = MIN (param (t, 0, 1) - 1, t->cols - 1);
		t->wrap_pending = false;
		break;
	case 'H':		/* CUP */
	case 'f':		/* HVP */
		move_to (t, param (t, 0, 1) - 1, param (t, 1, 1) - 1);
		break;
	case 'I':		/* CHT */
		for (n = param (t, 0, 1); n > 0; n--) {
			do
				t->cx++;
			while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
			t->cx = MIN (t->cx, t->cols - 1);
		}
		t->wrap_pending = false;
		break;
	case 'J':		/* ED */
		switch (param0 (t, 0)) {
		case 0:
			erase (t, t->cy, t->cx, t->cols);
			for (int y = t->cy + 1; y < t->rows; y++)
				erase (t, y, 0, t->cols);
			break;
		case 1:
			for (int y = 0; y < t->cy; y++)
				erase (t, y, 0, t->cols);
			erase (t, t->cy, 0, t->cx + 1);
			break;
		case 2:
			for (int y = 0; y < t->rows; y++)
				erase (t, y, 0, t->cols);
			break;
		case 3:
			for (size_t i = 0; i < t->sb_len; i++)
				free (t->sb[(t->sb_start + i) % SCROLLBACK_LINES].cells);
			t->sb_start = t->sb_len = 0;
			break;
		}
		t->wrap_pending = false;
		break;
	case 'K':		/* EL */
		switch (param0 (t, 0)) {
		case 0: erase (t, t->cy, t->cx, t->cols); break;
		case 1: erase (t, t->cy, 0, t->cx + 1); break;
		case 2: erase (t, t->cy, 0, t->cols); break;
		}
		t->wrap_pending = false;
		break;
	case 'L':		/* IL */
		if (t->cy >= t->top && t->cy <= t->bot) {
			scroll_down (t, t->cy, t->bot, param (t, 0, 1));
			t->cx = 0;
		}
		t->wrap_pending = false;
		break;
	case 'M':		/* DL */
		if (t->cy >= t->top && t->cy <= t->bot) {
			/* Deleted lines do not go to the scrollback. */
			bool alt = t->on_alt;
			t->on_alt = true;
			scroll_up (t, t->cy, t->bot, param (t, 0, 1));
			t->on_alt = alt;
			t->cx = 0;
		}
		t->wrap_pending = false;
		break;
	case 'P':		/* DCH */
		n = MIN (param (t, 0, 1), t->cols - t->cx);
		memmove (cell (t, t->cy, t->cx), cell (t, t->cy, t->cx + n),
			 (size_t) (t->cols - t->cx - n) * sizeof (VCell));
		blank_cells (t, cell (t, t->cy, t->cols - n), n);
		t->wrap_pending = false;
		break;
	case 'S':		/* SU */
		if (t->private_mark == 0)
			scroll_up (t, t->top, t->bot, param (t, 0, 1));
		break;
	case 'T':		/* SD */
		if (t->private_mark == 0 && t->nparams <= 1)
			scroll_down (t, t->top, t->bot, param (t, 0, 1));
		break;
	case 'X':		/* ECH */
		erase (t, t->cy, t->cx, t->cx + param (t, 0, 1));
		t->wrap_pending = false;
		break;
	case 'Z':		/* CBT */
		for (n = param (t, 0, 1); n > 0 && t->cx > 0; n--)
			do
				t->cx--;
			while (t->cx > 0 && !t->tabs[t->cx]);
		t->wrap_pending = false;
		break;
	case 'b':		/* REP */
		if (t->last_char != 0)
			for (n = MIN (param (t, 0, 1), 65535); n > 0; n--)
				put_char (t, t->last_char);
		break;
	case 'c':		/* DA */
		if (t->private_mark == '>')
			reply (t, "\033[>0;276;0c");
		else if (t->private_mark == 0)
			reply (t, "\033[?62;22c");
		break;
	case 'd':		/* VPA */
		move_to (t, param (t, 0, 1) - 1, t->cx);
		break;
	case 'g':		/* TBC */
		if (param0 (t, 0) == 0)
			t->tabs[t->cx] = false;
		else if (param0 (t, 0) == 3)
			memset (t->tabs, 0, (size_t) t->cols * sizeof (bool));
		break;
	case 'h':
		set_mode (t, true);
		break;
	case 'l':
		set_mode (t, false);
		break;
	case 'm':
		if (t->private_mark == 0)
			sgr (t);
		break;
	case 'n':		/* DSR */
		if (param0 (t, 0) == 5)
			reply (t, "\033[0n");
		else if (param0 (t, 0) == 6) {
			snprintf (buf, sizeof buf, "\033[%s%d;%dR", t->private_mark == '?' ? "?" : "",
				  t->cy - (t->origin ? t->top : 0) + 1, t->cx + 1);
			reply (t, buf);
		}
		break;
	case 'r':		/* DECSTBM */
		if (t->private_mark == 0) {
			int top = param (t, 0, 1) - 1, bot = param (t, 1, t->rows) - 1;
			bot = MIN (bot, t->rows - 1);
			if (top < bot) {
				t->top = top;
				t->bot = bot;
				move_to (t, 0, 0);
			}
		}
		break;
	case 's':		/* SCOSC */
		if (t->private_mark == 0)
			save_cursor (t);
		break;
	case 'u':		/* SCORC */
		if (t->private_mark == 0)
			restore_cursor (t);
		break;
	case 't':		/* Window operations */
		if (param0 (t, 0) == 18) {
			snprintf (buf, sizeof buf, "\033[8;%d;%dt", t->rows, t->cols);
			reply (t, buf);
		}
		break;
	default:
		break;
	}
}

static void
esc_dispatch (VTerm *t, char f)
{
	if (t->inter == '(' || t->inter == ')') {
		bool graphics = f == '0';
		if (t->inter == '(')
			t->g0_graphics = graphics;
		else
			t->g1_graphics = graphics;
		return;
	}
	if (t->inter != 0)
		return;	/* ESC # 8 and the like */

	switch (f) {
	case '7': save_cursor (t); break;
	case '8': restore_cursor (t); break;
	case 'D': index_line (t); t->wrap_pending = false; break;
	case 'E': t->cx = 0; index_line (t); t->wrap_pending = false; break;
	case 'H': t->tabs[t->cx] = true; break;
	case 'M': reverse_index (t); t->wrap_pending = false; break;
	case 'c':
		reset (t);
		for (int y = 0; y < t->rows; y++)
			erase (t, y, 0, t->cols);
		break;
	case '=': t->app_keypad = true; break;
	case '>': t->app_keypad = false; break;
	default: break;
	}
}

static void
osc_dispatch (VTerm *t)
{
	t->osc[t->osc_len] = '\0';
	char *semi = strchr (t->osc, ';');
	if (semi == NULL || t->osc_fn == NULL)
		return;
	*semi = '\0';
	long n;
	if (parse_long (t->osc, &n))
		t->osc_fn ((int) n, semi + 1, t->data);
}

static void
start_sequence (VTerm *t, enum ParseState state)
{
	t->state = state;
	t->inter = 0;
	t->private_mark = 0;
	t->nparams = 0;
	t->param_started = false;
	t->osc_len = 0;
	memset (t->param_colon, 0, sizeof t->param_colon);
}

static void
control (VTerm *t, unsigned char c)
{
	switch (c) {
	case '\a':
		break;
	case '\b':
		if (t->cx > 0)
			t->cx--;
		t->wrap_pending = false;
		break;
	case '\t':
		while (t->cx < t->cols - 1 && !t->tabs[++t->cx])
			;
		t->wrap_pending = false;
		break;
	case '\n':
	case '\v':
	case '\f':
		index_line (t);
		t->wrap_pending = false;
		break;
	case '\r':
		t->cx = 0;
		t->wrap_pending = false;
		break;
	case 016:		/* SO */
		t->shift_out = true;
		break;
	case 017:		/* SI */
		t->shift_out = false;
		break;
	default:
		break;
	}
}

static void
feed_byte (VTerm *t, unsigned char c)
{
	/* Strings run to BEL or ST, and swallow everything else. */
	switch (t->state) {
	case S_OSC:
		if (c == '\a')
			osc_dispatch (t), t->state = S_GROUND;
		else if (c == 033)
			t->state = S_OSC_ESC;
		else if (t->osc_len < MAX_OSC - 1)
			t->osc[t->osc_len++] = (char) c;
		return;
	case S_OSC_ESC:
		if (c == '\\') {
			osc_dispatch (t);
			t->state = S_GROUND;
		} else {
			start_sequence (t, S_ESCAPE);
			feed_byte (t, c);
		}
		return;
	case S_STRING:
		if (c == 033)
			t->state = S_STRING_ESC;
		else if (c == '\a')
			t->state = S_GROUND;
		return;
	case S_STRING_ESC:
		t->state = c == '\\' ? S_GROUND : S_STRING;
		return;
	default:
		break;
	}

	if (c == 033) {
		t->utf8_len = 0;
		start_sequence (t, S_ESCAPE);
		return;
	}
	if (c == 030 || c == 032) {	/* CAN, SUB: abort the sequence */
		t->state = S_GROUND;
		return;
	}
	if (c < 0x20 || c == 0x7f) {
		if (c != 0x7f)
			control (t, c);
		return;
	}

	switch (t->state) {
	case S_GROUND:
		if (c < 0x80) {
			t->utf8_len = 0;
			put_char (t, c);
		} else if (t->utf8_len == 0) {
			int need = utf8_seq_len (c);
			if (need <= 1)
				put_char (t, UTF8_REPLACEMENT);
			else {
				t->utf8[0] = c;
				t->utf8_len = 1;
				t->utf8_need = need;
			}
		} else if ((c & 0xC0) == 0x80) {
			t->utf8[t->utf8_len++] = c;
			if (t->utf8_len == t->utf8_need) {
				size_t len;
				uint32_t cp = utf8_decode ((const char *) t->utf8, (size_t) t->utf8_len, 0, &len);
				t->utf8_len = 0;
				put_char (t, cp);
			}
		} else {
			t->utf8_len = 0;
			put_char (t, UTF8_REPLACEMENT);
			feed_byte (t, c);
		}
		break;
	case S_ESCAPE:
		if (c >= 0x20 && c <= 0x2f) {
			t->inter = (char) c;
			t->state = S_ESCAPE_INTER;
		} else if (c == '[')
			start_sequence (t, S_CSI);
		else if (c == ']')
			start_sequence (t, S_OSC);
		else if (c == 'P' || c == 'X' || c == '^' || c == '_')
			t->state = S_STRING;
		else {
			esc_dispatch (t, (char) c);
			t->state = S_GROUND;
		}
		break;
	case S_ESCAPE_INTER:
		if (c >= 0x20 && c <= 0x2f)
			break;
		esc_dispatch (t, (char) c);
		t->state = S_GROUND;
		break;
	case S_CSI:
		if (c >= '0' && c <= '9') {
			if (t->nparams == 0)
				t->nparams = 1;
			if (t->nparams <= MAX_PARAMS) {
				int *p = &t->params[t->nparams - 1];
				if (!t->param_started)
					*p = 0;
				*p = *p > 100000 ? *p : *p * 10 + (c - '0');
			}
			t->param_started = true;
		} else if (c == ';' || c == ':') {
			if (t->nparams == 0) {
				t->nparams = 1;
				t->params[0] = 0;
			}
			if (c == ':' && t->nparams <= MAX_PARAMS)
				t->param_colon[t->nparams - 1] = true;
			if (t->nparams < MAX_PARAMS) {
				t->params[t->nparams++] = 0;
			}
			t->param_started = false;
		} else if (c >= '<' && c <= '?') {
			t->private_mark = (char) c;
		} else if (c >= 0x20 && c <= 0x2f) {
			t->inter = (char) c;
		} else if (c >= 0x40 && c <= 0x7e) {
			if (t->nparams > MAX_PARAMS)
				t->nparams = MAX_PARAMS;
			csi_dispatch (t, (char) c);
			t->state = S_GROUND;
		} else
			t->state = S_GROUND;
		break;
	default:
		t->state = S_GROUND;
		break;
	}
}

void
vterm_write (VTerm *t, const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++)
		feed_byte (t, (unsigned char) s[i]);
}
