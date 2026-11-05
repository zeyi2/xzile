/* Preview buffers: images and PDFs shown in their windows

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

/* Images and PDFs are drawn in their windows by the frontend, through
   preview_backend; only xzile-gui has one. A binary file visited there
   gets a preview buffer, which has no text and cannot be saved. A text
   file, such as an SVG, can be shown as a preview with image-mode, and
   as text again with C-c C-c; its text is untouched meanwhile.

   This file has what images and PDFs share; image.c and pdf.c have what
   differs. */

#include "preview.h"

const PreviewBackend *preview_backend;

struct Preview {
	/* The preview in the window, if kind is not PREVIEW_NONE. */
	enum PreviewKind kind;
	PreviewDoc *doc;
	PreviewView view;
	bool binary;		/* There is no text: the file is binary. */
	Binding *text_keymap;	/* The text view's, while previewing. */
	char *text_label;
	bool text_readonly;
	int64_t wheel_turn;	/* When the wheel last turned the page. */
};

/* For the text view of a file shown as a preview, if it has no keymap
   of its own. */
static Binding *toggle_map;

/* Not fabs and pow: the terminal build links only the C library. */
static double
absd (double x)
{
	return x < 0 ? -x : x;
}

static double
power (double x, long n)
{
	double r = 1;
	for (long i = 0; i < labs (n) && i < 64; i++)
		r *= x;
	return n < 0 ? 1 / r : r;
}

bool
preview_showing (const Buffer *bp)
{
	return bp->preview != NULL && bp->preview->kind != PREVIEW_NONE;
}

static const char *
kind_label (enum PreviewKind kind)
{
	return kind == PREVIEW_PDF ? "PDF" : "Image";
}

static Binding *
kind_keymap (enum PreviewKind kind)
{
	return kind == PREVIEW_PDF ? pdf_keymap () : image_keymap ();
}

static void
resync_windows (Buffer *bp)
{
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (wp->bp == bp)
			wp->topdelta = 0;
	thisflag |= FLAG_NEED_RESYNC;
}

/* Show `bp' as a preview of its file. */
static void
show_preview (Buffer *bp, enum PreviewKind kind, bool binary)
{
	if (bp->preview == NULL)
		bp->preview = xzalloc (sizeof (Preview));
	Preview *pv = bp->preview;
	if (pv->kind == PREVIEW_NONE) {
		pv->text_keymap = bp->keymap != toggle_map ? bp->keymap : NULL;
		pv->text_label = bp->mode_label;
		pv->text_readonly = bp->readonly;
		bp->mode_label = NULL;
	}
	if (pv->doc != NULL)
		preview_backend->close (pv->doc);
	pv->doc = preview_backend->open (kind, bp->filename);
	pv->kind = kind;
	pv->binary = pv->binary || binary;
	pv->view = preview_initial_view (kind);
	bp->keymap = kind_keymap (kind);
	free (bp->mode_label);
	bp->mode_label = xstrdup (kind_label (kind));
	bp->readonly = true;
	resync_windows (bp);
}

/* Show a text buffer as text again. */
static void
hide_preview (Buffer *bp)
{
	Preview *pv = bp->preview;
	if (pv == NULL || pv->kind == PREVIEW_NONE)
		return;
	preview_backend->close (pv->doc);
	pv->doc = NULL;
	pv->kind = PREVIEW_NONE;
	if (pv->text_keymap == NULL && toggle_map == NULL) {
		toggle_map = keymap_new ();
		keymap_bind (toggle_map, "\\C-c\\C-c", "preview-toggle-display");
	}
	bp->keymap = pv->text_keymap != NULL ? pv->text_keymap : toggle_map;
	free (bp->mode_label);
	bp->mode_label = pv->text_label;
	pv->text_label = NULL;
	bp->readonly = pv->text_readonly;
	resync_windows (bp);
}

void
preview_free (Buffer *bp)
{
	Preview *pv = bp->preview;
	if (pv == NULL)
		return;
	if (pv->doc != NULL && preview_backend != NULL)
		preview_backend->close (pv->doc);
	free (pv->text_label);
	free (pv);
	bp->preview = NULL;
}


/* Visiting files. */

Buffer *
preview_visit (const char *filename)
{
	if (preview_backend == NULL || !get_variable_bool ("preview-binary-files"))
		return NULL;
	enum PreviewKind kind = preview_sniff_file (filename);
	if (kind == PREVIEW_NONE || !preview_backend->supports (kind))
		return NULL;

	Buffer *bp = buffer_new (NULL);
	buffer_set_names (bp, filename);
	free (bp->dir);
	bp->dir = path_dirname (bp->filename);
	bp->nosave = true;
	bp->noundo = true;
	show_preview (bp, kind, true);
	return bp;
}

bool
preview_refuse_save (Buffer *bp)
{
	if (bp->preview == NULL || !bp->preview->binary)
		return false;
	minibuf_error ("%s is a preview of a binary file, with no text to save", bp->name);
	return true;
}

bool
preview_follow_disk (Buffer *bp)
{
	if (!preview_showing (bp))
		return false;
	preview_backend->reload (bp->preview->doc);
	return bp->preview->binary;
}


/* Display. */

/* `s' centred on row `y' of the `width' columns at `x'. */
static void
add_centred (size_t y, size_t x, size_t width, const char *s)
{
	size_t len = strlen (s), i = 0, cols = 0;
	while (i < len) {
		size_t char_len;
		uint32_t ch = utf8_decode (s, len, i, &char_len);
		size_t w = (size_t) MAX (utf8_char_display_width (ch), 0);
		if (cols + w > width)
			break;
		cols += w;
		i += char_len;
	}
	term_move (y, x + (width - cols) / 2);
	term_addnstr (s, i);
}

void
preview_begin_redisplay (void)
{
	if (preview_backend != NULL)
		preview_backend->clear ();
}

bool
preview_draw (Window *wp, size_t topline, size_t leftcol)
{
	if (!preview_showing (wp->bp))
		return false;
	Preview *pv = wp->bp->preview;

	for (size_t y = 0; y < wp->eheight; y++) {
		term_move (topline + y, leftcol);
		term_add_picture (wp->ewidth);
	}

	PreviewInfo info;
	preview_backend->info (pv->doc, pv->view.page, &info);
	if (info.state == PREVIEW_READY && pv->view.page >= info.pages)
		pv->view.page = MAX (info.pages - 1, 0);
	preview_backend->place (pv->doc, &pv->view, topline, leftcol, wp->eheight, wp->ewidth);

	if ((info.state != PREVIEW_READY || info.page_error != NULL) && wp->eheight > 0) {
		char *msg = info.state == PREVIEW_LOADING ? xasprintf ("Loading %s\xe2\x80\xa6", wp->bp->name)
			: info.state == PREVIEW_READY ? xasprintf ("Cannot show page %ld: %s", pv->view.page + 1, info.page_error)
			: xasprintf ("Cannot show %s: %s", wp->bp->name, info.error != NULL ? info.error : "unknown error");
		term_apply_face (info.state == PREVIEW_LOADING ? FACE_DEFAULT : FACE_ERROR);
		add_centred (topline + wp->eheight / 2, leftcol, wp->ewidth, msg);
		term_apply_face (FACE_DEFAULT);
		free (msg);
	}
	return true;
}

/* The geometry of the preview in `wp', if it has loaded. */
static bool
geometry (Window *wp, PreviewGeometry *g)
{
	Preview *pv = wp->bp->preview;
	PreviewInfo info;
	preview_backend->info (pv->doc, pv->view.page, &info);
	if (info.state != PREVIEW_READY)
		return false;
	g->view = &pv->view;
	preview_backend->cell_pixels (wp->eheight, wp->ewidth, &g->aw, &g->ah);
	g->pw = info.width;
	g->ph = info.height;
	g->pages = info.pages;
	g->reduced = info.reduced;
	return true;
}

char *
preview_status (Window *wp)
{
	if (!preview_showing (wp->bp))
		return NULL;
	Preview *pv = wp->bp->preview;
	PreviewGeometry g;
	if (!geometry (wp, &g)) {
		PreviewInfo info;
		preview_backend->info (pv->doc, pv->view.page, &info);
		return xstrdup (info.state == PREVIEW_LOADING ? "Loading" : "Error");
	}
	PreviewRect r = preview_layout (g.view, g.aw, g.ah, g.pw, g.ph);
	static const char *const fits[] = { "", "Fit page ", "Fit width ", "Fit height ", "" };
	int percent = (int) (r.scale * 100 + 0.5);
	return pv->kind == PREVIEW_PDF ? pdf_status (&g, fits[g.view->fit], percent)
		: image_status (&g, fits[g.view->fit], percent);
}


/* The mouse. */

typedef struct {
	size_t y, x;
	WinNode *leaf;
} LeafAt;

static void
find_leaf_at (WinNode *leaf, void *data)
{
	LeafAt *d = data;
	if (d->y >= leaf->y && d->y < leaf->y + leaf->wp->eheight &&
	    d->x >= leaf->x && d->x < leaf->x + leaf->wp->ewidth)
		d->leaf = leaf;
}

/* The window whose text area has cell `y', `x', if it shows a preview. */
static WinNode *
preview_leaf (size_t y, size_t x)
{
	if (preview_backend == NULL || root_node == NULL)
		return NULL;
	LeafAt d = { y, x, NULL };
	winnode_each_leaf (root_node, find_leaf_at, &d);
	return d.leaf != NULL && preview_showing (d.leaf->wp->bp) ? d.leaf : NULL;
}

bool
preview_at (size_t y, size_t x)
{
	return preview_leaf (y, x) != NULL;
}

#define WHEEL_TURN_MS 300

bool
preview_pointer_scroll (size_t y, size_t x, double dx, double dy, bool turn_pages)
{
	WinNode *leaf = preview_leaf (y, x);
	PreviewGeometry g;
	if (leaf == NULL || !geometry (leaf->wp, &g))
		return false;
	Preview *pv = leaf->wp->bp->preview;
	double mx, my;
	preview_pan (g.view, dx, dy, g.aw, g.ah, g.pw, g.ph, &mx, &my);
	/* Wheeling past the end of a page goes to the next one, but not
	   so fast that a fling skips pages. */
	if (turn_pages && pv->kind == PREVIEW_PDF && dy != 0 && absd (my) < 0.5) {
		int64_t now = monotonic_ms ();
		if (now - pv->wheel_turn >= WHEEL_TURN_MS && pdf_turn_page (&g, dy > 0 ? 1 : -1, dy < 0))
			pv->wheel_turn = now;
	} else if (absd (my) >= 0.5)
		pv->wheel_turn = monotonic_ms ();
	return true;
}

bool
preview_pointer_zoom (size_t y, size_t x, double px, double py, double factor)
{
	WinNode *leaf = preview_leaf (y, x);
	PreviewGeometry g;
	if (leaf == NULL || !geometry (leaf->wp, &g))
		return false;
	double ox, oy;
	preview_backend->cell_pixels (leaf->y, leaf->x, &ox, &oy);
	preview_zoom (g.view, factor, px - ox, py - oy, g.aw, g.ah, g.pw, g.ph);
	return true;
}


/* For the commands of image.c and pdf.c. */

static Preview *
current_preview (void)
{
	if (!preview_showing (cur_bp)) {
		minibuf_error ("Not showing a preview");
		return NULL;
	}
	return cur_bp->preview;
}

bool
preview_current (PreviewGeometry *g)
{
	if (current_preview () == NULL)
		return false;
	if (!geometry (cur_wp, g)) {
		minibuf_error ("%s has not loaded", cur_bp->name);
		return false;
	}
	return true;
}

long
preview_count_arg (long uniarg, Args *args)
{
	long n;
	if (args != NULL && !args_empty (args) && int_arg (args, &n))
		return n;
	return uniarg;
}

bool
preview_enter (enum PreviewKind kind)
{
	const char *what = kind_label (kind);
	if (remote_refuse (cur_bp, "Previewing"))
		return false;
	if (preview_backend == NULL) {
		minibuf_error ("%s previews need the graphical xzile-gui", what);
		return false;
	}
	if (!preview_backend->supports (kind)) {
		minibuf_error ("%s previews are not built into this xzile-gui; build it with `make gui'",
			       what);
		return false;
	}
	if (cur_bp->filename == NULL || cur_bp->needname) {
		minibuf_error ("Buffer is not visiting a file");
		return false;
	}
	if (!file_is_regular (cur_bp->filename)) {
		minibuf_error ("%s does not exist; save it first", cur_bp->filename);
		return false;
	}
	if (cur_bp->shell != NULL || cur_bp->dired != NULL || cur_bp->magit != NULL || cur_bp->compilation != NULL) {
		minibuf_error ("Cannot preview this buffer");
		return false;
	}
	if (preview_showing (cur_bp) && cur_bp->preview->kind == kind)
		return true;
	show_preview (cur_bp, kind, false);
	if (cur_bp->modified)
		minibuf_write ("Showing the file on disk; the buffer has unsaved changes");
	return true;
}


/* Commands common to images and PDFs. */

/* Scroll by most of a window, going on to the next or previous page
   at the end of one. */
static bool
scroll_screens (long n)
{
	PreviewGeometry g;
	if (!preview_current (&g))
		return false;
	bool pdf = cur_bp->preview->kind == PREVIEW_PDF;
	for (long i = 0; i < labs (n); i++) {
		double my;
		preview_pan (g.view, 0, (n > 0 ? 0.9 : -0.9) * g.ah, g.aw, g.ah, g.pw, g.ph, NULL, &my);
		if (absd (my) < 0.5 && !(pdf && pdf_turn_page (&g, n > 0 ? 1 : -1, n < 0))) {
			minibuf_error (n > 0 ? "End of %s" : "Beginning of %s", pdf ? "document" : "image");
			return false;
		}
	}
	return true;
}

static bool
F_preview_scroll_up (long uniarg, Args *args)
{
	return scroll_screens (preview_count_arg (uniarg, args));
}

static bool
F_preview_scroll_down (long uniarg, Args *args)
{
	return scroll_screens (-preview_count_arg (uniarg, args));
}

/* Pan by `cols' columns and `rows' lines. */
static bool
pan_cells (long cols, long rows)
{
	PreviewGeometry g;
	if (!preview_current (&g))
		return false;
	double cw, ch;
	preview_backend->cell_pixels (1, 1, &cw, &ch);
	preview_pan (g.view, (double) cols * cw, (double) rows * ch, g.aw, g.ah, g.pw, g.ph, NULL, NULL);
	return true;
}

#define PAN_COLUMNS 8
#define PAN_LINES 3

static bool
F_preview_pan_left (long uniarg, Args *args)
{
	return pan_cells (-PAN_COLUMNS * preview_count_arg (uniarg, args), 0);
}

static bool
F_preview_pan_right (long uniarg, Args *args)
{
	return pan_cells (PAN_COLUMNS * preview_count_arg (uniarg, args), 0);
}

static bool
F_preview_pan_up (long uniarg, Args *args)
{
	return pan_cells (0, -PAN_LINES * preview_count_arg (uniarg, args));
}

static bool
F_preview_pan_down (long uniarg, Args *args)
{
	return pan_cells (0, PAN_LINES * preview_count_arg (uniarg, args));
}

#define ZOOM_STEP 1.25

static bool
zoom_by (long n)
{
	PreviewGeometry g;
	if (!preview_current (&g))
		return false;
	preview_zoom (g.view, power (ZOOM_STEP, n), g.aw / 2, g.ah / 2, g.aw, g.ah, g.pw, g.ph);
	return true;
}

static bool
F_preview_zoom_in (long uniarg, Args *args)
{
	return zoom_by (preview_count_arg (uniarg, args));
}

static bool
F_preview_zoom_out (long uniarg, Args *args)
{
	return zoom_by (-preview_count_arg (uniarg, args));
}

static bool
set_fit (enum PreviewFit fit)
{
	Preview *pv = current_preview ();
	if (pv == NULL)
		return false;
	pv->view.fit = fit;
	pv->view.zoom = 1;
	return true;
}

static bool
F_preview_fit_page (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return set_fit (PREVIEW_FIT_PAGE);
}

static bool
F_preview_fit_width (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return set_fit (PREVIEW_FIT_WIDTH);
}

static bool
F_preview_fit_height (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return set_fit (PREVIEW_FIT_HEIGHT);
}

static bool
F_preview_actual_size (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return set_fit (PREVIEW_FIT_NONE);
}

static bool
F_preview_reset (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Preview *pv = current_preview ();
	if (pv == NULL)
		return false;
	long page = pv->view.page;
	pv->view = preview_initial_view (pv->kind);
	pv->view.page = page;
	return true;
}

static bool
F_preview_reload (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Preview *pv = current_preview ();
	if (pv == NULL)
		return false;
	preview_backend->reload (pv->doc);
	if (pv->binary)
		buffer_note_disk (cur_bp);
	minibuf_write ("Reloading %s", cur_bp->filename);
	return true;
}

static bool
F_preview_toggle_display (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (remote_refuse (cur_bp, "preview-toggle-display"))
		return false;
	if (preview_showing (cur_bp)) {
		if (cur_bp->preview->binary) {
			minibuf_error ("%s is binary and has no text view; "
				       "set preview-binary-files to nil to visit such files as text", cur_bp->name);
			return false;
		}
		hide_preview (cur_bp);
		return true;
	}
	enum PreviewKind kind = cur_bp->filename != NULL ? preview_sniff_file (cur_bp->filename) : PREVIEW_NONE;
	return preview_enter (kind == PREVIEW_NONE ? PREVIEW_IMAGE : kind);
}

static bool
F_preview_quit (long uniarg, Args *args)
{
	Preview *pv = current_preview ();
	if (pv == NULL)
		return false;
	if (!pv->binary)
		return F_preview_toggle_display (uniarg, args);
	buffer_kill (cur_bp);
	thisflag |= FLAG_NEED_RESYNC;
	return true;
}

static bool
F_preview_info (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	Preview *pv = current_preview ();
	if (pv == NULL)
		return false;
	PreviewInfo info;
	preview_backend->info (pv->doc, pv->view.page, &info);
	char *how = preview_backend->describe (pv->doc);
	if (info.state == PREVIEW_READY) {
		char *what = pv->kind == PREVIEW_PDF ? pdf_about (&info) : image_about (&info);
		if (info.page_error != NULL)
			minibuf_write ("%s: %s; page %ld cannot be shown: %s; %s", cur_bp->name, what,
				       pv->view.page + 1, info.page_error, how);
		else
			minibuf_write ("%s: %s; %s", cur_bp->name, what, how);
		free (what);
	} else
		minibuf_write ("%s: %s; %s", cur_bp->name,
			       info.state == PREVIEW_LOADING ? "loading" : info.error, how);
	free (how);
	return true;
}

Binding *
preview_keymap_new (void)
{
	static const struct {
		const char *keys, *func;
	} keys[] = {
		{ "\\SPC", "preview-scroll-up" },
		{ "\\C-v", "preview-scroll-up" },
		{ "\\BACKSPACE", "preview-scroll-down" },
		{ "\\M-v", "preview-scroll-down" },
		{ "\\LEFT", "preview-pan-left" },
		{ "\\C-b", "preview-pan-left" },
		{ "\\RIGHT", "preview-pan-right" },
		{ "\\C-f", "preview-pan-right" },
		{ "\\UP", "preview-pan-up" },
		{ "\\C-p", "preview-pan-up" },
		{ "\\DOWN", "preview-pan-down" },
		{ "\\C-n", "preview-pan-down" },
		{ "+", "preview-zoom-in" },
		{ "=", "preview-zoom-in" },
		{ "-", "preview-zoom-out" },
		{ "0", "preview-reset" },
		{ "1", "preview-actual-size" },
		{ "P", "preview-fit-page" },
		{ "W", "preview-fit-width" },
		{ "H", "preview-fit-height" },
		{ "g", "preview-reload" },
		{ "r", "preview-reload" },
		{ "i", "preview-info" },
		{ "q", "preview-quit" },
		{ "\\C-c\\C-c", "preview-toggle-display" },
	};
	Binding *map = keymap_new ();
	for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
		keymap_bind (map, keys[i].keys, keys[i].func);
	return map;
}

void
preview_init (void)
{
	lisp_func_new ("preview-toggle-display", F_preview_toggle_display, true,
		       "Switch between showing a file as a preview and as text.");
	lisp_func_new ("preview-scroll-up", F_preview_scroll_up, true,
		       "Scroll the preview forward by nearly a window, onto the next page\n"
		       "at the end of one.");
	lisp_func_new ("preview-scroll-down", F_preview_scroll_down, true,
		       "Scroll the preview back by nearly a window, onto the previous page\n"
		       "at the start of one.");
	lisp_func_new ("preview-pan-left", F_preview_pan_left, true,
		       "Move the view of the preview left.");
	lisp_func_new ("preview-pan-right", F_preview_pan_right, true,
		       "Move the view of the preview right.");
	lisp_func_new ("preview-pan-up", F_preview_pan_up, true,
		       "Move the view of the preview up.");
	lisp_func_new ("preview-pan-down", F_preview_pan_down, true,
		       "Move the view of the preview down.");
	lisp_func_new ("preview-zoom-in", F_preview_zoom_in, true,
		       "Enlarge the preview by a quarter; with ARG, ARG times.");
	lisp_func_new ("preview-zoom-out", F_preview_zoom_out, true,
		       "Shrink the preview by a fifth; with ARG, ARG times.");
	lisp_func_new ("preview-fit-page", F_preview_fit_page, true,
		       "Fit the whole page in the window.");
	lisp_func_new ("preview-fit-width", F_preview_fit_width, true,
		       "Fit the width of the page to the window.");
	lisp_func_new ("preview-fit-height", F_preview_fit_height, true,
		       "Fit the height of the page to the window.");
	lisp_func_new ("preview-actual-size", F_preview_actual_size, true,
		       "Show the preview at 100%: an image pixel to a screen pixel, or a\n"
		       "PDF at 96 pixels to the inch.");
	lisp_func_new ("preview-reset", F_preview_reset, true,
		       "Return to the initial view: an image fitted to the window if it is\n"
		       "larger, a PDF page fitted to its width.");
	lisp_func_new ("preview-reload", F_preview_reload, true,
		       "Read the file of the preview again.");
	lisp_func_new ("preview-info", F_preview_info, true,
		       "Describe the preview, and how it is drawn.");
	lisp_func_new ("preview-quit", F_preview_quit, true,
		       "Leave the preview: show a text file as text again, or kill the\n"
		       "buffer of a binary file.");
}
