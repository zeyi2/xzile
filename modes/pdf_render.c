/* Rendering PDF pages for xzile-gui, with poppler and cairo

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

/* Opening a PDF reads only its page sizes; pages are rendered one at a
   time, at the size they are shown. */

#include "preview.h"

#include <SDL.h>
#ifdef HAVE_POPPLER
#include <cairo.h>
#include <poppler.h>
#endif

#include "gui_render.h"

#ifdef HAVE_POPPLER

struct PdfFile {
	PopplerDocument *doc;
};

bool
pdf_render_supported (void)
{
	return true;
}

char *
pdf_render_open (const char *path, PdfFile **pdf, long *npages, double **page_w, double **page_h)
{
	GError *err = NULL;
	char *uri = g_filename_to_uri (path, NULL, &err);
	PopplerDocument *pd = uri != NULL ? poppler_document_new_from_file (uri, NULL, &err) : NULL;
	g_free (uri);
	if (pd == NULL) {
		char *error;
		if (err != NULL && g_error_matches (err, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED))
			error = xstrdup ("the PDF is encrypted, and passwords are not supported");
		else
			error = xstrdup (err != NULL ? err->message : "cannot open the PDF");
		if (err != NULL)
			g_error_free (err);
		return error;
	}
	int n = poppler_document_get_n_pages (pd);
	if (n <= 0) {
		g_object_unref (pd);
		return xstrdup ("the PDF has no pages");
	}
	*npages = n;
	*page_w = xmalloc ((size_t) n * sizeof (double));
	*page_h = xmalloc ((size_t) n * sizeof (double));
	for (int i = 0; i < n; i++) {
		double w = 0, h = 0;
		PopplerPage *page = poppler_document_get_page (pd, i);
		if (page != NULL) {
			poppler_page_get_size (page, &w, &h);
			g_object_unref (page);
		}
		(*page_w)[i] = w > 0 ? w : 612;
		(*page_h)[i] = h > 0 ? h : 792;
	}
	*pdf = xmalloc (sizeof (PdfFile));
	(*pdf)->doc = pd;
	return NULL;
}

char *
pdf_render_page (PdfFile *pdf, long page_no, int w, int h, SDL_Surface **surface)
{
	*surface = NULL;
	PopplerPage *page = pdf != NULL ? poppler_document_get_page (pdf->doc, (int) page_no) : NULL;
	if (page == NULL)
		return xstrdup ("no such page");
	double pw, ph;
	poppler_page_get_size (page, &pw, &ph);
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat (0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	/* Cairo draws straight into the surface: its ARGB32 is SDL's
	   ARGB8888, and the page is opaque. */
	cairo_surface_t *cs = s != NULL ? cairo_image_surface_create_for_data (s->pixels, CAIRO_FORMAT_ARGB32,
									     s->w, s->h, s->pitch) : NULL;
	if (cs == NULL || cairo_surface_status (cs) != CAIRO_STATUS_SUCCESS || pw <= 0 || ph <= 0) {
		if (cs != NULL)
			cairo_surface_destroy (cs);
		SDL_FreeSurface (s);
		g_object_unref (page);
		return xstrdup ("cannot render the page");
	}
	cairo_t *cr = cairo_create (cs);
	cairo_set_source_rgb (cr, 1, 1, 1);
	cairo_paint (cr);
	cairo_scale (cr, s->w / pw, s->h / ph);
	poppler_page_render (page, cr);
	cairo_destroy (cr);
	cairo_surface_flush (cs);
	cairo_surface_destroy (cs);
	g_object_unref (page);
	*surface = s;
	return NULL;
}

void
pdf_render_close (PdfFile *pdf)
{
	if (pdf == NULL)
		return;
	g_object_unref (pdf->doc);
	free (pdf);
}

#else

bool
pdf_render_supported (void)
{
	return false;
}

char *
pdf_render_open (const char *path, PdfFile **pdf, long *npages, double **page_w, double **page_h)
{
	(void) path;
	(void) pdf;
	(void) npages;
	(void) page_w;
	(void) page_h;
	return xstrdup ("PDF support is not built in");
}

char *
pdf_render_page (PdfFile *pdf, long page, int w, int h, SDL_Surface **surface)
{
	(void) pdf;
	(void) page;
	(void) w;
	(void) h;
	*surface = NULL;
	return xstrdup ("PDF support is not built in");
}

void
pdf_render_close (PdfFile *pdf)
{
	(void) pdf;
}

#endif
