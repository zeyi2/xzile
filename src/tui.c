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

#include "xzile.h"
#include "screen.h"
#include "tui.h"

#include <langinfo.h>

/* Two clicks this close make a double click. */
#define DOUBLE_CLICK_MS 400

/*--------------------------------------------------------------------------
 * The frame arena.
 *--------------------------------------------------------------------------*/

typedef union {
	long double ld;
	void *p;
	long long ll;
} Align;

typedef struct Chunk {
	struct Chunk *next;
	size_t used, size;
	Align data[];
} Chunk;

static Chunk *chunks;

static void *
arena_alloc (size_t n)
{
	n = (n + sizeof (Align) - 1) / sizeof (Align) * sizeof (Align);
	if (chunks == NULL || chunks->used + n > chunks->size) {
		size_t size = MAX (n, (size_t) 16384);
		Chunk *c = xmalloc (sizeof (Chunk) + size);
		c->next = chunks;
		c->used = 0;
		c->size = size;
		chunks = c;
	}
	void *p = (char *) chunks->data + chunks->used;
	chunks->used += n;
	memset (p, 0, n);
	return p;
}

static void
arena_reset (void)
{
	/* Keep the newest chunk. */
	while (chunks != NULL && chunks->next != NULL) {
		Chunk *next = chunks->next;
		chunks->next = next->next;
		free (next);
	}
	if (chunks != NULL)
		chunks->used = 0;
}

static const char *
arena_str (const char *s)
{
	if (s == NULL)
		return NULL;
	size_t n = strlen (s);
	char *p = arena_alloc (n + 1);
	memcpy (p, s, n);
	return p;
}

/*--------------------------------------------------------------------------
 * Regions and the pointer.
 *--------------------------------------------------------------------------*/

typedef struct {
	TuiRect r;
	TuiRegionSpec spec;
} HitRegion;

static HitRegion *regions;
static size_t nregions, regions_cap;

/* The pointer, when it is over the screen with no button down. */
static bool ptr_inside;
static size_t ptr_y, ptr_x;
static TuiKey hover_key;
static enum TuiCursor hover_cursor;

/* The region pressed, until the release. */
static struct {
	bool active;
	HitRegion reg;
	long state[4];
	size_t y, x;		/* Where it was pressed. */
	bool moved;
	Align payload[256 / sizeof (Align)];	/* A copy of `data', if it asked. */
} capture;

/* The last press, for double clicks. */
static TuiKey last_key;
static int64_t last_ms;
static int last_clicks;

static bool
rect_has (TuiRect r, size_t y, size_t x)
{
	return (long) y >= r.y && (long) y < r.y + r.h && (long) x >= r.x && (long) x < r.x + r.w;
}

void
tui_region (TuiRect r, const TuiRegionSpec *spec)
{
	if (r.w <= 0 || r.h <= 0)
		return;
	if (nregions == regions_cap) {
		regions_cap = regions_cap ? regions_cap * 2 : 64;
		regions = xrealloc (regions, regions_cap * sizeof (HitRegion));
	}
	regions[nregions].r = r;
	regions[nregions].spec = *spec;
	nregions++;
}

static const HitRegion *
region_under (size_t y, size_t x)
{
	for (size_t i = nregions; i-- > 0;)
		if (rect_has (regions[i].r, y, x))
			return &regions[i];
	return NULL;
}

bool
tui_region_at (size_t y, size_t x)
{
	return region_under (y, x) != NULL;
}

static bool
update_hover (void)
{
	TuiKey key = 0;
	enum TuiCursor cursor = TUI_CURSOR_DEFAULT;
	if (ptr_inside && !capture.active) {
		const HitRegion *r = region_under (ptr_y, ptr_x);
		if (r != NULL) {
			key = r->spec.key;
			cursor = r->spec.cursor;
		}
	}
	bool changed = key != hover_key || cursor != hover_cursor;
	hover_key = key;
	hover_cursor = cursor;
	return changed;
}

bool
tui_pointer_moved (bool inside, size_t y, size_t x)
{
	ptr_inside = inside;
	ptr_y = y;
	ptr_x = x;
	return update_hover ();
}

bool
tui_hovered (TuiKey key)
{
	return key != 0 && ((capture.active && capture.reg.spec.key == key) || (!capture.active && hover_key == key));
}

bool
tui_pressed (TuiKey key)
{
	return key != 0 && capture.active && capture.reg.spec.key == key;
}

bool
tui_captured (void)
{
	return capture.active;
}

void
tui_forget (void *data)
{
	if (capture.active && capture.reg.spec.data == data)
		capture.active = false;
}

enum TuiCursor
tui_cursor (void)
{
	return capture.active ? capture.reg.spec.cursor : hover_cursor;
}

static bool
send (const HitRegion *reg, enum TuiEventKind kind, size_t y, size_t x, int clicks, int wheel, long *state)
{
	if (reg->spec.handler == NULL)
		return false;
	TuiEvent ev;
	memset (&ev, 0, sizeof ev);
	ev.kind = kind;
	ev.y = (int) y - reg->r.y;
	ev.x = (int) x - reg->r.x;
	ev.clicks = clicks;
	ev.wheel = wheel;
	ev.moved = capture.moved;
	ev.rect = reg->r;
	ev.state = state;
	ev.a = reg->spec.a;
	ev.b = reg->spec.b;
	return reg->spec.handler (&ev, reg->spec.data);
}

static bool
press (size_t y, size_t x)
{
	capture.active = false;
	for (size_t i = nregions; i-- > 0;) {
		HitRegion reg = regions[i];
		if (!rect_has (reg.r, y, x))
			continue;
		int64_t now = monotonic_ms ();
		int clicks = reg.spec.key != 0 && reg.spec.key == last_key && now - last_ms <= DOUBLE_CLICK_MS
			? last_clicks + 1 : 1;
		/* The handler may draw, starting a new frame: copy first. */
		if (reg.spec.data_size > 0) {
			memcpy (capture.payload, reg.spec.data, MIN (reg.spec.data_size, sizeof capture.payload));
			reg.spec.data = capture.payload;
		}
		capture.reg = reg;
		capture.y = y;
		capture.x = x;
		capture.moved = false;
		memset (capture.state, 0, sizeof capture.state);
		capture.active = true;
		bool handled = send (&capture.reg, TUI_PRESS, y, x, clicks, 0, capture.state);
		if (handled || reg.spec.opaque) {
			last_key = reg.spec.key;
			last_ms = now;
			last_clicks = clicks;
			capture.active = handled && capture.active;
			update_hover ();
			return true;
		}
		capture.active = false;
	}
	last_key = 0;
	return false;
}

bool
tui_dispatch_mouse (Keystroke key)
{
	size_t y, x;
	if (!term_mouse_position (&y, &x))
		return false;
	switch (key) {
	case KBD_MOUSE_1:
		return press (y, x);
	case KBD_DRAG_MOUSE_1:
		if (!capture.active)
			return false;
		if (y != capture.y || x != capture.x)
			capture.moved = true;
		send (&capture.reg, TUI_DRAG, y, x, 0, 0, capture.state);
		return true;
	case KBD_UP_MOUSE_1: {
		if (!capture.active)
			return false;
		HitRegion reg = capture.reg;
		long state[4];
		memcpy (state, capture.state, sizeof state);
		capture.active = false;
		send (&reg, TUI_RELEASE, y, x, 0, 0, state);
		ptr_y = y;
		ptr_x = x;
		update_hover ();
		return true;
	}
	case KBD_WHEEL_UP:
	case KBD_WHEEL_DOWN:
		for (size_t i = nregions; i-- > 0;) {
			HitRegion reg = regions[i];
			if (!rect_has (reg.r, y, x))
				continue;
			long state[4] = { 0 };
			if (send (&reg, TUI_WHEEL, y, x, 0, key == KBD_WHEEL_UP ? -1 : 1, state) || reg.spec.opaque)
				return true;
		}
		return false;
	default:
		return false;
	}
}

/*--------------------------------------------------------------------------
 * Styles and painting.
 *--------------------------------------------------------------------------*/

bool
tui_unicode (void)
{
	static int unicode = -1;
	if (term_get_capabilities ()->gui)
		return true;
	if (unicode < 0) {
		const char *cs = nl_langinfo (CODESET);
		unicode = cs != NULL && (strcmp (cs, "UTF-8") == 0 || strcmp (cs, "utf8") == 0);
	}
	return unicode;
}

/* The colours a panel or fill gives what is drawn in it. */
typedef struct {
	bool has_fg, has_bg;
	int fg, bg;
} Inherit;

static Inherit inherit;

/* The style of `face' (NULL: no face) with `flags', drawn in what
   `inherit' gives. As every face inherits the default face, colours the
   same as the default face's count as left out. */
static void
style_for (const char *face, unsigned flags, TerminalStyle *st)
{
	TerminalStyle dflt;
	bool have_dflt = resolve_terminal_style (FACE_DEFAULT, &dflt);
	if (!have_dflt)
		memset (&dflt, 0, sizeof dflt);
	bool own = face != NULL && resolve_terminal_style (face, st);
	if (!own)
		*st = dflt;
	bool own_fg = own && st->has_foreground &&
		!(have_dflt && dflt.has_foreground && dflt.foreground == st->foreground);
	bool own_bg = own && st->has_background &&
		!(have_dflt && dflt.has_background && dflt.background == st->background);
	if (!own_fg && inherit.has_fg) {
		st->has_foreground = true;
		st->foreground = inherit.fg;
	}
	if (!own_bg && inherit.has_bg) {
		st->has_background = true;
		st->background = inherit.bg;
	}
	if (flags & TUI_BOLD)
		st->bold = true;
	if (flags & TUI_UNDERLINE)
		st->underline = true;
	if (flags & TUI_REVERSE)
		st->reverse = !st->reverse;
}

static Inherit
push_inherit (const char *face)
{
	Inherit saved = inherit;
	TerminalStyle st;
	if (face != NULL) {
		style_for (face, 0, &st);
		if (st.has_foreground) {
			inherit.has_fg = true;
			inherit.fg = st.foreground;
		}
		if (st.has_background) {
			inherit.has_bg = true;
			inherit.bg = st.background;
		}
	}
	return saved;
}

int
tui_width (const char *text, size_t len)
{
	int w = 0;
	for (size_t i = 0; i < len;) {
		size_t cl;
		uint32_t c = utf8_decode (text, len, i, &cl);
		w += c < 0x20 ? 2 : utf8_char_display_width (c);
		i += cl ? cl : 1;
	}
	return w;
}

static bool
on_screen (int y, int x)
{
	return y >= 0 && x >= 0 && (size_t) y < scr_rows && (size_t) x < scr_cols;
}

int
tui_put (int y, int x, int w, const char *text, const char *face, unsigned flags)
{
	if (w <= 0 || !on_screen (y, x))
		return 0;
	w = MIN (w, (int) scr_cols - x);
	TerminalStyle st;
	style_for (face, flags, &st);
	term_apply_style (&st);
	term_move ((size_t) y, (size_t) x);
	size_t len = strlen (text);
	int col = 0;
	for (size_t i = 0; i < len;) {
		size_t cl;
		uint32_t c = utf8_decode (text, len, i, &cl);
		cl = cl ? cl : 1;
		int cw = c < 0x20 ? 2 : utf8_char_display_width (c);
		if (col + cw > w)
			break;
		if (c == '\n' || c == '\t')
			term_addch (' ');
		else
			term_addnstr (text + i, cl);
		col += cw;
		i += cl;
	}
	if (flags & TUI_DIM)
		for (int k = 0; k < col; k++)
			scr_cells[(size_t) y * scr_cols + (size_t) (x + k)].attr.flags |= ATTR_DIM;
	term_attrset (FONT_NORMAL);
	return col;
}

void
tui_clear (TuiRect r, const char *face)
{
	TerminalStyle st;
	style_for (face, 0, &st);
	term_apply_style (&st);
	for (int y = MAX (r.y, 0); y < r.y + r.h && y < (int) scr_rows; y++) {
		if (r.x >= (int) scr_cols)
			break;
		term_move ((size_t) y, (size_t) MAX (r.x, 0));
		for (int x = MAX (r.x, 0); x < r.x + r.w && x < (int) scr_cols; x++)
			term_addch (' ');
	}
	term_attrset (FONT_NORMAL);
}

/*--------------------------------------------------------------------------
 * Nodes.
 *--------------------------------------------------------------------------*/

enum Kind {
	K_COLUMN, K_ROW, K_EXPANDED, K_SIZED, K_PADDING, K_FILL, K_TEXT, K_PANEL,
	K_CLICK, K_CUSTOM, K_SCROLLBAR,
};

typedef struct {
	int minw, maxw, minh, maxh;
} Cons;

struct TuiNode {
	enum Kind kind;
	TuiNode *child, *last, *next;
	int x, y, w, h;		/* Laid out: position in the parent, size. */
	int flex, gap;
	int sw, sh;		/* K_SIZED, K_CUSTOM. */
	int pad[4];		/* K_PADDING: top, right, bottom, left. */
	const char *face;	/* K_FILL. */
	TuiSpan *spans;		/* K_TEXT. */
	size_t nspans;
	bool fill_width;
	int align;
	TuiPanel panel;		/* K_PANEL. */
	TuiRegionSpec region;	/* K_CLICK. */
	TuiPaint paint;		/* K_CUSTOM. */
	void *data;
	TuiScroll scroll;	/* K_SCROLLBAR. */
	TuiScrollTo scroll_to;
	long a, b;
};

static TuiNode *
node_new (enum Kind kind)
{
	TuiNode *n = arena_alloc (sizeof (TuiNode));
	n->kind = kind;
	return n;
}

void
tui_add (TuiNode *parent, TuiNode *child)
{
	if (child == NULL)
		return;
	child->next = NULL;
	if (parent->last != NULL)
		parent->last->next = child;
	else
		parent->child = child;
	parent->last = child;
}

static TuiNode *
flex_v (enum Kind kind, TuiNode *child, va_list ap)
{
	TuiNode *n = node_new (kind);
	for (; child != NULL; child = va_arg (ap, TuiNode *))
		tui_add (n, child);
	return n;
}

TuiNode *
tui_column (TuiNode *child, ...)
{
	va_list ap;
	va_start (ap, child);
	TuiNode *n = flex_v (K_COLUMN, child, ap);
	va_end (ap);
	return n;
}

TuiNode *
tui_row (TuiNode *child, ...)
{
	va_list ap;
	va_start (ap, child);
	TuiNode *n = flex_v (K_ROW, child, ap);
	va_end (ap);
	return n;
}

TuiNode *
tui_gap (TuiNode *flex, int gap)
{
	flex->gap = gap;
	return flex;
}

TuiNode *
tui_expanded (int flex, TuiNode *child)
{
	TuiNode *n = node_new (K_EXPANDED);
	n->flex = MAX (flex, 1);
	tui_add (n, child);
	return n;
}

TuiNode *
tui_spacer (void)
{
	return tui_expanded (1, NULL);
}

TuiNode *
tui_sized (int w, int h, TuiNode *child)
{
	TuiNode *n = node_new (K_SIZED);
	n->sw = w;
	n->sh = h;
	tui_add (n, child);
	return n;
}

TuiNode *
tui_padding (int top, int right, int bottom, int left, TuiNode *child)
{
	TuiNode *n = node_new (K_PADDING);
	n->pad[0] = top;
	n->pad[1] = right;
	n->pad[2] = bottom;
	n->pad[3] = left;
	tui_add (n, child);
	return n;
}

TuiNode *
tui_fill (const char *face, TuiNode *child)
{
	TuiNode *n = node_new (K_FILL);
	n->face = arena_str (face);
	tui_add (n, child);
	return n;
}

TuiNode *
tui_spans (const TuiSpan *spans, size_t n)
{
	TuiNode *t = node_new (K_TEXT);
	t->spans = arena_alloc ((n ? n : 1) * sizeof (TuiSpan));
	for (size_t i = 0; i < n; i++) {
		t->spans[i].text = arena_str (spans[i].text != NULL ? spans[i].text : "");
		t->spans[i].face = arena_str (spans[i].face);
		t->spans[i].flags = spans[i].flags;
	}
	t->nspans = n;
	return t;
}

TuiNode *
tui_text (const char *text, const char *face)
{
	TuiSpan s = { text, face, 0 };
	return tui_spans (&s, 1);
}

TuiNode *
tui_text_fill (TuiNode *text, int align)
{
	text->fill_width = true;
	text->align = align;
	return text;
}

TuiNode *
tui_panel (const TuiPanel *panel, TuiNode *child)
{
	TuiNode *n = node_new (K_PANEL);
	n->panel = *panel;
	n->panel.face = arena_str (panel->face);
	n->panel.border_face = arena_str (panel->border_face);
	n->panel.title = arena_str (panel->title);
	n->panel.badge = arena_str (panel->badge);
	if (panel->nhints > 0) {
		TuiHint *h = arena_alloc (panel->nhints * sizeof (TuiHint));
		for (size_t i = 0; i < panel->nhints; i++) {
			h[i].keys = arena_str (panel->hints[i].keys);
			h[i].label = arena_str (panel->hints[i].label);
			h[i].key = panel->hints[i].key;
		}
		n->panel.hints = h;
	}
	tui_add (n, child);
	return n;
}

TuiNode *
tui_clickable (const TuiRegionSpec *spec, TuiNode *child)
{
	TuiNode *n = node_new (K_CLICK);
	n->region = *spec;
	tui_add (n, child);
	return n;
}

TuiNode *
tui_custom (int w, int h, TuiPaint paint, void *data)
{
	TuiNode *n = node_new (K_CUSTOM);
	n->sw = w;
	n->sh = h;
	n->paint = paint;
	n->data = data;
	return n;
}

TuiNode *
tui_spinner (const char *face)
{
	static const char *frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
	static const char *ascii[] = { "|", "/", "-", "\\" };
	int64_t t = monotonic_ms () / 80;
	const char *f = tui_unicode () ? frames[t % 10] : ascii[t % 4];
	return tui_text (f, face);
}

TuiNode *
tui_scrollbar (const TuiScroll *s, TuiKey key, TuiScrollTo scroll, void *data, long a, long b)
{
	TuiNode *n = node_new (K_SCROLLBAR);
	n->scroll = *s;
	n->region.key = key;
	n->scroll_to = scroll;
	n->data = data;
	n->a = a;
	n->b = b;
	return n;
}

/*--------------------------------------------------------------------------
 * Layout.
 *--------------------------------------------------------------------------*/

/* floor (), without the maths library. */
static int
ifloor (double v)
{
	int i = (int) v;
	return i > v ? i - 1 : i;
}

static int
clampi (int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void layout (TuiNode *n, Cons c);

static int
spans_width (const TuiNode *n)
{
	int w = 0;
	for (size_t i = 0; i < n->nspans; i++)
		w += tui_width (n->spans[i].text, strlen (n->spans[i].text));
	return w;
}

static Cons
tight (int w, int h)
{
	Cons c = { w, w, h, h };
	return c;
}

/* Lay out the children of a row or column. */
static void
layout_flex (TuiNode *n, Cons c, bool column)
{
	int cross = column ? c.maxw : 0;
	int avail = column ? c.maxh : c.maxw;
	int nchildren = 0, total_flex = 0, used = 0;
	for (TuiNode *k = n->child; k != NULL; k = k->next) {
		nchildren++;
		if (k->kind == K_EXPANDED)
			total_flex += k->flex;
	}
	used = n->gap * MAX (nchildren - 1, 0);
	bool cross_tight = column ? true : c.minh == c.maxh;

	/* The inflexible children first, then the flexible ones share what
	   is left. */
	for (TuiNode *k = n->child; k != NULL; k = k->next) {
		if (k->kind == K_EXPANDED)
			continue;
		int left = MAX (avail - used, 0);
		Cons kc = column
			? (Cons) { cross, cross, 0, left }
			: (Cons) { 0, left, cross_tight ? c.maxh : 0, c.maxh };
		layout (k, kc);
		used += column ? k->h : k->w;
		if (!column)
			cross = MAX (cross, k->h);
	}
	int free_space = avail >= TUI_INF ? 0 : MAX (avail - used, 0);
	int given = 0, seen = 0;
	for (TuiNode *k = n->child; k != NULL; k = k->next) {
		if (k->kind != K_EXPANDED)
			continue;
		seen += k->flex;
		int share = (int) ((long) free_space * seen / total_flex) - given;
		given += share;
		layout (k, column ? tight (cross, share) : (Cons) { share, share, cross_tight ? c.maxh : 0, c.maxh });
		if (!column)
			cross = MAX (cross, k->h);
	}
	used += given;
	if (!column && cross_tight)
		cross = c.maxh;

	/* Place them, stretched across. */
	int pos = 0;
	for (TuiNode *k = n->child; k != NULL; k = k->next) {
		if (column) {
			k->x = 0;
			k->y = pos;
			k->w = cross;
			pos += k->h + n->gap;
		} else {
			k->x = pos;
			k->y = 0;
			k->h = cross;
			pos += k->w + n->gap;
		}
	}
	if (column) {
		n->w = clampi (cross, c.minw, c.maxw);
		n->h = clampi (total_flex > 0 ? avail : used, c.minh, c.maxh);
	} else {
		n->w = clampi (total_flex > 0 ? avail : used, c.minw, c.maxw);
		n->h = clampi (cross, c.minh, c.maxh);
	}
}

static void
layout (TuiNode *n, Cons c)
{
	TuiNode *k = n->child;
	switch (n->kind) {
	case K_COLUMN:
	case K_ROW:
		layout_flex (n, c, n->kind == K_COLUMN);
		return;
	case K_EXPANDED:
	case K_CLICK:
		if (k != NULL) {
			layout (k, c);
			n->w = clampi (k->w, c.minw, c.maxw);
			n->h = clampi (k->h, c.minh, c.maxh);
			k->w = n->w;
			k->h = n->h;
		} else {
			n->w = c.minw;
			n->h = c.minh;
		}
		return;
	case K_SIZED: {
		int w = n->sw >= 0 ? clampi (n->sw, c.minw, c.maxw) : -1;
		int h = n->sh >= 0 ? clampi (n->sh, c.minh, c.maxh) : -1;
		if (k != NULL) {
			layout (k, (Cons) { w >= 0 ? w : c.minw, w >= 0 ? w : c.maxw,
					    h >= 0 ? h : c.minh, h >= 0 ? h : c.maxh });
			n->w = w >= 0 ? w : k->w;
			n->h = h >= 0 ? h : k->h;
		} else {
			n->w = w >= 0 ? w : c.minw;
			n->h = h >= 0 ? h : c.minh;
		}
		return;
	}
	case K_PADDING: {
		int dw = n->pad[1] + n->pad[3], dh = n->pad[0] + n->pad[2];
		Cons kc = { MAX (c.minw - dw, 0), MAX (c.maxw - dw, 0), MAX (c.minh - dh, 0), MAX (c.maxh - dh, 0) };
		if (k != NULL) {
			layout (k, kc);
			k->x = n->pad[3];
			k->y = n->pad[0];
		}
		n->w = clampi ((k != NULL ? k->w : 0) + dw, c.minw, c.maxw);
		n->h = clampi ((k != NULL ? k->h : 0) + dh, c.minh, c.maxh);
		return;
	}
	case K_FILL:
		/* As big as the child, or all there is with none. */
		if (k != NULL) {
			layout (k, c);
			n->w = k->w;
			n->h = k->h;
		} else {
			n->w = c.maxw < TUI_INF ? c.maxw : c.minw;
			n->h = c.maxh < TUI_INF ? c.maxh : c.minh;
		}
		return;
	case K_TEXT:
		n->w = clampi (n->fill_width && c.maxw < TUI_INF ? c.maxw : spans_width (n), c.minw, c.maxw);
		n->h = clampi (1, c.minh, c.maxh);
		return;
	case K_PANEL: {
		int b = n->panel.border != TUI_BORDER_NONE ? 1 : 0;
		int dw = 2 * (b + n->panel.pad_x), dh = 2 * (b + n->panel.pad_y);
		n->w = c.maxw < TUI_INF ? c.maxw : c.minw;
		if (k != NULL) {
			Cons kc = { MAX (n->w - dw, 0), MAX (n->w - dw, 0), MAX (c.minh - dh, 0), MAX (c.maxh - dh, 0) };
			layout (k, kc);
			k->x = b + n->panel.pad_x;
			k->y = b + n->panel.pad_y;
		}
		n->h = clampi ((k != NULL ? k->h : 0) + dh, c.minh, c.maxh);
		return;
	}
	case K_CUSTOM:
		n->w = n->sw >= 0 ? clampi (n->sw, c.minw, c.maxw) : (c.maxw < TUI_INF ? c.maxw : c.minw);
		n->h = n->sh >= 0 ? clampi (n->sh, c.minh, c.maxh) : (c.maxh < TUI_INF ? c.maxh : c.minh);
		return;
	case K_SCROLLBAR:
		n->w = clampi (1, c.minw, c.maxw);
		n->h = c.maxh < TUI_INF ? c.maxh : c.minh;
		return;
	}
}

/*--------------------------------------------------------------------------
 * Scroll bars.
 *--------------------------------------------------------------------------*/

bool
tui_scroll_thumb (const TuiScroll *s, int track, double *start, double *size)
{
	if (track <= 0 || s->total <= s->viewport || s->viewport <= 0)
		return false;
	double range = s->total - s->viewport;
	double frac = s->offset / range;
	frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
	*size = MAX (1.0, track * (s->viewport / s->total));
	*start = (track - *size) * frac;
	return true;
}

double
tui_scroll_drag_offset (const TuiScroll *s, int track, double pos, double grab)
{
	double start, size;
	if (!tui_scroll_thumb (s, track, &start, &size) || track - size <= 0)
		return s->offset;
	double frac = (pos - grab) / (track - size);
	frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
	return frac * (s->total - s->viewport);
}

typedef struct {
	TuiScroll s;
	int track;
	TuiScrollTo scroll_to;
	void *data;
} ScrollbarData;

static bool
scrollbar_event (const TuiEvent *ev, void *data)
{
	ScrollbarData *d = data;
	double start, size;
	bool has = tui_scroll_thumb (&d->s, d->track, &start, &size);
	double range = d->s.total - d->s.viewport;
	switch (ev->kind) {
	case TUI_PRESS:
		if (!has)
			return true;
		if (ev->y + 0.5 >= start && ev->y + 0.5 <= start + size) {
			/* Grab the thumb where it was pressed. */
			ev->state[0] = 1;
			ev->state[1] = (long) ((ev->y + 0.5 - start) * 1000 + 0.5);
		} else {
			double to = d->s.offset + (ev->y + 0.5 < start ? -1 : 1) * MAX (d->s.viewport - 1, 1);
			d->scroll_to (to < 0 ? 0 : to > range ? range : to, d->data, ev->a, ev->b);
		}
		return true;
	case TUI_DRAG:
		if (has && ev->state[0])
			d->scroll_to (tui_scroll_drag_offset (&d->s, d->track, ev->y + 0.5, ev->state[1] / 1000.0),
				      d->data, ev->a, ev->b);
		return true;
	case TUI_RELEASE:
		return true;
	case TUI_WHEEL: {
		double to = d->s.offset + ev->wheel * 3;
		d->scroll_to (to < 0 ? 0 : to > range ? range : to, d->data, ev->a, ev->b);
		return true;
	}
	}
	return false;
}

void
tui_paint_scrollbar (int y, int x, int h, const TuiScroll *s, TuiKey key, TuiScrollTo scroll,
		     void *data, long a, long b)
{
	if (h <= 0)
		return;
	double start = 0, size = 0;
	bool has = tui_scroll_thumb (s, h, &start, &size);
	bool hot = has && tui_hovered (key);

	TerminalStyle track, thumb;
	style_for (FACE_TUI_SCROLLBAR_TRACK, 0, &track);
	style_for (hot ? FACE_TUI_SCROLLBAR_THUMB_HOVER : FACE_TUI_SCROLLBAR_THUMB, 0, &thumb);
	const TerminalCapabilities *caps = term_get_capabilities ();
	bool blocks = tui_unicode () && caps->has_colors && track.has_background && thumb.has_foreground;

	for (int c = 0; c < h; c++) {
		if (!on_screen (y + c, x))
			continue;
		if (!has) {
			tui_put (y + c, x, 1, " ", NULL, 0);
			continue;
		}
		if (!blocks) {
			/* Whole cells: a solid thumb on the track. */
			bool in = c + 0.5 >= start && c + 0.5 < start + size;
			if (in)
				tui_put (y + c, x, 1, tui_unicode () ? "█" : "#",
					 hot ? FACE_TUI_SCROLLBAR_THUMB_HOVER : FACE_TUI_SCROLLBAR_THUMB, 0);
			else
				tui_put (y + c, x, 1, " ", FACE_TUI_SCROLLBAR_TRACK, 0);
			continue;
		}
		/* Eighths of a cell, as Amp draws them: the cell where the thumb
		   starts shows its lower part, the cell where it ends its upper
		   part (the block reversed). Whole cells are spaces, which leave
		   no gaps between lines as block characters can. */
		static const char *eighths[] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇" };
		double end = start + size;
		const char *ch = " ";
		bool thumb_bg = false, reverse = false;
		if (c == ifloor (start) && start - c > 0) {
			int m = ifloor ((1 - (start - c)) * 8);
			if (m >= 1 && m <= 7)
				ch = eighths[m - 1];
			else
				thumb_bg = m >= 8;
		} else if (c == ifloor (end) && end - c > 0) {
			int m = ifloor ((1 - (end - c)) * 8);
			if (m >= 1 && m <= 7) {
				ch = eighths[m - 1];
				reverse = true;
			} else
				thumb_bg = m < 1;
		} else if (c >= start && c < end)
			thumb_bg = true;
		TerminalStyle st = track;
		st.has_foreground = true;
		st.foreground = thumb.foreground;
		if (thumb_bg)
			st.background = thumb.foreground;
		st.reverse = reverse;
		st.bold = st.underline = st.italic = st.strike = false;
		term_apply_style (&st);
		term_move ((size_t) (y + c), (size_t) x);
		term_addstr (ch);
		term_attrset (FONT_NORMAL);
	}

	ScrollbarData *d = arena_alloc (sizeof *d);
	d->s = *s;
	d->track = h;
	d->scroll_to = scroll;
	d->data = data;
	TuiRegionSpec spec = { key, scrollbar_event, d, sizeof *d, a, b,
			       has ? TUI_CURSOR_POINTER : TUI_CURSOR_DEFAULT, true };
	tui_region ((TuiRect) { x, y, 1, h }, &spec);
}

/*--------------------------------------------------------------------------
 * Painting.
 *--------------------------------------------------------------------------*/

static void
paint_spans (const TuiNode *n, int y, int x)
{
	int w = n->w, natural = spans_width (n);
	int pos = 0;
	if (n->fill_width && natural < w) {
		int extra = w - natural;
		int lead = n->align == 1 ? extra / 2 : n->align == 2 ? extra : 0;
		const char *face = n->nspans > 0 ? n->spans[0].face : NULL;
		for (int i = 0; i < lead; i++)
			pos += tui_put (y, x + pos, 1, " ", face, 0);
	}
	bool cut = natural > w;
	const char *ellipsis = tui_unicode () ? "…" : "~";
	int limit = cut ? w - 1 : w;
	const char *last_face = NULL;
	unsigned last_flags = 0;
	for (size_t i = 0; i < n->nspans && pos < limit; i++) {
		pos += tui_put (y, x + pos, limit - pos, n->spans[i].text, n->spans[i].face, n->spans[i].flags);
		last_face = n->spans[i].face;
		last_flags = n->spans[i].flags;
	}
	if (cut && w > 0) {
		/* A wide character may have left a gap before the ellipsis. */
		while (pos < w - 1)
			pos += tui_put (y, x + pos, 1, " ", last_face, last_flags);
		pos += tui_put (y, x + pos, 1, ellipsis, last_face, last_flags);
	}
	while (pos < w)
		pos += MAX (tui_put (y, x + pos, 1, " ", n->nspans > 0 ? n->spans[n->nspans - 1].face : NULL, 0), 1);
}

static bool
hint_event (const TuiEvent *ev, void *data)
{
	(void) data;
	if (ev->kind != TUI_PRESS)
		return false;
	pushkey ((Keystroke) ev->a);
	return true;
}

static void
paint_border (const TuiNode *n, int y, int x)
{
	const TuiPanel *p = &n->panel;
	bool u = tui_unicode ();
	bool round = p->border == TUI_BORDER_ROUNDED;
	const char *h = u ? "─" : "-", *v = u ? "│" : "|";
	const char *tl = u ? (round ? "╭" : "┌") : "+", *tr = u ? (round ? "╮" : "┐") : "+";
	const char *bl = u ? (round ? "╰" : "└") : "+", *br = u ? (round ? "╯" : "┘") : "+";
	const char *bf = p->border_face != NULL ? p->border_face : FACE_TUI_BORDER;
	int w = n->w, ht = n->h;
	if (w < 2 || ht < 2)
		return;
	for (int i = 1; i < w - 1; i++) {
		tui_put (y, x + i, 1, h, bf, 0);
		tui_put (y + ht - 1, x + i, 1, h, bf, 0);
	}
	for (int j = 1; j < ht - 1; j++) {
		tui_put (y + j, x, 1, v, bf, 0);
		tui_put (y + j, x + w - 1, 1, v, bf, 0);
	}
	tui_put (y, x, 1, tl, bf, 0);
	tui_put (y, x + w - 1, 1, tr, bf, 0);
	tui_put (y + ht - 1, x, 1, bl, bf, 0);
	tui_put (y + ht - 1, x + w - 1, 1, br, bf, 0);

	/* The badge on the right of the top border, then the title on the
	   left in what room is left. */
	int right = w - 2;
	if (p->badge != NULL && p->badge[0] != '\0') {
		int bw = tui_width (p->badge, strlen (p->badge)) + 2;
		if (bw + 4 < right) {
			right -= bw;
			tui_put (y, x + right, 1, " ", bf, 0);
			tui_put (y, x + right + 1, bw - 2, p->badge, FACE_TUI_MUTED, 0);
			tui_put (y, x + right + bw - 1, 1, " ", bf, 0);
			right--;
		}
	}
	if (p->title != NULL && p->title[0] != '\0' && right > 4) {
		tui_put (y, x + 2, 1, " ", bf, 0);
		int tw = tui_put (y, x + 3, right - 4, p->title, FACE_TUI_TITLE, TUI_BOLD);
		tui_put (y, x + 3 + tw, 1, " ", bf, 0);
	}

	/* Key hints in the bottom border, each clickable, as many as fit. */
	int col = 2, room = w - 4;
	for (size_t i = 0; i < p->nhints; i++) {
		const TuiHint *hint = &p->hints[i];
		int kw = tui_width (hint->keys, strlen (hint->keys));
		int lw = tui_width (hint->label, strlen (hint->label));
		int need = kw + 1 + lw + 2;
		if (col - 2 + need > room)
			break;
		TuiKey key = TUI_KEY (0x7e, (uintptr_t) hint->key);
		bool hot = tui_hovered (key);
		int y1 = y + ht - 1;
		tui_put (y1, x + col, 1, " ", bf, 0);
		tui_put (y1, x + col + 1, kw, hint->keys, FACE_TUI_KEY, hot ? TUI_UNDERLINE : 0);
		tui_put (y1, x + col + 1 + kw, 1, " ", FACE_TUI_MUTED, 0);
		tui_put (y1, x + col + 2 + kw, lw, hint->label, hot ? FACE_TUI_TITLE : FACE_TUI_MUTED, 0);
		tui_put (y1, x + col + 2 + kw + lw, 1, " ", bf, 0);
		TuiRegionSpec spec = { key, hint_event, NULL, 0, (long) hint->key, 0, TUI_CURSOR_POINTER, true };
		tui_region ((TuiRect) { x + col + 1, y1, kw + 1 + lw, 1 }, &spec);
		col += need;
	}
}

static void
paint (TuiNode *n, int x, int y)
{
	if (n->w <= 0 || n->h <= 0)
		return;
	switch (n->kind) {
	case K_TEXT:
		paint_spans (n, y, x);
		return;
	case K_FILL: {
		tui_clear ((TuiRect) { x, y, n->w, n->h }, n->face);
		Inherit saved = push_inherit (n->face);
		if (n->child != NULL)
			paint (n->child, x + n->child->x, y + n->child->y);
		inherit = saved;
		return;
	}
	case K_PANEL: {
		tui_clear ((TuiRect) { x, y, n->w, n->h }, n->panel.face);
		Inherit saved = push_inherit (n->panel.face);
		if (n->panel.border != TUI_BORDER_NONE)
			paint_border (n, y, x);
		if (n->child != NULL)
			paint (n->child, x + n->child->x, y + n->child->y);
		inherit = saved;
		return;
	}
	case K_CLICK:
		/* Below what the child records, so that clickable children of a
		   clickable node are on top. */
		tui_region ((TuiRect) { x, y, n->w, n->h }, &n->region);
		if (n->child != NULL)
			paint (n->child, x, y);
		return;
	case K_CUSTOM:
		if (n->paint != NULL)
			n->paint ((TuiRect) { x, y, n->w, n->h }, n->data);
		return;
	case K_SCROLLBAR:
		tui_paint_scrollbar (y, x, n->h, &n->scroll, n->region.key, n->scroll_to, n->data, n->a, n->b);
		return;
	default:
		for (TuiNode *k = n->child; k != NULL; k = k->next)
			paint (k, x + k->x, y + k->y);
		return;
	}
}

void
tui_render (TuiNode *root, TuiRect area)
{
	if (root == NULL)
		return;
	layout (root, tight (MAX (area.w, 0), MAX (area.h, 0)));
	inherit = (Inherit) { false, false, 0, 0 };
	paint (root, area.x, area.y);
	term_attrset (FONT_NORMAL);
}

/*--------------------------------------------------------------------------
 * Frames and overlays.
 *--------------------------------------------------------------------------*/

typedef struct {
	void (*draw) (void *data);
	void *data;
} Overlay;

static Overlay overlays[8];
static size_t noverlays;

void
tui_frame_begin (void)
{
	arena_reset ();
	nregions = 0;
}

void
tui_overlay_push (void (*draw) (void *data), void *data)
{
	if (noverlays < sizeof overlays / sizeof overlays[0])
		overlays[noverlays++] = (Overlay) { draw, data };
}

void
tui_overlay_pop (void (*draw) (void *data), void *data)
{
	for (size_t i = noverlays; i-- > 0;)
		if (overlays[i].draw == draw && overlays[i].data == data) {
			memmove (overlays + i, overlays + i + 1, (noverlays - i - 1) * sizeof (Overlay));
			noverlays--;
			return;
		}
}

void
tui_draw_overlays (void)
{
	for (size_t i = 0; i < noverlays; i++)
		overlays[i].draw (overlays[i].data);
	/* What is under the pointer may have changed. */
	update_hover ();
}
