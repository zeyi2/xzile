/* Tests for recognising images and laying out previews (image_probe.c,
   preview_view.c).

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

#include "preview.h"

#include <unistd.h>

#define CHECK(cond) check ((cond), #cond, __LINE__)

static void
check (bool ok, const char *what, int line)
{
	if (!ok) {
		printf ("preview test failed at line %d: %s\n", line, what);
		exit (EXIT_FAILURE);
	}
}

static bool
near (double a, double b)
{
	return a - b < 1e-6 && b - a < 1e-6;
}

#define SNIFF(s) preview_sniff ((const unsigned char *) (s), sizeof (s) - 1)

static void
test_sniff (void)
{
	CHECK (SNIFF ("%PDF-1.7\n") == PREVIEW_PDF);
	CHECK (SNIFF ("\x89PNG\r\n\x1a\n\0\0\0\rIHDR") == PREVIEW_IMAGE);
	CHECK (SNIFF ("\xff\xd8\xff\xe0") == PREVIEW_IMAGE);
	CHECK (SNIFF ("GIF89a\x01\0\x01\0") == PREVIEW_IMAGE);
	CHECK (SNIFF ("GIF87a\x01\0\x01\0") == PREVIEW_IMAGE);
	CHECK (SNIFF ("RIFF\0\0\0\0WEBPVP8 ") == PREVIEW_IMAGE);
	CHECK (SNIFF ("II*\0\x08\0\0\0") == PREVIEW_IMAGE);
	CHECK (SNIFF ("MM\0*\0\0\0\x08") == PREVIEW_IMAGE);
	CHECK (SNIFF ("BM\x46\0\0\0\0\0\0\0\x36\0\0\0\x28\0\0\0") == PREVIEW_IMAGE);

	/* Text, however it starts. */
	CHECK (SNIFF ("BMW owners' manual, chapter one\n") == PREVIEW_NONE);
	CHECK (SNIFF ("%PDF") == PREVIEW_NONE);
	CHECK (SNIFF ("<svg xmlns=\"http://www.w3.org/2000/svg\"/>") == PREVIEW_NONE);
	CHECK (SNIFF ("<!DOCTYPE html>\n<html>") == PREVIEW_NONE);
	CHECK (SNIFF ("RIFF\0\0\0\0WAVEfmt ") == PREVIEW_NONE);
	CHECK (SNIFF ("GIF8") == PREVIEW_NONE);
	CHECK (preview_sniff (NULL, 0) == PREVIEW_NONE);
}

static enum ImageFormat format;
static long w, h;

#define PROBE(d, n) image_probe ((const unsigned char *) (d), (n), &format, &w, &h)
#define PROBE_S(s) PROBE (s, sizeof (s) - 1)

static void
test_sizes (void)
{
	static const unsigned char png[] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\x07\x80\0\0\x04\x38\x08\x06\0\0\0";
	CHECK (PROBE (png, sizeof png - 1) && format == IMAGE_PNG && w == 1920 && h == 1080);
	static const unsigned char empty_png[] = "\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\0\0\0\0\0";
	CHECK (!PROBE (empty_png, sizeof empty_png - 1) && format == IMAGE_PNG);

	CHECK (PROBE_S ("GIF89a\x10\0\x20\0\0\0\0,\0\0\0\0\x10\0\x20\0\0") && format == IMAGE_GIF && w == 16 && h == 32);
	/* A first frame larger than the screen, after an extension: the
	   decoder makes a surface of the frame's size. */
	CHECK (PROBE_S ("GIF89a\x10\0\x10\0\0\0\0"
			"\x21\xf9\x04\0\0\0\0\0"
			",\0\0\0\0\xe8\x03\xd0\x07\0") && w == 1000 && h == 2000);
	/* No frame: the size is not known. */
	CHECK (!PROBE_S ("GIF89a\x10\0\x20\0\0\0\0") && format == IMAGE_GIF);

	/* Stored top-down, with a negative height. */
	unsigned char bmp[26] = "BM";
	bmp[14] = 40;
	bmp[18] = 100;
	bmp[22] = 0x9c, bmp[23] = 0xff, bmp[24] = 0xff, bmp[25] = 0xff;
	CHECK (PROBE (bmp, sizeof bmp) && format == IMAGE_BMP && w == 100 && h == 100);

	/* A JPEG with an APP0 segment before the frame. */
	static const unsigned char jpeg[] =
		"\xff\xd8"
		"\xff\xe0\0\x10JFIF\0\x01\x01\0\0\x01\0\x01\0\0"
		"\xff\xc0\0\x11\x08\x02\x58\x03\x20\x03\x01\x22\0\x02\x11\x01\x03\x11\x01";
	CHECK (PROBE (jpeg, sizeof jpeg - 1) && format == IMAGE_JPEG && w == 800 && h == 600);
	/* Cut off before the frame. */
	CHECK (!PROBE (jpeg, 20) && format == IMAGE_JPEG);

	unsigned char vp8x[30] = "RIFF\0\0\0\0WEBPVP8X";
	vp8x[24] = 0xff, vp8x[25] = 0x03;	/* 1024 - 1 */
	vp8x[27] = 0xff, vp8x[28] = 0x02;	/* 768 - 1 */
	CHECK (PROBE (vp8x, sizeof vp8x) && format == IMAGE_WEBP && w == 1024 && h == 768);

	unsigned char vp8l[30] = "RIFF\0\0\0\0WEBPVP8L";
	unsigned long bits = (640 - 1) | (unsigned long) (480 - 1) << 14;
	vp8l[20] = 0x2f;
	vp8l[21] = bits & 0xff, vp8l[22] = (bits >> 8) & 0xff, vp8l[23] = (bits >> 16) & 0xff, vp8l[24] = (bits >> 24) & 0xff;
	CHECK (PROBE (vp8l, sizeof vp8l) && w == 640 && h == 480);

	CHECK (!PROBE_S ("hello") && format == IMAGE_UNKNOWN);
	CHECK (!PROBE (NULL, 0) && format == IMAGE_UNKNOWN);
}

/* A little-endian TIFF: ImageWidth a SHORT, ImageLength a LONG. */
static const unsigned char tiff_le[] =
	"II*\0\x08\0\0\0"
	"\x02\0"
	"\0\x01\x03\0\x01\0\0\0\x80\x02\0\0"
	"\x01\x01\x04\0\x01\0\0\0\xe0\x01\0\0"
	"\0\0\0\0";

static void
test_tiff (void)
{
	CHECK (PROBE (tiff_le, sizeof tiff_le - 1) && format == IMAGE_TIFF && w == 640 && h == 480);

	/* Big-endian, with the directory after the image data. */
	unsigned char be[0x80 + 42] = "MM\0*\0\0\0\x80";
	static const unsigned char ifd[] =
		"\0\x03"
		"\x01\x03\0\x03\0\0\0\x01\0\x01\0\0"	/* Compression, ignored. */
		"\x01\0\0\x04\0\0\0\x01\0\x01\x86\xa0"	/* ImageWidth 100000 */
		"\x01\x01\0\x03\0\0\0\x01\x75\x30\0\0"	/* ImageLength 30000 */
		"\0\0\0\0";
	memcpy (be + 0x80, ifd, sizeof ifd - 1);
	CHECK (PROBE (be, sizeof be) && format == IMAGE_TIFF && w == 100000 && h == 30000);
	/* The directory cut off. */
	CHECK (!PROBE (be, 0x80 + 20) && format == IMAGE_TIFF);

	/* No ImageLength. */
	unsigned char no_h[sizeof tiff_le];
	memcpy (no_h, tiff_le, sizeof no_h);
	no_h[8] = 1;
	CHECK (!PROBE (no_h, sizeof no_h - 1) && format == IMAGE_TIFF);

	/* A directory outside the file. */
	unsigned char lost[sizeof tiff_le];
	memcpy (lost, tiff_le, sizeof lost);
	lost[7] = 0x7f;
	CHECK (!PROBE (lost, sizeof lost - 1) && format == IMAGE_TIFF);

	/* BigTIFF is not read, so not taken for an image at all. */
	CHECK (!PROBE_S ("II+\0\x08\0\0\0\x10\0\0\0\0\0\0\0") && format == IMAGE_UNKNOWN);
}

static void
test_svg (void)
{
	CHECK (PROBE_S ("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">")
	       && format == IMAGE_SVG && w == 200 && h == 100);
	CHECK (PROBE_S ("<?xml version=\"1.0\"?>\n<svg\n viewBox=\"0 0 300 150\">") && w == 300 && h == 150);
	/* Units, and a side from the viewBox's proportions. */
	CHECK (PROBE_S ("<svg width='2in' viewBox='0,0,10,5'>") && w == 192 && h == 96);
	CHECK (PROBE_S ("<svg width=\"4cm\" height=\"2cm\">") && w == 151 && h == 76);
	/* A size that depends on where it is shown. */
	CHECK (!PROBE_S ("<svg width=\"100%\" height=\"100%\">") && format == IMAGE_SVG);
	CHECK (!PROBE_S ("<svg xmlns=\"http://www.w3.org/2000/svg\">") && format == IMAGE_SVG);
	CHECK (!PROBE_S ("<svgfoo width=\"1\" height=\"1\">") && format == IMAGE_UNKNOWN);
	CHECK (!PROBE_S ("<html><body>no image</body></html>") && format == IMAGE_UNKNOWN);
}

static void
test_file (void)
{
	char path[] = "/tmp/xzile-preview-test-XXXXXX";
	int fd = mkstemp (path);
	CHECK (fd >= 0);
	CHECK (write (fd, tiff_le, sizeof tiff_le - 1) == (ssize_t) (sizeof tiff_le - 1));
	close (fd);
	CHECK (preview_sniff_file (path) == PREVIEW_IMAGE);
	CHECK (image_probe_file (path, &format, &w, &h) && format == IMAGE_TIFF && w == 640 && h == 480);
	unlink (path);
	CHECK (preview_sniff_file (path) == PREVIEW_NONE);
	CHECK (!image_probe_file (path, &format, &w, &h) && format == IMAGE_UNKNOWN);
}

static void
test_layout (void)
{
	/* A small image is centred and not enlarged by default. */
	PreviewView v = preview_initial_view (PREVIEW_IMAGE);
	PreviewRect r = preview_layout (&v, 800, 600, 200, 100);
	CHECK (near (r.scale, 1) && near (r.x, 300) && near (r.y, 250) && near (r.w, 200));

	/* A large one is shrunk to fit. */
	r = preview_layout (&v, 800, 600, 1600, 600);
	CHECK (near (r.scale, 0.5) && near (r.x, 0) && near (r.y, 150) && near (r.h, 300));

	v.fit = PREVIEW_FIT_PAGE;
	r = preview_layout (&v, 800, 600, 200, 100);
	CHECK (near (r.scale, 4) && near (r.w, 800) && near (r.y, 100));

	v.fit = PREVIEW_FIT_HEIGHT;
	r = preview_layout (&v, 800, 600, 200, 100);
	CHECK (near (r.scale, 6) && near (r.h, 600));

	/* A PDF page starts fitted to the width, at its top. */
	v = preview_initial_view (PREVIEW_PDF);
	r = preview_layout (&v, 400, 300, 100, 200);
	CHECK (near (r.scale, 4) && near (r.x, 0) && near (r.y, 0) && near (r.h, 800));

	/* Panning stops at the edges, and says how far it went. */
	double mx, my;
	preview_pan (&v, 0, 100, 400, 300, 100, 200, &mx, &my);
	CHECK (near (mx, 0) && near (my, 100));
	r = preview_layout (&v, 400, 300, 100, 200);
	CHECK (near (r.y, -100));
	preview_pan (&v, 0, 10000, 400, 300, 100, 200, &mx, &my);
	CHECK (near (my, 400));
	r = preview_layout (&v, 400, 300, 100, 200);
	CHECK (near (r.y, 300 - 800));
	preview_pan (&v, 0, 1, 400, 300, 100, 200, &mx, &my);
	CHECK (near (my, 0));
	/* A page that fits does not move. */
	PreviewView small = { PREVIEW_FIT_NONE, 1, 0.5, 0.5, 0 };
	preview_pan (&small, 50, 50, 400, 300, 100, 100, &mx, &my);
	CHECK (near (mx, 0) && near (my, 0) && near (small.cx, 0.5));
}

static void
test_zoom (void)
{
	/* The point under the anchor stays there. */
	PreviewView v = { PREVIEW_FIT_PAGE, 1, 0.5, 0.5, 0 };
	PreviewRect before = preview_layout (&v, 400, 400, 400, 400);
	double ax = 100, ay = 300;
	double fx = (ax - before.x) / before.w, fy = (ay - before.y) / before.h;
	preview_zoom (&v, 2, ax, ay, 400, 400, 400, 400);
	CHECK (v.fit == PREVIEW_FIT_NONE && near (v.zoom, 2));
	PreviewRect after = preview_layout (&v, 400, 400, 400, 400);
	CHECK (near (after.x + fx * after.w, ax) && near (after.y + fy * after.h, ay));

	/* Zooming out again recentres a page that fits. */
	preview_zoom (&v, 0.25, ax, ay, 400, 400, 400, 400);
	after = preview_layout (&v, 400, 400, 400, 400);
	CHECK (near (after.scale, 0.5) && near (after.x, 100) && near (after.y, 100));

	/* Within limits. */
	preview_zoom (&v, 1e9, 0, 0, 400, 400, 400, 400);
	CHECK (near (v.zoom, PREVIEW_MAX_ZOOM));
	preview_zoom (&v, 1e-9, 0, 0, 400, 400, 400, 400);
	CHECK (near (v.zoom, PREVIEW_MIN_ZOOM));

	/* Nothing breaks for an empty page. */
	PreviewView e = preview_initial_view (PREVIEW_IMAGE);
	PreviewRect r = preview_layout (&e, 400, 400, 0, 0);
	CHECK (near (r.w, 0) && near (r.h, 0));
}

int
main (void)
{
	test_sniff ();
	test_sizes ();
	test_tiff ();
	test_svg ();
	test_file ();
	test_layout ();
	test_zoom ();
	printf ("preview tests passed\n");
	return EXIT_SUCCESS;
}
