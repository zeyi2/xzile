/* File types and page layout for previews

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

/* Only the C library here, so that the terminal build and the tests
   (preview-test.c) can use it. */

#include "preview.h"

#include <fcntl.h>
#include <unistd.h>

enum PreviewKind
preview_sniff (const unsigned char *h, size_t n)
{
	if (n >= 5 && memcmp (h, "%PDF-", 5) == 0)
		return PREVIEW_PDF;
	return image_sniff (h, n) != IMAGE_UNKNOWN ? PREVIEW_IMAGE : PREVIEW_NONE;
}

enum PreviewKind
preview_sniff_file (const char *path)
{
	unsigned char head[32];
	int fd = open (path, O_RDONLY);
	if (fd < 0)
		return PREVIEW_NONE;
	size_t n = 0;
	while (n < sizeof head) {
		ssize_t r = read (fd, head + n, sizeof head - n);
		if (r <= 0)
			break;
		n += (size_t) r;
	}
	close (fd);
	return preview_sniff (head, n);
}


/* Layout. */

PreviewView
preview_initial_view (enum PreviewKind kind)
{
	PreviewView v = { PREVIEW_FIT_BEST, 1.0, 0.5, 0.5, 0 };
	if (kind == PREVIEW_PDF) {
		v.fit = PREVIEW_FIT_WIDTH;
		v.cy = 0;
	}
	return v;
}

static double
view_scale (const PreviewView *v, double aw, double ah, double pw, double ph)
{
	if (pw <= 0 || ph <= 0)
		return 1;
	double fw = aw / pw, fh = ah / ph, s;
	switch (v->fit) {
	case PREVIEW_FIT_BEST:
		s = MIN (1.0, MIN (fw, fh));
		break;
	case PREVIEW_FIT_PAGE:
		s = MIN (fw, fh);
		break;
	case PREVIEW_FIT_WIDTH:
		s = fw;
		break;
	case PREVIEW_FIT_HEIGHT:
		s = fh;
		break;
	default:
		s = v->zoom;
		break;
	}
	return s > 1e-6 ? s : 1e-6;
}

/* Where a page `size' long starts in an area `area' long: centred if
   it fits, otherwise with `centre' in the middle but no gap at either
   end. */
static double
place_axis (double area, double size, double centre)
{
	if (size <= area)
		return (area - size) / 2;
	double x = area / 2 - centre * size;
	return MAX (area - size, MIN (0.0, x));
}

/* The centre that puts the page at `x'. */
static double
centre_of (double area, double size, double x)
{
	return size > area ? (area / 2 - x) / size : 0.5;
}

PreviewRect
preview_layout (const PreviewView *v, double aw, double ah, double pw, double ph)
{
	PreviewRect r;
	r.scale = view_scale (v, aw, ah, pw, ph);
	r.w = pw * r.scale;
	r.h = ph * r.scale;
	r.x = place_axis (aw, r.w, v->cx);
	r.y = place_axis (ah, r.h, v->cy);
	return r;
}

void
preview_pan (PreviewView *v, double dx, double dy, double aw, double ah, double pw, double ph,
	     double *moved_x, double *moved_y)
{
	PreviewRect r = preview_layout (v, aw, ah, pw, ph);
	double x = place_axis (aw, r.w, centre_of (aw, r.w, r.x - dx));
	double y = place_axis (ah, r.h, centre_of (ah, r.h, r.y - dy));
	v->cx = centre_of (aw, r.w, x);
	v->cy = centre_of (ah, r.h, y);
	if (moved_x != NULL)
		*moved_x = r.x - x;
	if (moved_y != NULL)
		*moved_y = r.y - y;
}

void
preview_zoom (PreviewView *v, double factor, double ax, double ay,
	      double aw, double ah, double pw, double ph)
{
	PreviewRect r = preview_layout (v, aw, ah, pw, ph);
	double s = MAX (PREVIEW_MIN_ZOOM, MIN (PREVIEW_MAX_ZOOM, r.scale * factor));
	/* The point of the page under the anchor stays there. */
	double fx = r.w > 0 ? (ax - r.x) / r.w : 0.5, fy = r.h > 0 ? (ay - r.y) / r.h : 0.5;
	v->fit = PREVIEW_FIT_NONE;
	v->zoom = s;
	v->cx = (aw / 2 - ax) / (pw * s) + fx;
	v->cy = (ah / 2 - ay) / (ph * s) + fy;
	preview_pan (v, 0, 0, aw, ah, pw, ph, NULL, NULL);
}
