/* Image formats and sizes, from their headers

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
   (preview-test.c) can use it.

   An image is decoded only if its size can be read first, so that a
   small file cannot make the decoder allocate gigabytes. The size is
   the largest the decoder will produce: for a GIF, the larger of the
   screen and the first frame; for a TIFF, its first directory, which
   is the one decoded. The headers are read where they are, not just at
   the start of the file: a TIFF's directory and a JPEG's frame header
   may be anywhere. */

#include "preview.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

/* Where an image is read from: a buffer, or a file. */
typedef struct {
	const unsigned char *data;
	size_t n;
	int fd;
} Src;

/* Read `len' bytes at `off' into `buf'; false if they are not all there. */
static bool
src_read (const Src *s, uint64_t off, void *buf, size_t len)
{
	if (s->data != NULL) {
		if (off > s->n || len > s->n - off)
			return false;
		memcpy (buf, s->data + off, len);
		return true;
	}
	for (size_t got = 0; got < len;) {
		ssize_t r = pread (s->fd, (char *) buf + got, len - got, (off_t) (off + got));
		if (r <= 0)
			return false;
		got += (size_t) r;
	}
	return true;
}

static unsigned long
be16 (const unsigned char *p)
{
	return (unsigned long) p[0] << 8 | p[1];
}

static unsigned long
le16 (const unsigned char *p)
{
	return (unsigned long) p[1] << 8 | p[0];
}

static unsigned long
be32 (const unsigned char *p)
{
	return be16 (p) << 16 | be16 (p + 2);
}

static unsigned long
le32 (const unsigned char *p)
{
	return le16 (p + 2) << 16 | le16 (p);
}

enum ImageFormat
image_sniff (const unsigned char *h, size_t n)
{
	if (n >= 8 && memcmp (h, "\x89PNG\r\n\x1a\n", 8) == 0)
		return IMAGE_PNG;
	if (n >= 3 && h[0] == 0xff && h[1] == 0xd8 && h[2] == 0xff)
		return IMAGE_JPEG;
	if (n >= 6 && (memcmp (h, "GIF87a", 6) == 0 || memcmp (h, "GIF89a", 6) == 0))
		return IMAGE_GIF;
	if (n >= 12 && memcmp (h, "RIFF", 4) == 0 && memcmp (h + 8, "WEBP", 4) == 0)
		return IMAGE_WEBP;
	if (n >= 4 && (memcmp (h, "II*\0", 4) == 0 || memcmp (h, "MM\0*", 4) == 0))
		return IMAGE_TIFF;
	/* "BM" alone starts too much text: check the reserved bytes and the
	   size of the header that follows. */
	if (n >= 18 && h[0] == 'B' && h[1] == 'M' && memcmp (h + 6, "\0\0\0\0", 4) == 0) {
		unsigned long dib = le32 (h + 14);
		if (dib == 12 || dib == 40 || dib == 52 || dib == 56 || dib == 64 || dib == 108 || dib == 124)
			return IMAGE_BMP;
	}
	return IMAGE_UNKNOWN;
}

static bool
jpeg_size (const Src *s, long *w, long *h)
{
	unsigned char b[9];
	uint64_t i = 2;
	/* Segments up to the frame header: a few, unless the file is bad. */
	for (int segments = 0; segments < 10000; segments++) {
		if (!src_read (s, i, b, 2) || b[0] != 0xff)
			return false;
		unsigned m = b[1];
		if (m == 0xff) {	/* Fill byte. */
			i++;
			continue;
		}
		if (m == 0x01 || (m >= 0xd0 && m <= 0xd8)) {	/* No length. */
			i += 2;
			continue;
		}
		if (m == 0xd9 || m == 0xda)	/* The end, or the data, before a frame. */
			return false;
		if (!src_read (s, i, b, 9))
			return false;
		/* Start of frame, except DHT, JPG and DAC. */
		if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
			*h = (long) be16 (b + 5);
			*w = (long) be16 (b + 7);
			return true;
		}
		i += 2 + be16 (b + 2);
	}
	return false;
}

/* The larger of the screen and the first frame. */
static bool
gif_size (const Src *s, long *w, long *h)
{
	unsigned char b[10];
	if (!src_read (s, 6, b, 7))
		return false;
	*w = (long) le16 (b);
	*h = (long) le16 (b + 2);
	uint64_t i = 13;
	if (b[4] & 0x80)	/* The global colour table. */
		i += 3u << ((b[4] & 7) + 1);
	for (int blocks = 0; blocks < 10000; blocks++) {
		if (!src_read (s, i, b, 1))
			return false;
		if (b[0] == 0x2c) {	/* An image. */
			if (!src_read (s, i + 1, b, 8))
				return false;
			*w = MAX (*w, (long) le16 (b + 4));
			*h = MAX (*h, (long) le16 (b + 6));
			return true;
		}
		if (b[0] != 0x21)	/* Not an extension: no image. */
			return false;
		/* An extension's label, then blocks up to an empty one. */
		i += 2;
		for (;;) {
			if (!src_read (s, i, b, 1))
				return false;
			i += 1 + b[0];
			if (b[0] == 0)
				break;
		}
	}
	return false;
}

/* The first directory's ImageWidth and ImageLength. */
static bool
tiff_size (const Src *s, long *w, long *h)
{
	unsigned char b[12];
	if (!src_read (s, 0, b, 8))
		return false;
	bool le = b[0] == 'I';
	unsigned long (*u16) (const unsigned char *) = le ? le16 : be16;
	unsigned long (*u32) (const unsigned char *) = le ? le32 : be32;
	/* 43 is BigTIFF, which this does not read. */
	if (u16 (b + 2) != 42)
		return false;
	uint64_t ifd = u32 (b + 4);
	if (!src_read (s, ifd, b, 2))
		return false;
	unsigned long count = u16 (b);
	*w = *h = 0;
	for (unsigned long i = 0; i < count; i++) {
		if (!src_read (s, ifd + 2 + 12 * i, b, 12))
			return false;
		unsigned long tag = u16 (b), type = u16 (b + 2), n = u32 (b + 4);
		if ((tag != 256 && tag != 257) || n != 1)
			continue;
		long v;
		if (type == 3)		/* SHORT */
			v = (long) u16 (b + 8);
		else if (type == 4)	/* LONG */
			v = (long) u32 (b + 8);
		else
			return false;
		*(tag == 256 ? w : h) = v;
	}
	return *w > 0 && *h > 0;
}

/* An SVG length in pixels, at 96 to the inch, as SDL_image reads it. */
static bool
svg_length (const char *s, double *px)
{
	char *end;
	double v = strtod (s, &end);
	if (end == s || !(v > 0))
		return false;
	static const struct {
		const char *unit;
		double px;
	} units[] = {
		{ "px", 1 }, { "pt", 96.0 / 72 }, { "pc", 16 }, { "mm", 96 / 25.4 }, { "cm", 96 / 2.54 }, { "in", 96 },
	};
	if (*end == '"' || *end == '\'') {
		*px = v;
		return true;
	}
	for (size_t i = 0; i < sizeof units / sizeof units[0]; i++)
		if (strncmp (end, units[i].unit, 2) == 0 && (end[2] == '"' || end[2] == '\'')) {
			*px = v * units[i].px;
			return true;
		}
	/* Percentages and font sizes depend on what the SVG is put in. */
	return false;
}

/* The value of attribute `name' in the tag that `tag' starts. */
static const char *
svg_attr (const char *tag, const char *name)
{
	size_t len = strlen (name);
	for (const char *p = tag; *p != '\0' && *p != '>'; p++)
		if (isspace ((unsigned char) p[-1]) && strncmp (p, name, len) == 0) {
			const char *q = p + len;
			while (isspace ((unsigned char) *q))
				q++;
			if (*q++ != '=')
				continue;
			while (isspace ((unsigned char) *q))
				q++;
			if (*q == '"' || *q == '\'')
				return q + 1;
		}
	return NULL;
}

/* The size of an SVG: its width and height, or those of its viewBox
   for what it does not give. Sets `*format' if it is an SVG at all. */
static bool
svg_size (const Src *s, enum ImageFormat *format, long *w, long *h)
{
	char buf[65536];
	size_t n = MIN (sizeof buf - 1, s->data != NULL ? s->n : sizeof buf - 1);
	while (n > 0 && !src_read (s, 0, buf, n))
		n = s->data != NULL ? 0 : n / 2;
	buf[n] = '\0';
	if (memchr (buf, '\0', n) != NULL)	/* Not text. */
		return false;
	const char *tag = strstr (buf, "<svg");
	if (tag == NULL || !(isspace ((unsigned char) tag[4]) || tag[4] == '>'))
		return false;
	*format = IMAGE_SVG;
	tag += 4;
	const char *end = strchr (tag, '>');
	if (end == NULL)
		return false;

	double vw = 0, vh = 0;
	const char *vb = svg_attr (tag, "viewBox");
	if (vb != NULL) {
		double x, y;
		char *e;
		x = strtod (vb, &e);
		y = strtod (e + strspn (e, ", \t\r\n"), &e);
		vw = strtod (e + strspn (e, ", \t\r\n"), &e);
		vh = strtod (e + strspn (e, ", \t\r\n"), &e);
		(void) x;
		(void) y;
		if (!(vw > 0 && vh > 0))
			vw = vh = 0;
	}
	const char *aw = svg_attr (tag, "width"), *ah = svg_attr (tag, "height");
	double pw = 0, ph = 0;
	bool has_w = aw != NULL && aw < end && svg_length (aw, &pw);
	bool has_h = ah != NULL && ah < end && svg_length (ah, &ph);
	if (!has_w && !has_h) {
		pw = vw;
		ph = vh;
	} else if (!has_w)
		pw = vw > 0 ? ph * vw / vh : 0;
	else if (!has_h)
		ph = vh > 0 ? pw * vh / vw : 0;
	if (!(pw >= 0.5 && ph >= 0.5) || pw > 1e9 || ph > 1e9)
		return false;
	*w = (long) (pw + 0.5);
	*h = (long) (ph + 0.5);
	return true;
}

static bool
probe (const Src *s, enum ImageFormat *format, long *w, long *h)
{
	unsigned char d[32] = { 0 };
	size_t n = s->data != NULL ? MIN (s->n, sizeof d) : sizeof d;
	while (n > 0 && !src_read (s, 0, d, n))
		n--;
	*format = image_sniff (d, n);
	*w = *h = 0;
	bool ok;
	switch (*format) {
	case IMAGE_PNG:
		ok = n >= 24 && memcmp (d + 12, "IHDR", 4) == 0;
		if (ok) {
			*w = (long) be32 (d + 16);
			*h = (long) be32 (d + 20);
		}
		break;
	case IMAGE_GIF:
		ok = gif_size (s, w, h);
		break;
	case IMAGE_BMP:
		ok = n >= 26;
		if (ok && le32 (d + 14) == 12) {
			*w = (long) le16 (d + 18);
			*h = (long) le16 (d + 20);
		} else if (ok) {
			*w = (long) (int32_t) le32 (d + 18);
			*h = (long) (int32_t) le32 (d + 22);
			if (*h < 0)	/* Stored top-down. */
				*h = -*h;
		}
		break;
	case IMAGE_WEBP:
		ok = n >= 30;
		if (ok && memcmp (d + 12, "VP8X", 4) == 0) {
			*w = 1 + (long) (le16 (d + 24) | (unsigned long) d[26] << 16);
			*h = 1 + (long) (le16 (d + 27) | (unsigned long) d[29] << 16);
		} else if (ok && memcmp (d + 12, "VP8L", 4) == 0 && d[20] == 0x2f) {
			unsigned long b = le32 (d + 21);
			*w = 1 + (long) (b & 0x3fff);
			*h = 1 + (long) ((b >> 14) & 0x3fff);
		} else if (ok && memcmp (d + 12, "VP8 ", 4) == 0) {
			*w = (long) (le16 (d + 26) & 0x3fff);
			*h = (long) (le16 (d + 28) & 0x3fff);
		} else
			ok = false;
		break;
	case IMAGE_JPEG:
		ok = jpeg_size (s, w, h);
		break;
	case IMAGE_TIFF:
		ok = tiff_size (s, w, h);
		break;
	default:
		ok = svg_size (s, format, w, h);
		break;
	}
	return ok && *w > 0 && *h > 0;
}

bool
image_probe (const unsigned char *data, size_t n, enum ImageFormat *format, long *w, long *h)
{
	static const unsigned char empty[1];
	Src s = { data != NULL ? data : empty, data != NULL ? n : 0, -1 };
	return probe (&s, format, w, h);
}

bool
image_probe_fd (int fd, enum ImageFormat *format, long *w, long *h)
{
	Src s = { NULL, 0, fd };
	return probe (&s, format, w, h);
}

bool
image_probe_file (const char *path, enum ImageFormat *format, long *w, long *h)
{
	*format = IMAGE_UNKNOWN;
	*w = *h = 0;
	int fd = open (path, O_RDONLY);
	if (fd < 0)
		return false;
	bool ok = image_probe_fd (fd, format, w, h);
	close (fd);
	return ok;
}

const char *
image_format_name (enum ImageFormat format)
{
	static const char *const names[] = { "unknown", "PNG", "JPEG", "GIF", "WebP", "TIFF", "BMP", "SVG" };
	return names[format];
}
