/* Decoding images for xzile-gui, with SDL2_image

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

/* The size of an image is read from its header (image_probe.c) before
   it is decoded, from the same open file, and the image is decoded by
   the loader of the format the header is of. An image whose size cannot
   be read is not decoded: a decoder allocates what the header asks, so
   a small file could otherwise take gigabytes. */

#include "preview.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>

#include <SDL.h>
#ifdef HAVE_SDL_IMAGE
#include <SDL_image.h>
#endif

#include "gui_render.h"

/* Larger images are not decoded at all... */
#define MAX_DECODE_PIXELS (128.0 * 1024 * 1024)
/* ...and those larger than this are shown at reduced resolution. */
#define MAX_IMAGE_TEXELS (24.0 * 1024 * 1024)
#define MIN_LEVEL 64

void
image_decoded_free (DecodedImage *img)
{
	for (int i = 0; i < img->nlevels; i++)
		SDL_FreeSurface (img->levels[i]);
	img->nlevels = 0;
}

#ifdef HAVE_SDL_IMAGE

bool
image_decode_supported (void)
{
	return true;
}

void
image_decode_init (void)
{
	/* Now, rather than lazily from two workers at once. */
	IMG_Init (IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_TIF | IMG_INIT_WEBP);
}

void
image_decode_quit (void)
{
	IMG_Quit ();
}

static SDL_Surface *
stretch (SDL_Surface *src, int w, int h)
{
	SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat (0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (dst != NULL && SDL_SoftStretchLinear (src, NULL, dst, NULL) != 0) {
		SDL_FreeSurface (dst);
		dst = NULL;
	}
	return dst;
}

static bool
has_alpha (SDL_Surface *s)
{
	for (int y = 0; y < s->h; y++) {
		const Uint32 *p = (const Uint32 *) ((const Uint8 *) s->pixels + (size_t) y * (size_t) s->pitch);
		for (int x = 0; x < s->w; x++)
			if ((p[x] >> 24) != 0xff)
				return true;
	}
	return false;
}

/* Read the image of `fp', checking its size first. */
static char *
load (FILE *fp, SDL_Surface **out)
{
	enum ImageFormat format;
	long w, h;
	bool sized = image_probe_fd (fileno (fp), &format, &w, &h);
	if (format == IMAGE_UNKNOWN)
		return xstrdup ("not an image format that can be shown");
	if (!sized)
		return xasprintf ("the size of this %s image cannot be read from its header",
				  image_format_name (format));
	if ((double) w * (double) h > MAX_DECODE_PIXELS)
		return xasprintf ("the image is too large (%ldx%ld)", w, h);

	/* Not IMG_Load: it picks the loader from the content again, and
	   would take a TIFF with "<svg" near its start for an SVG. */
	static SDL_Surface *(*const loaders[]) (SDL_RWops *src) = {
		[IMAGE_PNG] = IMG_LoadPNG_RW, [IMAGE_JPEG] = IMG_LoadJPG_RW, [IMAGE_GIF] = IMG_LoadGIF_RW,
		[IMAGE_WEBP] = IMG_LoadWEBP_RW, [IMAGE_TIFF] = IMG_LoadTIF_RW, [IMAGE_BMP] = IMG_LoadBMP_RW,
		[IMAGE_SVG] = IMG_LoadSVG_RW,
	};
	SDL_RWops *rw = SDL_RWFromFP (fp, SDL_FALSE);
	if (rw == NULL)
		return xstrdup (SDL_GetError ());
	*out = loaders[format] (rw);
	SDL_RWclose (rw);
	return *out == NULL ? xstrdup (IMG_GetError ()) : NULL;
}

char *
image_decode (const char *path, int max_w, int max_h, DecodedImage *img)
{
	memset (img, 0, sizeof *img);
	FILE *fp = fopen (path, "rb");
	if (fp == NULL)
		return xstrdup (strerror (errno));
	SDL_Surface *raw = NULL;
	char *error = load (fp, &raw);
	fclose (fp);
	if (error != NULL)
		return error;

	bool maybe_alpha = raw->format->Amask != 0 || SDL_HasColorKey (raw);
	SDL_Surface *s = SDL_ConvertSurfaceFormat (raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface (raw);
	if (s == NULL)
		return xstrdup (SDL_GetError ());
	img->width = s->w;
	img->height = s->h;
	img->alpha = maybe_alpha && has_alpha (s);

	double f = 1, px = (double) s->w * (double) s->h;
	if (max_w > 0 && s->w > max_w)
		f = MIN (f, (double) max_w / s->w);
	if (max_h > 0 && s->h > max_h)
		f = MIN (f, (double) max_h / s->h);
	if (px * f * f > MAX_IMAGE_TEXELS)
		f = sqrt (MAX_IMAGE_TEXELS / px);
	if (f < 1) {
		SDL_Surface *t = stretch (s, MAX (1, (int) (s->w * f)), MAX (1, (int) (s->h * f)));
		SDL_FreeSurface (s);
		if (t == NULL)
			return xasprintf ("cannot reduce the image: %s", SDL_GetError ());
		s = t;
		img->reduced = true;
	}
	img->levels[img->nlevels++] = s;
	while (img->nlevels < IMAGE_MAX_LEVELS && (s->w > MIN_LEVEL || s->h > MIN_LEVEL)) {
		SDL_Surface *t = stretch (s, MAX (1, s->w / 2), MAX (1, s->h / 2));
		if (t == NULL)
			break;
		img->levels[img->nlevels++] = s = t;
	}
	return NULL;
}

#else

bool
image_decode_supported (void)
{
	return false;
}

void
image_decode_init (void)
{
}

void
image_decode_quit (void)
{
}

char *
image_decode (const char *path, int max_w, int max_h, DecodedImage *img)
{
	(void) path;
	(void) max_w;
	(void) max_h;
	memset (img, 0, sizeof *img);
	return xstrdup ("image support is not built in");
}

#endif
