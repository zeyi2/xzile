/* Image mode

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

/* An image is shown by preview.c; xzile-gui decodes it in
   image_decode.c. */

#include "preview.h"

static Binding *image_map;

Binding *
image_keymap (void)
{
	if (image_map == NULL)
		image_map = preview_keymap_new ();
	return image_map;
}

char *
image_status (const PreviewGeometry *g, const char *fit, int percent)
{
	return xasprintf ("%.0fx%.0f  %s%d%%%s", g->pw, g->ph, fit, percent, g->reduced ? " (reduced)" : "");
}

char *
image_about (const PreviewInfo *info)
{
	return xasprintf ("%.0fx%.0f image%s", info->width, info->height,
			  info->reduced ? ", shown at reduced resolution" : "");
}

static bool
F_image_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return preview_enter (PREVIEW_IMAGE);
}

void
image_mode_init (void)
{
	lisp_func_new ("image-mode", F_image_mode, true,
		       "Show the file of the current buffer as an image, in xzile-gui.\n"
		       "A binary image file is shown this way when visited. In the preview,\n"
		       "+ and - zoom, 0 fits the window, 1 shows actual size, W, H and P fit\n"
		       "the width, height or page, the arrows and the mouse move around,\n"
		       "Ctrl and the wheel zoom, g reloads, i describes, q quits and C-c C-c\n"
		       "shows a text file as text again.");
}
