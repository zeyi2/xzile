/* Shared definitions of the image and PDF modes

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

/* For the files of modes/ only: the editor sees the hooks in xzile.h.

   preview.c      preview buffers, their display, the mouse, and the
                  commands common to images and PDFs
   preview_view.c what a file is, and where a page goes in a window
   image.c        image-mode            image_probe.c  image headers
   pdf.c          pdf-mode and pages
   gui_preview.c  the frontend of xzile-gui (preview_backend), which
                  runs image_decode.c and pdf_render.c in workers
   html.c         html-mode, which has nothing to do with these */

#ifndef MODES_PREVIEW_H
#define MODES_PREVIEW_H

#include "../src/xzile.h"

enum PreviewKind { PREVIEW_NONE, PREVIEW_IMAGE, PREVIEW_PDF };

/* What a file is, from its first bytes. Text, such as SVG, is
   PREVIEW_NONE. */
enum PreviewKind preview_sniff (const unsigned char *head, size_t n);
enum PreviewKind preview_sniff_file (const char *path);


/* Image headers (image_probe.c). */

enum ImageFormat {
	IMAGE_UNKNOWN, IMAGE_PNG, IMAGE_JPEG, IMAGE_GIF, IMAGE_WEBP, IMAGE_TIFF, IMAGE_BMP, IMAGE_SVG,
};

/* The binary image format that `head' starts, if any. */
enum ImageFormat image_sniff (const unsigned char *head, size_t n);
/* The format of an image and the size its header declares, without
   decoding it: true if both are known. `*format' is set even when the
   size is not. Images are decoded only once this has said how large
   they are. */
bool image_probe (const unsigned char *data, size_t n, enum ImageFormat *format, long *w, long *h);
bool image_probe_fd (int fd, enum ImageFormat *format, long *w, long *h);
bool image_probe_file (const char *path, enum ImageFormat *format, long *w, long *h);
/* The format's name, for messages. */
const char *image_format_name (enum ImageFormat format);


/* Where a page goes in a window (preview_view.c). */

enum PreviewFit {
	PREVIEW_FIT_BEST,	/* The whole page, but never enlarged. */
	PREVIEW_FIT_PAGE,	/* The whole page. */
	PREVIEW_FIT_WIDTH,
	PREVIEW_FIT_HEIGHT,
	PREVIEW_FIT_NONE,	/* At `zoom'. */
};

#define PREVIEW_MIN_ZOOM 0.01
#define PREVIEW_MAX_ZOOM 32.0

/* How a page is shown. Sizes are in pixels: page sizes at 100%, and
   the area of the window showing it. */
typedef struct {
	enum PreviewFit fit;
	double zoom;		/* Shown pixels per page pixel, for PREVIEW_FIT_NONE. */
	double cx, cy;		/* The point at the centre, as fractions of the page. */
	long page;		/* From 0. */
} PreviewView;

/* The page's place relative to the area, and its scale. */
typedef struct {
	double x, y, w, h, scale;
} PreviewRect;

/* How a preview of `kind' is first shown: an image fitted to the
   window if it is larger, a PDF page fitted to its width. */
PreviewView preview_initial_view (enum PreviewKind kind);
PreviewRect preview_layout (const PreviewView *v, double aw, double ah, double pw, double ph);
/* Move the view by `dx', `dy', within the page; the distance moved is
   stored in `*moved_x' and `*moved_y'. */
void preview_pan (PreviewView *v, double dx, double dy, double aw, double ah, double pw, double ph,
		  double *moved_x, double *moved_y);
/* Zoom by `factor', keeping the point at `ax', `ay' of the area still. */
void preview_zoom (PreviewView *v, double factor, double ax, double ay,
		   double aw, double ah, double pw, double ph);


/* The frontend that draws previews, if it can (gui_preview.c). Called
   on the main thread only. */

typedef struct PreviewDoc PreviewDoc;

enum PreviewState { PREVIEW_LOADING, PREVIEW_READY, PREVIEW_FAILED };

typedef struct {
	enum PreviewState state;
	long pages;
	double width, height;	/* Of the page asked for, at 100%. */
	bool reduced;		/* Shown at less than full resolution. */
	const char *error;	/* For PREVIEW_FAILED. */
	/* Why the page asked for cannot be shown, if it cannot; it is not
	   tried again until the document is reloaded. */
	const char *page_error;
} PreviewInfo;

typedef struct {
	bool (*supports) (enum PreviewKind kind);
	PreviewDoc *(*open) (enum PreviewKind kind, const char *path);
	void (*close) (PreviewDoc *d);
	void (*reload) (PreviewDoc *d);
	void (*info) (PreviewDoc *d, long page, PreviewInfo *info);
	/* Show `d' as `view' says in `rows' by `cols' cells at `y', `x',
	   until the next clear. */
	void (*place) (PreviewDoc *d, const PreviewView *view, size_t y, size_t x, size_t rows, size_t cols);
	void (*clear) (void);
	/* The size of `rows' by `cols' cells, in pixels. */
	void (*cell_pixels) (size_t rows, size_t cols, double *w, double *h);
	/* How the document is drawn, for preview-info. */
	char *(*describe) (PreviewDoc *d);
} PreviewBackend;

extern const PreviewBackend *preview_backend;


/* For image.c and pdf.c (preview.c). */

/* A loaded preview in a window: its view, the window's area and the
   page's size in pixels. */
typedef struct {
	PreviewView *view;
	double aw, ah, pw, ph;
	long pages;
	bool reduced;
} PreviewGeometry;

/* Show the current buffer's file as a preview of `kind'. */
bool preview_enter (enum PreviewKind kind);
/* The current window's preview, if it has loaded; else says why not. */
bool preview_current (PreviewGeometry *g);
long preview_count_arg (long uniarg, Args *args);
/* A new keymap with the keys common to all previews. */
Binding *preview_keymap_new (void);


/* What differs between the kinds (image.c, pdf.c): the keymap; the
   status line text, given the fit (such as "Fit width ") and the scale
   in percent; and what the preview is, for preview-info. */

Binding *image_keymap (void);
char *image_status (const PreviewGeometry *g, const char *fit, int percent);
char *image_about (const PreviewInfo *info);

Binding *pdf_keymap (void);
char *pdf_status (const PreviewGeometry *g, const char *fit, int percent);
char *pdf_about (const PreviewInfo *info);
/* Go `n' pages on, at the top of the page, or the bottom if `from_end'.
   False if there is no such page. */
bool pdf_turn_page (const PreviewGeometry *g, long n, bool from_end);

#endif
