/* A small widget toolkit for the screen grid

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

/* The toolkit draws into the screen grid that both frontends show, so
   the terminal and the window get the same panels, lists, scroll bars
   and mouse handling.

   It is built afresh on every redisplay, as XZile draws: a tree of
   nodes is made in a per-frame arena, laid out under size constraints
   (rows and columns share space among their flexible children, as in
   Flutter), and painted. Painting a clickable node records a region;
   mouse events go to the topmost region under the pointer, and a press
   captures the pointer, so that drags and the release go to the region
   pressed even when the pointer leaves it. State that lasts, such as a
   scroll position or a selection, belongs to the caller; the toolkit
   remembers only what is hovered and pressed, by the key each region
   is given, so that a node can be drawn hovered as it is built.

   Styles are faces, so themes apply. A node drawn inside a panel takes
   the panel's colours where its own face leaves them out. */

#ifndef TUI_H
#define TUI_H

/* Unbounded, for constraints. */
#define TUI_INF (INT_MAX / 4)

typedef struct {
	int x, y, w, h;
} TuiRect;

typedef struct TuiNode TuiNode;

/* What identifies a region from one frame to the next. 0 is none. */
typedef uintptr_t TuiKey;

/* A key from two numbers, such as a kind of control and an index. */
#define TUI_KEY(kind, n) ((TuiKey) (((uintptr_t) (kind) << 24) ^ (uintptr_t) (n)))

/* The pointer shapes a region may ask for. */
enum TuiCursor {
	TUI_CURSOR_DEFAULT,
	TUI_CURSOR_POINTER,	/* Something to click. */
	TUI_CURSOR_EW_RESIZE,
	TUI_CURSOR_NS_RESIZE,
};

/* Mouse events. A press is reported with the number of clicks it
   makes (2 for a double click); drags and the release go to the region
   pressed. `moved' says whether the pointer moved between the press and
   the release. */
enum TuiEventKind { TUI_PRESS, TUI_DRAG, TUI_RELEASE, TUI_WHEEL };

typedef struct {
	enum TuiEventKind kind;
	int y, x;		/* The pointer, relative to the region. */
	int clicks;		/* For a press. */
	int wheel;		/* -1 up, 1 down. */
	bool moved;		/* For a drag and the release. */
	TuiRect rect;		/* The region, on the screen. */
	long *state;		/* Kept from the press to the release. */
	long a, b;		/* The region's arguments. */
} TuiEvent;

/* Returns whether it handled the event. A press it does not handle
   goes to the regions below; so does an unhandled wheel event. */
typedef bool (*TuiHandler) (const TuiEvent *ev, void *data);

/* A clickable region. `data' must outlive the frame, as a press keeps
   it until the release; `a' and `b' are copied, for what may go away
   meanwhile (such as a window, by its id). */
typedef struct {
	TuiKey key;
	TuiHandler handler;
	void *data;
	size_t data_size;	/* If not 0, `data' is copied by a press, and
				   need only last the frame. */
	long a, b;
	enum TuiCursor cursor;
	bool opaque;		/* Takes every press, handled or not. */
} TuiRegionSpec;

enum {
	TUI_BOLD = 1,
	TUI_UNDERLINE = 2,
	TUI_REVERSE = 4,
	TUI_DIM = 8,
};

/* A run of text in a face (NULL: the inherited colours). */
typedef struct {
	const char *text;
	const char *face;
	unsigned flags;
} TuiSpan;

enum TuiBorder { TUI_BORDER_NONE, TUI_BORDER_ROUNDED, TUI_BORDER_SINGLE };

/* A key and what it does, for a panel's footer; a click on it pushes
   `key'. */
typedef struct {
	const char *keys;	/* As shown, e.g. "RET". */
	const char *label;
	Keystroke key;
} TuiHint;

typedef struct {
	enum TuiBorder border;
	const char *face;	/* Background and text. */
	const char *border_face;
	const char *title;	/* In the top border, on the left. */
	const char *badge;	/* In the top border, on the right. */
	const TuiHint *hints;	/* In the bottom border, `nhints' of them. */
	size_t nhints;
	int pad_x, pad_y;	/* Inside the border. */
} TuiPanel;

/* Paints a custom node into `r'. */
typedef void (*TuiPaint) (TuiRect r, void *data);

/*--------------------------------------------------------------------------
 * The frame.
 *--------------------------------------------------------------------------*/

/* Start a frame: forget the regions and nodes of the last one. The
   redisplay calls it before drawing the windows. */
void tui_frame_begin (void);

/* An overlay is drawn over the windows at the end of every redisplay,
   until it is removed: as the picker is, while it runs. */
void tui_overlay_push (void (*draw) (void *data), void *data);
void tui_overlay_pop (void (*draw) (void *data), void *data);
void tui_draw_overlays (void);

/* Lay out `root' in `area' and paint it. */
void tui_render (TuiNode *root, TuiRect area);

/* Whether box drawing and block characters can be shown. */
bool tui_unicode (void);

/*--------------------------------------------------------------------------
 * Nodes. They live until the next frame begins; text is copied.
 *--------------------------------------------------------------------------*/

/* Children are listed up to a NULL. */
TuiNode *tui_column (TuiNode *child, ...);
TuiNode *tui_row (TuiNode *child, ...);
/* Add `child' to a row or column. */
void tui_add (TuiNode *parent, TuiNode *child);
/* Space between the children of a row or column. */
TuiNode *tui_gap (TuiNode *flex, int gap);

/* Share the free space of a row or column, by `flex'. */
TuiNode *tui_expanded (int flex, TuiNode *child);
/* Flexible empty space. */
TuiNode *tui_spacer (void);
/* At most `w' wide and `h' high (-1: as the child is). With no child,
   empty space of that size. */
TuiNode *tui_sized (int w, int h, TuiNode *child);
TuiNode *tui_padding (int top, int right, int bottom, int left, TuiNode *child);
/* Fill the space given with the background of `face', drawing the
   child over it. */
TuiNode *tui_fill (const char *face, TuiNode *child);

/* One line of text, cut short with an ellipsis if it does not fit. */
TuiNode *tui_text (const char *text, const char *face);
TuiNode *tui_spans (const TuiSpan *spans, size_t n);
/* Make text take all the width it is given, and align it in it:
   0 left, 1 centre, 2 right. */
TuiNode *tui_text_fill (TuiNode *text, int align);

TuiNode *tui_panel (const TuiPanel *panel, TuiNode *child);

/* A region over the child, which gets the events of `spec'. */
TuiNode *tui_clickable (const TuiRegionSpec *spec, TuiNode *child);

/* A node `w' by `h' (-1: all there is) that `paint' draws. */
TuiNode *tui_custom (int w, int h, TuiPaint paint, void *data);

/* A spinner, for something going on. */
TuiNode *tui_spinner (const char *face);

/*--------------------------------------------------------------------------
 * Scrolling.
 *--------------------------------------------------------------------------*/

/* A position in content `total' long, of which `viewport' is shown
   from `offset'. */
typedef struct {
	double total, viewport, offset;
} TuiScroll;

/* Where the thumb of a scroll bar `track' cells long goes, in cells
   (fractional: drawn to an eighth of a cell); false if all the content
   is shown, and there is no thumb. */
bool tui_scroll_thumb (const TuiScroll *s, int track, double *start, double *size);

/* The offset that puts the thumb, grabbed `grab' cells from its start,
   with the pointer at `pos'. */
double tui_scroll_drag_offset (const TuiScroll *s, int track, double pos, double grab);

/* A vertical scroll bar. Clicking the track pages towards the click;
   dragging the thumb scrolls. `scroll' is called with the new offset
   (clamped), and `data'; `spec' gives the key, and its arguments. */
typedef void (*TuiScrollTo) (double offset, void *data, long a, long b);
TuiNode *tui_scrollbar (const TuiScroll *s, TuiKey key, TuiScrollTo scroll, void *data, long a, long b);
/* Draw a scroll bar in column `x', rows [y, y + h), and record its
   region, without a node. */
void tui_paint_scrollbar (int y, int x, int h, const TuiScroll *s, TuiKey key, TuiScrollTo scroll,
			  void *data, long a, long b);

/*--------------------------------------------------------------------------
 * Painting, for custom nodes.
 *--------------------------------------------------------------------------*/

/* Draw `text' at row `y', from column `x', in no more than `w' columns;
   returns the columns used. The face's missing colours are those of
   the panel or fill being drawn in. */
int tui_put (int y, int x, int w, const char *text, const char *face, unsigned flags);
/* Fill cells with spaces in `face'. */
void tui_clear (TuiRect r, const char *face);
/* Record a region over `r'. */
void tui_region (TuiRect r, const TuiRegionSpec *spec);

/* The display width of `text', of its first `len' bytes. */
int tui_width (const char *text, size_t len);

/*--------------------------------------------------------------------------
 * The mouse.
 *--------------------------------------------------------------------------*/

/* Give a mouse keystroke to the region under the pointer, or to the
   region pressed; returns whether one took it. */
bool tui_dispatch_mouse (Keystroke key);
/* Note that the pointer moved, with no button down, to row `y', column
   `x' (or left the screen, if !`inside'); returns whether what is under
   it changed, so that hovered regions should be redrawn. */
bool tui_pointer_moved (bool inside, size_t y, size_t x);
/* Whether the region with `key' is hovered, or pressed. */
bool tui_hovered (TuiKey key);
bool tui_pressed (TuiKey key);
/* Whether a press is captured, and the pointer the region under the
   pointer, or captured, asks for. */
bool tui_captured (void);
enum TuiCursor tui_cursor (void);
/* Forget the press of a region whose data is `data', as when what it
   belongs to goes away. */
void tui_forget (void *data);
/* Whether a region is at row `y', column `x'. */
bool tui_region_at (size_t y, size_t x);

#endif
