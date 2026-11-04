/* Showing images and PDFs in xzile-gui (gui_preview.c), for gui.c

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

/* Sets preview_backend. */
void gui_preview_init (SDL_Renderer *renderer);
void gui_preview_shutdown (void);
/* The text grid: where cell 0, 0 is, and the size of a cell, in pixels. */
void gui_preview_set_metrics (int pad, int cell_w, int cell_h, float scale);
/* Draw the previews placed since the last clear, over the cells of
   the grid marked ATTR_PICTURE. */
void gui_preview_draw (const Cell *cells, size_t rows, size_t cols);
/* The event a worker sends when it has finished something. */
Uint32 gui_preview_event (void);
/* Take finished work; returns true if a redisplay is needed. */
bool gui_preview_poll (void);
/* Forget all textures, after the renderer lost them. */
void gui_preview_reset (void);
