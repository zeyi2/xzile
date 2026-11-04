/* The screen model shared by the terminal and graphical frontends

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

/* The term_* drawing functions write into this grid of cells; each
   frontend shows it on term_refresh. */

enum {
	ATTR_BOLD = 1,
	ATTR_UNDERLINE = 2,
	ATTR_REVERSE = 4,
	ATTR_DIM = 8,
	ATTR_ITALIC = 16,
	ATTR_STRIKE = 32,
	ATTR_PICTURE = 64,	/* Covered by a preview, in xzile-gui. */
};

typedef struct {
	int fg, bg;		/* Colour index, VCOLOR_RGB value, or TERM_COLOR_DEFAULT. */
	unsigned flags;
} Attr;

typedef struct {
	char ch[16];		/* UTF-8 bytes; empty for the right half of a wide char. */
	Attr attr;
} Cell;

extern size_t scr_rows, scr_cols;
extern Cell *scr_cells;		/* What we want displayed. */
extern size_t scr_y, scr_x;	/* The cursor. */
extern bool scr_cursor_visible;

void screen_blank_cell (Cell *c);
/* Reallocate the grid, blank, for `rows' by `cols' cells. */
void screen_resize (size_t rows, size_t cols);
