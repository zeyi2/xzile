/* PDF mode

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

/* A PDF is shown by preview.c, a page at a time; xzile-gui renders
   the pages in pdf_render.c. Here are its pages. */

#include "preview.h"

static Binding *pdf_map;

Binding *
pdf_keymap (void)
{
	if (pdf_map != NULL)
		return pdf_map;
	static const struct {
		const char *keys, *func;
	} keys[] = {
		{ "n", "preview-next-page" },
		{ "\\PAGEDOWN", "preview-next-page" },
		{ "p", "preview-previous-page" },
		{ "\\PAGEUP", "preview-previous-page" },
		{ "\\M-<", "preview-first-page" },
		{ "\\HOME", "preview-first-page" },
		{ "\\M->", "preview-last-page" },
		{ "\\END", "preview-last-page" },
		{ "\\M-gg", "preview-goto-page" },
		{ "\\M-g\\M-g", "preview-goto-page" },
	};
	pdf_map = preview_keymap_new ();
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
		keymap_bind (pdf_map, keys[i].keys, keys[i].func);
	return pdf_map;
}

char *
pdf_status (const PreviewGeometry *g, const char *fit, int percent)
{
	return xasprintf ("Page %ld/%ld  %s%d%%", g->view->page + 1, g->pages, fit, percent);
}

char *
pdf_about (const PreviewInfo *info)
{
	return xasprintf ("PDF, %ld pages", info->pages);
}

bool
pdf_turn_page (const PreviewGeometry *g, long n, bool from_end)
{
	long page = g->view->page + n;
	page = MAX (0, MIN (page, g->pages - 1));
	if (page == g->view->page)
		return false;
	g->view->page = page;
	g->view->cy = from_end ? 1 : 0;
	return true;
}

static bool
go_pages (long n)
{
	PreviewGeometry g;
	if (!preview_current (&g))
		return false;
	if (!pdf_turn_page (&g, n, false)) {
		minibuf_error (n > 0 ? "Last page" : "First page");
		return false;
	}
	return true;
}

/* Show page `page', from 0, at its top. */
static bool
go_to (long page)
{
	PreviewGeometry g;
	if (!preview_current (&g))
		return false;
	g.view->page = page < 0 ? g.pages - 1 : page;
	g.view->cy = 0;
	return true;
}

static bool
F_preview_next_page (long uniarg, Args *args)
{
	return go_pages (preview_count_arg (uniarg, args));
}

static bool
F_preview_previous_page (long uniarg, Args *args)
{
	return go_pages (-preview_count_arg (uniarg, args));
}

static bool
F_preview_first_page (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return go_to (0);
}

static bool
F_preview_last_page (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return go_to (-1);
}

static bool
F_preview_goto_page (long uniarg, Args *args)
{
	PreviewGeometry g;
	if (!preview_current (&g))
		return false;
	long n;
	if (args != NULL && !args_empty (args)) {
		if (!int_arg (args, &n))
			return false;
	} else if (args == NULL)
		n = uniarg;
	else {
		n = minibuf_read_number ("Goto page (1-%ld): ", g.pages);
		if (n >= LONG_MAX - 1)
			return n == LONG_MAX - 1;
	}
	if (n < 1 || n > g.pages) {
		minibuf_error ("No page %ld; there are %ld", n, g.pages);
		return false;
	}
	return go_to (n - 1);
}

static bool
F_pdf_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return preview_enter (PREVIEW_PDF);
}

void
pdf_mode_init (void)
{
	lisp_func_new ("pdf-mode", F_pdf_mode, true,
		       "Show the file of the current buffer as a PDF document, in xzile-gui.\n"
		       "A PDF file is shown this way when visited. n and p (or PageDown and\n"
		       "PageUp) turn pages, SPC and DEL scroll a screen at a time onto the next\n"
		       "or previous page, M-< and M-> go to the first and last page and M-g g\n"
		       "to any. Zooming and moving are as in `image-mode'.");
	lisp_func_new ("preview-next-page", F_preview_next_page, true,
		       "Show the next page of the document; with ARG, ARG pages on.");
	lisp_func_new ("preview-previous-page", F_preview_previous_page, true,
		       "Show the previous page of the document; with ARG, ARG pages back.");
	lisp_func_new ("preview-first-page", F_preview_first_page, true,
		       "Show the first page of the document.");
	lisp_func_new ("preview-last-page", F_preview_last_page, true,
		       "Show the last page of the document.");
	lisp_func_new ("preview-goto-page", F_preview_goto_page, true,
		       "Show page PAGE of the document.");
}
