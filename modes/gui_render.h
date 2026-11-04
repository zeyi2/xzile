/* Decoding images and rendering PDF pages, for gui_preview.c

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

/* image_decode.c uses SDL2_image if built with HAVE_SDL_IMAGE, and
   pdf_render.c poppler and cairo if built with HAVE_POPPLER; without,
   each supports nothing. Their work runs in worker threads: it touches
   neither the renderer nor the editor. Functions that fail return why,
   to be freed; on success, NULL. Surfaces are ARGB8888. */

#ifndef MODES_GUI_RENDER_H
#define MODES_GUI_RENDER_H

#include <SDL.h>

#define IMAGE_MAX_LEVELS 12

/* An image, at full or reduced resolution, and its levels of detail,
   each half the size of the last. */
typedef struct {
	SDL_Surface *levels[IMAGE_MAX_LEVELS];
	int nlevels;
	long width, height;	/* Of the image itself. */
	bool reduced;		/* levels[0] is smaller than the image. */
	bool alpha;		/* Some pixels are not opaque. */
} DecodedImage;

bool image_decode_supported (void);
/* On the main thread, before and after any decoding. */
void image_decode_init (void);
void image_decode_quit (void);
/* Decode the image at `path', reduced if need be to at most `max_w'
   by `max_h' (0 for no limit). Nothing is decoded unless the size in
   its header can be read, and is within bounds. */
char *image_decode (const char *path, int max_w, int max_h, DecodedImage *img);
void image_decoded_free (DecodedImage *img);

/* An open PDF. Poppler is not safe to use from two threads at once:
   these functions must not be. */
typedef struct PdfFile PdfFile;

bool pdf_render_supported (void);
/* Open the PDF at `path', and read the sizes of its pages in points. */
char *pdf_render_open (const char *path, PdfFile **pdf, long *npages, double **page_w, double **page_h);
/* Render page `page', from 0, `w' by `h' pixels. */
char *pdf_render_page (PdfFile *pdf, long page, int w, int h, SDL_Surface **surface);
void pdf_render_close (PdfFile *pdf);

#endif
