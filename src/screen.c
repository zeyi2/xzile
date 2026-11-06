/* Screen model shared by the terminal and graphical frontends

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

#include <wchar.h>

size_t scr_rows = 24, scr_cols = 80;
Cell *scr_cells = NULL;
size_t scr_y = 0, scr_x = 0;
bool scr_cursor_visible = true;
static Attr cur_attr = { TERM_COLOR_DEFAULT, TERM_COLOR_DEFAULT, 0 };

void
screen_blank_cell (Cell *c)
{
	c->ch[0] = ' ';
	c->ch[1] = '\0';
	c->attr.fg = c->attr.bg = TERM_COLOR_DEFAULT;
	c->attr.flags = 0;
}

void
screen_resize (size_t rows, size_t cols)
{
	free (scr_cells);
	scr_rows = rows;
	scr_cols = cols;
	scr_cells = xmalloc (rows * cols * sizeof (Cell));
	for (size_t i = 0; i < rows * cols; i++)
		screen_blank_cell (&scr_cells[i]);
	scr_y = MIN (scr_y, rows - 1);
	scr_x = MIN (scr_x, cols - 1);
}

size_t
term_width (void)
{
	return scr_cols;
}

size_t
term_height (void)
{
	return scr_rows;
}

/*--------------------------------------------------------------------------
 * Drawing into the virtual screen.
 *--------------------------------------------------------------------------*/

void
term_move (size_t y, size_t x)
{
	scr_y = MIN (y, scr_rows - 1);
	scr_x = MIN (x, scr_cols - 1);
}

static void
put_cell (const char *bytes, size_t n, size_t width)
{
	if (width == 2 && scr_x + 1 >= scr_cols) {
		/* A wide character does not fit: pad the line instead. */
		put_cell (" ", 1, 1);
		return;
	}

	Cell *c = &scr_cells[scr_y * scr_cols + scr_x];
	n = MIN (n, sizeof c->ch - 1);
	memcpy (c->ch, bytes, n);
	c->ch[n] = '\0';
	c->attr = cur_attr;
	if (width == 2) {
		Cell *c2 = c + 1;
		c2->ch[0] = '\0';
		c2->attr = cur_attr;
	}

	scr_x += width;
	if (scr_x >= scr_cols) {
		if (scr_y + 1 < scr_rows) {
			scr_x = 0;
			scr_y++;
		} else
			scr_x = scr_cols - 1;
	}
}

/* Append a zero-width character to the previous cell. */
static void
put_combining (const char *bytes, size_t n)
{
	if (scr_x == 0)
		return;
	Cell *c = &scr_cells[scr_y * scr_cols + scr_x - 1];
	if (c->ch[0] == '\0' && scr_x >= 2)
		c--;
	size_t len = strlen (c->ch);
	if (len + n < sizeof c->ch) {
		memcpy (c->ch + len, bytes, n);
		c->ch[len + n] = '\0';
	}
}

void
term_clrtoeol (void)
{
	Attr saved = cur_attr;
	cur_attr.fg = cur_attr.bg = TERM_COLOR_DEFAULT;
	cur_attr.flags = 0;
	for (size_t x = scr_x; x < scr_cols; x++) {
		Cell *c = &scr_cells[scr_y * scr_cols + x];
		c->ch[0] = ' ';
		c->ch[1] = '\0';
		c->attr = cur_attr;
	}
	cur_attr = saved;
}

void
term_addnstr (const char *s, size_t len)
{
	for (size_t i = 0; i < len;) {
		unsigned char b = (unsigned char) s[i];
		if (b == '\n') {
			term_clrtoeol ();
			if (scr_y + 1 < scr_rows) {
				scr_y++;
				scr_x = 0;
			}
			i++;
		} else if (b == '\t') {
			do
				put_cell (" ", 1, 1);
			while (scr_x % 8 != 0 && scr_x != 0);
			i++;
		} else if (b < 0x20 || b == 0x7f) {
			char buf[2] = { '^', (char) (b == 0x7f ? '?' : b + '@') };
			put_cell (buf, 1, 1);
			put_cell (buf + 1, 1, 1);
			i++;
		} else if (b < 0x80) {
			put_cell ((const char *) &s[i], 1, 1);
			i++;
		} else {
			size_t char_len;
			uint32_t cp = utf8_decode (s, len, i, &char_len);
			if (cp == UTF8_REPLACEMENT && char_len == 1) {
				put_cell ("?", 1, 1);
			} else if (cp < 0xA0) {
				char buf[8];
				int n = snprintf (buf, sizeof buf, "\\%o", (unsigned) cp);
				for (int k = 0; k < n; k++)
					put_cell (buf + k, 1, 1);
			} else {
				int w = wcwidth ((wchar_t) cp);
				if (w < 0)
					w = 1;
				if (w == 0)
					put_combining (s + i, char_len);
				else
					put_cell (s + i, char_len, (size_t) (w > 1 ? 2 : 1));
			}
			i += char_len;
		}
	}
}

void
term_addstr (const char *s)
{
	term_addnstr (s, strlen (s));
}

void
term_addch (char c)
{
	term_addnstr (&c, 1);
}

void
term_add_picture (size_t n)
{
	Attr saved = cur_attr;
	cur_attr.fg = cur_attr.bg = TERM_COLOR_DEFAULT;
	cur_attr.flags = ATTR_PICTURE;
	for (size_t i = 0; i < n; i++)
		put_cell (" ", 1, 1);
	cur_attr = saved;
}

void
term_attrset (size_t attr)
{
	cur_attr.fg = cur_attr.bg = TERM_COLOR_DEFAULT;
	cur_attr.flags = 0;
	if (attr & FONT_REVERSE)
		cur_attr.flags |= ATTR_REVERSE;
	if (attr & FONT_UNDERLINE)
		cur_attr.flags |= ATTR_UNDERLINE;
}

void
term_apply_style (const TerminalStyle *style)
{
	cur_attr.fg = cur_attr.bg = TERM_COLOR_DEFAULT;
	cur_attr.flags = 0;

	if (style->bold)
		cur_attr.flags |= ATTR_BOLD;
	if (style->underline)
		cur_attr.flags |= ATTR_UNDERLINE;
	if (style->reverse)
		cur_attr.flags |= ATTR_REVERSE;
	if (style->italic)
		cur_attr.flags |= ATTR_ITALIC;
	if (style->strike)
		cur_attr.flags |= ATTR_STRIKE;

	if (!term_get_capabilities ()->has_colors || (!style->has_foreground && !style->has_background))
		return;

	int fg = style->has_foreground ? style->foreground : TERM_COLOR_DEFAULT;
	int bg = style->has_background ? style->background : TERM_COLOR_DEFAULT;
	if (!term_get_capabilities ()->supports_default_colors) {
		/* Use the concrete fallbacks precomputed by the resolver. */
		if (fg == TERM_COLOR_DEFAULT)
			fg = style->default_foreground_fallback;
		if (bg == TERM_COLOR_DEFAULT)
			bg = style->default_background_fallback;
	}
	cur_attr.fg = fg < 0 ? TERM_COLOR_DEFAULT : fg;
	cur_attr.bg = bg < 0 ? TERM_COLOR_DEFAULT : bg;
}

void
term_apply_face (const char *face_name)
{
	TerminalStyle style;
	if (resolve_terminal_style (face_name, &style))
		term_apply_style (&style);
	else
		term_attrset (FONT_NORMAL);
}

void
term_mark_link (bool plain)
{
	TerminalStyle style;
	if (!resolve_terminal_style (FACE_LINK, &style))
		return;
	if (style.underline)
		cur_attr.flags |= ATTR_UNDERLINE;
	if (style.bold)
		cur_attr.flags |= ATTR_BOLD;
	if (plain && style.has_foreground)
		cur_attr.fg = style.foreground;
}

void
term_set_vattr (const VAttr *a)
{
	cur_attr.fg = a->fg;
	cur_attr.bg = a->bg;
	cur_attr.flags = 0;
	if (a->flags & VATTR_BOLD)
		cur_attr.flags |= ATTR_BOLD;
	if (a->flags & VATTR_DIM)
		cur_attr.flags |= ATTR_DIM;
	if (a->flags & VATTR_ITALIC)
		cur_attr.flags |= ATTR_ITALIC;
	if (a->flags & VATTR_UNDERLINE)
		cur_attr.flags |= ATTR_UNDERLINE;
	if (a->flags & VATTR_REVERSE)
		cur_attr.flags |= ATTR_REVERSE;
	if (a->flags & VATTR_STRIKE)
		cur_attr.flags |= ATTR_STRIKE;
}

void
term_set_cursor_visible (bool visible)
{
	scr_cursor_visible = visible;
}

void
term_get_cursor (size_t *y, size_t *x)
{
	*y = scr_y;
	*x = scr_x;
}
