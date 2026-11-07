/* Graphical frontend using SDL2 and SDL2_ttf

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

/*
 * This file replaces term.c in xzile-gui. It implements the same term_*
 * interface: the editor draws into the shared cell grid (screen.c), and
 * term_refresh paints that grid into a window with anti-aliased fonts,
 * truecolor, and a few graphical touches (padding, line spacing, a
 * solid cursor, seamless borders). Keyboard and mouse events become
 * keystrokes.
 */

#include "xzile.h"
#include "screen.h"
#include "tui.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include "../modes/gui_preview.h"

#if !SDL_VERSION_ATLEAST(2, 0, 18) || !SDL_TTF_VERSION_ATLEAST(2, 0, 18)
#error "xzile-gui needs SDL and SDL_ttf 2.0.18 or later"
#endif

/* Padding around the text grid, in logical pixels. */
#define PADDING 10
/* Line height as a multiple of the font size. */
#define LINE_SPACING 1.45
#define DEFAULT_FONT_SIZE 14.0
#define MIN_FONT_SIZE 6.0
#define MAX_FONT_SIZE 72.0
#define BELL_MS 120
#define INITIAL_COLS 100
#define INITIAL_ROWS 36

enum {
	STYLE_BOLD = 1,
	STYLE_ITALIC = 2,
	NSTYLES = 4,
};

static TerminalCapabilities capabilities = {
	.has_colors = true,
	.supports_default_colors = true,
	.supports_reverse = true,
	.supports_underline = true,
	.color_count = 256,
	.truecolor = true,
	.gui = true,
};

static bool initialized = false;
static SDL_Window *window;
static SDL_Renderer *renderer;
static float scale = 1;		/* Pixels per logical pixel. */
static int out_w, out_h;	/* Renderer size in pixels. */

/* What was last painted, so the window can be repainted at any time. */
static Cell *phys;
static size_t phys_rows, phys_cols;
static size_t phys_y, phys_x;
static bool phys_cursor_visible;

static bool focused = true;
static bool pending_resize = false;
static int64_t bell_until = 0;
static char *title;


/*--------------------------------------------------------------------------
 * Fonts.
 *--------------------------------------------------------------------------*/

typedef struct {
	char *path;
	long index;
} FontFile;

static TTF_Font *fonts[NSTYLES];
static int cell_w = 8, cell_h = 16, text_y, ascent, pad;
static double font_size = DEFAULT_FONT_SIZE;
static double zoom = 0;		/* Added to the configured size. */
static char *font_var;		/* The values of gui-font and gui-font-size */
static char *font_size_var;	/* in use. */

typedef struct {
	char *path;
	long index;
	TTF_Font *font;
} Fallback;

static Fallback *fallbacks;
static size_t nfallbacks;

static void
font_file_free (FontFile *f)
{
	free (f->path);
	f->path = NULL;
}

static FontFile
font_file (const char *path, long index)
{
	FontFile f = { NULL, index };
	if (path[0] == '~') {
		char *home = get_home_dir ();
		f.path = xasprintf ("%s%s", home, path + 1);
		free (home);
	} else
		f.path = xstrdup (path);
	if (!file_is_regular (f.path))
		font_file_free (&f);
	return f;
}

static bool
contains_nocase (const char *hay, const char *needle)
{
	for (size_t n = strlen (needle); *hay != '\0'; hay++) {
		size_t i = 0;
		while (i < n && hay[i] != '\0' &&
		       tolower ((unsigned char) hay[i]) == tolower ((unsigned char) needle[i]))
			i++;
		if (i == n)
			return true;
	}
	return false;
}

/* Ask fontconfig, if installed, for a font. Returns a file only when
   the match's family contains `family' (any family if NULL). */
static FontFile
fc_match (const char *pattern, const char *family)
{
	FontFile f = { NULL, 0 };
	const char *argv[] = { "fc-match", "-f", "%{family}\n%{file}\n%{index}", pattern, NULL };
	StrBuf out;
	sb_init (&out);
	if (process_run (argv, NULL, NULL, 0, &out, NULL) == 0) {
		char *file = strchr (out.s, '\n');
		char *index = file != NULL ? strchr (file + 1, '\n') : NULL;
		if (index != NULL) {
			*file++ = '\0';
			*index++ = '\0';
			if (family == NULL || contains_nocase (out.s, family)) {
				f = font_file (file, 0);
				parse_long (index, &f.index);
			}
		}
	}
	sb_free (&out);
	return f;
}

/* Font files for the four styles of a family: regular, bold, italic,
   bold italic. A ".ttc#N" suffix selects a face in a collection. */
static const char *const font_files[][NSTYLES] = {
	{ "~/Library/Fonts/JetBrainsMono-Regular.ttf", "~/Library/Fonts/JetBrainsMono-Bold.ttf",
	  "~/Library/Fonts/JetBrainsMono-Italic.ttf", "~/Library/Fonts/JetBrainsMono-BoldItalic.ttf" },
	{ "/Library/Fonts/JetBrainsMono-Regular.ttf", "/Library/Fonts/JetBrainsMono-Bold.ttf",
	  "/Library/Fonts/JetBrainsMono-Italic.ttf", "/Library/Fonts/JetBrainsMono-BoldItalic.ttf" },
	{ "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SF-Mono-Regular.otf",
	  "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SF-Mono-Bold.otf",
	  "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SF-Mono-RegularItalic.otf",
	  "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SF-Mono-BoldItalic.otf" },
	{ "/System/Library/Fonts/Menlo.ttc#0", "/System/Library/Fonts/Menlo.ttc#1",
	  "/System/Library/Fonts/Menlo.ttc#2", "/System/Library/Fonts/Menlo.ttc#3" },
	{ "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
	  "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Bold.ttf",
	  "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Italic.ttf",
	  "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-BoldItalic.ttf" },
	{ "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
	  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
	  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf",
	  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-BoldOblique.ttf" },
	{ "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
	  "/usr/share/fonts/TTF/DejaVuSansMono-Oblique.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-BoldOblique.ttf" },
};

/* Families to look for with fontconfig, best first. */
static const char *const font_families[] = {
	"JetBrains Mono", "Fira Code", "Cascadia Code", "SF Mono", "Source Code Pro", "Hack",
	"Iosevka", "Ubuntu Mono", "DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono",
};

static FontFile
font_file_spec (const char *spec)
{
	const char *hash = strrchr (spec, '#');
	long index = 0;
	if (hash != NULL && parse_long (hash + 1, &index)) {
		char *path = xstrndup (spec, (size_t) (hash - spec));
		FontFile f = font_file (path, index);
		free (path);
		return f;
	}
	return font_file (spec, 0);
}

/* Find the files of the font family `family' with fontconfig. */
static bool
find_family (const char *family, FontFile files[NSTYLES])
{
	static const char *const styles[NSTYLES] = {
		"", ":weight=bold", ":slant=italic", ":weight=bold:slant=italic",
	};
	files[0] = fc_match (family, family);
	if (files[0].path == NULL)
		return false;
	for (int s = 1; s < NSTYLES; s++) {
		char *pattern = xasprintf ("%s%s", family, styles[s]);
		files[s] = fc_match (pattern, family);
		free (pattern);
	}
	return true;
}

/* Choose font files: `spec' is the gui-font variable, a file or family. */
static void
find_fonts (const char *spec, FontFile files[NSTYLES])
{
	memset (files, 0, NSTYLES * sizeof files[0]);
	if (spec != NULL && strcmp (spec, "nil") != 0 && *spec != '\0') {
		if (strchr (spec, '/') != NULL) {
			files[0] = font_file_spec (spec);
			if (files[0].path != NULL)
				return;
		} else if (find_family (spec, files))
			return;
		fprintf (stderr, "%s: cannot find font `%s'\n", program_name, spec);
	}

	for (size_t i = 0; i < sizeof font_files / sizeof font_files[0]; i++) {
		files[0] = font_file_spec (font_files[i][0]);
		if (files[0].path != NULL) {
			for (int s = 1; s < NSTYLES; s++)
				files[s] = font_file_spec (font_files[i][s]);
			return;
		}
	}
	for (size_t i = 0; i < sizeof font_families / sizeof font_families[0]; i++)
		if (find_family (font_families[i], files))
			return;
	files[0] = fc_match ("monospace", NULL);
}

static TTF_Font *
open_font (const FontFile *f, int px)
{
	TTF_Font *font = TTF_OpenFontIndex (f->path, px, f->index);
	if (font != NULL)
		TTF_SetFontHinting (font, TTF_HINTING_LIGHT_SUBPIXEL);
	return font;
}

static void close_fallbacks (void);
static void glyph_cache_clear (void);

static int
font_px (void)
{
	return (int) lround (font_size * scale);
}

/* (Re)open the fonts at the current size and scale. */
static void
load_fonts (void)
{
	FontFile files[NSTYLES];
	find_fonts (font_var, files);

	for (int s = 0; s < NSTYLES; s++)
		if (fonts[s] != NULL) {
			TTF_CloseFont (fonts[s]);
			fonts[s] = NULL;
		}
	close_fallbacks ();
	glyph_cache_clear ();

	int px = font_px ();
	if (files[0].path != NULL)
		fonts[0] = open_font (&files[0], px);
	if (fonts[0] == NULL) {
		fprintf (stderr, "%s: no usable monospace font found; set gui-font in ~/.%s\n",
			 program_name, PACKAGE);
		exit (EXIT_FAILURE);
	}
	for (int s = 1; s < NSTYLES; s++) {
		/* Synthesize styles that have no font file of their own. */
		bool own = files[s].path != NULL &&
			(strcmp (files[s].path, files[0].path) != 0 || files[s].index != files[0].index);
		fonts[s] = own ? open_font (&files[s], px) : NULL;
		if (fonts[s] == NULL) {
			fonts[s] = open_font (&files[0], px);
			TTF_SetFontStyle (fonts[s], (s & STYLE_BOLD ? TTF_STYLE_BOLD : 0) |
					  (s & STYLE_ITALIC ? TTF_STYLE_ITALIC : 0));
		}
	}
	for (int s = 0; s < NSTYLES; s++)
		font_file_free (&files[s]);

	int advance = 0;
	TTF_GlyphMetrics32 (fonts[0], 'M', NULL, NULL, NULL, NULL, &advance);
	cell_w = advance > 0 ? advance : px / 2;
	int height = TTF_FontHeight (fonts[0]);
	cell_h = MAX (height, (int) lround (px * LINE_SPACING));
	text_y = (cell_h - height) / 2;
	ascent = TTF_FontAscent (fonts[0]);
	pad = (int) lround (PADDING * scale);
	gui_preview_set_metrics (pad, cell_w, cell_h, scale);
}

static void
close_fallbacks (void)
{
	for (size_t i = 0; i < nfallbacks; i++) {
		free (fallbacks[i].path);
		if (fallbacks[i].font != NULL)
			TTF_CloseFont (fallbacks[i].font);
	}
	free (fallbacks);
	fallbacks = NULL;
	nfallbacks = 0;
}

static TTF_Font *
fallback_open (const FontFile *f)
{
	/* Faces of one collection may cover different characters. */
	for (size_t i = 0; i < nfallbacks; i++)
		if (strcmp (fallbacks[i].path, f->path) == 0 && fallbacks[i].index == f->index)
			return fallbacks[i].font;
	fallbacks = xrealloc (fallbacks, (nfallbacks + 1) * sizeof (Fallback));
	fallbacks[nfallbacks].path = xstrdup (f->path);
	fallbacks[nfallbacks].index = f->index;
	fallbacks[nfallbacks].font = open_font (f, font_px ());
	return fallbacks[nfallbacks++].font;
}

/* Fonts to try for characters the main font lacks, where fontconfig
   cannot suggest one. */
static const char *const fallback_files[] = {
	"/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
	"/Library/Fonts/Arial Unicode.ttf",
	"/System/Library/Fonts/Hiragino Sans GB.ttc",
	"/System/Library/Fonts/Apple Symbols.ttf",
	"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
};

/* A font with a glyph for `cp', or NULL. */
static TTF_Font *
fallback_font (uint32_t cp)
{
	for (size_t i = 0; i < nfallbacks; i++)
		if (fallbacks[i].font != NULL && TTF_GlyphIsProvided32 (fallbacks[i].font, cp))
			return fallbacks[i].font;

	char *pattern = xasprintf (":charset=%x", (unsigned) cp);
	FontFile f = fc_match (pattern, NULL);
	free (pattern);
	TTF_Font *font = f.path != NULL ? fallback_open (&f) : NULL;
	font_file_free (&f);
	if (font != NULL && TTF_GlyphIsProvided32 (font, cp))
		return font;

	for (size_t i = 0; i < sizeof fallback_files / sizeof fallback_files[0]; i++) {
		f = font_file_spec (fallback_files[i]);
		font = f.path != NULL ? fallback_open (&f) : NULL;
		font_file_free (&f);
		if (font != NULL && TTF_GlyphIsProvided32 (font, cp))
			return font;
	}
	return NULL;
}


/*--------------------------------------------------------------------------
 * Glyph cache: one white texture per cell text and style, tinted when
 * drawn.
 *--------------------------------------------------------------------------*/

typedef struct {
	char text[sizeof ((Cell *) 0)->ch];
	int style;
	SDL_Texture *tex;	/* NULL if nothing to draw. */
	int w, h, dy;		/* Size, and offset from the cell's text top. */
	int slant;		/* The width a synthesized italic adds. */
	bool used;
} Glyph;

#define GLYPH_CACHE_SIZE 4096	/* A power of two. */
static Glyph glyph_cache[GLYPH_CACHE_SIZE];
static size_t glyph_count;

static void
glyph_cache_clear (void)
{
	for (size_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
		if (glyph_cache[i].tex != NULL)
			SDL_DestroyTexture (glyph_cache[i].tex);
		glyph_cache[i].tex = NULL;
		glyph_cache[i].used = false;
	}
	glyph_count = 0;
}

static Glyph *
glyph_get (const char *text, int style)
{
	uint32_t h = 2166136261u;
	for (const char *p = text; *p != '\0'; p++)
		h = (h ^ (unsigned char) *p) * 16777619u;
	h ^= (uint32_t) style;

	size_t i = h & (GLYPH_CACHE_SIZE - 1);
	for (; glyph_cache[i].used; i = (i + 1) & (GLYPH_CACHE_SIZE - 1))
		if (glyph_cache[i].style == style && strcmp (glyph_cache[i].text, text) == 0)
			return &glyph_cache[i];

	if (glyph_count >= GLYPH_CACHE_SIZE * 3 / 4) {
		glyph_cache_clear ();
		return glyph_get (text, style);
	}

	Glyph *g = &glyph_cache[i];
	g->used = true;
	glyph_count++;
	strcpy (g->text, text);
	g->style = style;
	g->tex = NULL;

	size_t len = strlen (text), char_len;
	uint32_t cp = utf8_decode (text, len, 0, &char_len);
	TTF_Font *font = fonts[style];
	if (!TTF_GlyphIsProvided32 (font, cp)) {
		TTF_Font *fb = fallback_font (cp);
		if (fb != NULL)
			font = fb;
	}

	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface *surf = TTF_RenderUTF8_Blended (font, text, white);
	if (surf == NULL)
		return g;
	g->tex = SDL_CreateTextureFromSurface (renderer, surf);
	g->w = surf->w;
	g->h = surf->h;
	/* SDL_ttf shears a synthesized italic by about a fifth of its
	   height, making it wider than its cell without being a wider
	   character. */
	g->slant = TTF_GetFontStyle (font) & TTF_STYLE_ITALIC ? TTF_FontHeight (font) / 4 : 0;
	/* Align the baselines of fallback fonts with the main font. */
	g->dy = ascent - TTF_FontAscent (font);
	SDL_FreeSurface (surf);
	if (g->tex != NULL)
		SDL_SetTextureBlendMode (g->tex, SDL_BLENDMODE_BLEND);
	return g;
}


/*--------------------------------------------------------------------------
 * Colours.
 *--------------------------------------------------------------------------*/

typedef struct {
	Uint8 r, g, b;
} Rgb;

/* The 16 ANSI colours, softened to suit the built-in themes: one set
   for dark backgrounds and one for light. */
static const Rgb ansi_dark[16] = {
	{ 0x34, 0x3a, 0x46 }, { 0xff, 0x7a, 0x93 }, { 0xa6, 0xda, 0x95 }, { 0xff, 0xc7, 0x77 },
	{ 0x82, 0xaa, 0xff }, { 0xc0, 0x99, 0xff }, { 0x86, 0xe1, 0xfc }, { 0xc8, 0xd3, 0xf5 },
	{ 0x5f, 0x67, 0x79 }, { 0xff, 0x98, 0xa4 }, { 0xc3, 0xe8, 0x8d }, { 0xff, 0xd8, 0x9a },
	{ 0xa4, 0xc2, 0xff }, { 0xd4, 0xb5, 0xff }, { 0xa8, 0xec, 0xff }, { 0xf4, 0xf6, 0xfb },
};

static const Rgb ansi_light[16] = {
	{ 0x2b, 0x2d, 0x33 }, { 0xd6, 0x30, 0x4b }, { 0x3a, 0x8a, 0x2e }, { 0xb8, 0x6e, 0x00 },
	{ 0x3b, 0x6f, 0xe0 }, { 0xa6, 0x26, 0xa4 }, { 0x0f, 0x7f, 0x8f }, { 0xa0, 0xa4, 0xad },
	{ 0x6b, 0x70, 0x7a }, { 0xe0, 0x4b, 0x62 }, { 0x4c, 0xa2, 0x3e }, { 0xc9, 0x8a, 0x12 },
	{ 0x56, 0x84, 0xec }, { 0xb8, 0x47, 0xb5 }, { 0x17, 0x93, 0xa5 }, { 0xe8, 0xea, 0xef },
};

static const Rgb *ansi = ansi_dark;

static Rgb default_fg = { 0xd5, 0xd9, 0xe0 }, default_bg = { 0x17, 0x19, 0x1e };
static Rgb cursor_rgb = { 0x82, 0xaa, 0xff };

static Rgb
color_rgb (int c, Rgb dflt)
{
	if (c < 0)
		return dflt;
	if (c & 0x1000000)
		return (Rgb) { (Uint8) (c >> 16), (Uint8) (c >> 8), (Uint8) c };
	if (c < 16)
		return ansi[c];
	if (c < 232) {
		static const Uint8 levels[] = { 0, 95, 135, 175, 215, 255 };
		c -= 16;
		return (Rgb) { levels[c / 36], levels[(c / 6) % 6], levels[c % 6] };
	}
	Uint8 v = (Uint8) (8 + 10 * (c - 232));
	return (Rgb) { v, v, v };
}

static bool
rgb_eq (Rgb a, Rgb b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b;
}

static Rgb
rgb_mix (Rgb a, Rgb b, int percent_a)
{
	return (Rgb) {
		(Uint8) ((a.r * percent_a + b.r * (100 - percent_a)) / 100),
		(Uint8) ((a.g * percent_a + b.g * (100 - percent_a)) / 100),
		(Uint8) ((a.b * percent_a + b.b * (100 - percent_a)) / 100),
	};
}

/* Take the default colours and the cursor colour from the theme. */
static void
update_theme_colors (void)
{
	TerminalStyle style;
	if (resolve_terminal_style (FACE_DEFAULT, &style)) {
		default_fg = style.has_foreground ? color_rgb (style.foreground, default_fg) : (Rgb) { 0xd5, 0xd9, 0xe0 };
		default_bg = style.has_background ? color_rgb (style.background, default_bg) : (Rgb) { 0x17, 0x19, 0x1e };
	}
	ansi = default_bg.r + default_bg.g + default_bg.b > 3 * 128 ? ansi_light : ansi_dark;
	cursor_rgb = default_fg;
	if (resolve_terminal_style (FACE_CURSOR, &style) && style.has_background)
		cursor_rgb = color_rgb (style.background, default_fg);
}

static void
cell_colors (const Attr *a, Rgb *fg, Rgb *bg)
{
	*fg = color_rgb (a->fg, default_fg);
	*bg = color_rgb (a->bg, default_bg);
	if (a->flags & ATTR_REVERSE) {
		Rgb t = *fg;
		*fg = *bg;
		*bg = t;
	}
	if (a->flags & ATTR_DIM)
		*fg = rgb_mix (*fg, *bg, 55);
}

/* Show `face' over what has colours `*fg' and `*bg' and attributes
   `*a'. Its colours count only where they are not the default face's,
   which every face inherits. */
static void
apply_face_over (const char *face, Attr *a, Rgb *fg, Rgb *bg)
{
	TerminalStyle st, dflt;
	if (!resolve_terminal_style (face, &st))
		return;
	bool have_dflt = resolve_terminal_style (FACE_DEFAULT, &dflt);
	if (st.reverse) {
		Rgb t = *fg;
		*fg = *bg;
		*bg = t;
	}
	if (st.has_foreground && !(have_dflt && dflt.has_foreground && dflt.foreground == st.foreground))
		*fg = color_rgb (st.foreground, *fg);
	if (st.has_background && !(have_dflt && dflt.has_background && dflt.background == st.background))
		*bg = color_rgb (st.background, *bg);
	if (st.underline)
		a->flags |= ATTR_UNDERLINE;
	if (st.bold)
		a->flags |= ATTR_BOLD;
}

/* The link under the pointer, if `hovering': it is shown in the
   link-hover face, with its hint beside it in the tooltip face. */
static bool hovering;
static ScreenLink hover;

static bool
hovered (size_t y, size_t x)
{
	return hovering && y == hover.y && x >= hover.x0 && x < hover.x1;
}

/* The colours and attributes of the cell at `y', `x'. */
static void
shown_colors (size_t y, size_t x, Attr *a, Rgb *fg, Rgb *bg)
{
	*a = phys[y * phys_cols + x].attr;
	cell_colors (a, fg, bg);
	if (hovered (y, x))
		apply_face_over (FACE_LINK_HOVER, a, fg, bg);
}

/*--------------------------------------------------------------------------
 * Painting.
 *--------------------------------------------------------------------------*/

static void
set_color (Rgb c, Uint8 alpha)
{
	SDL_SetRenderDrawColor (renderer, c.r, c.g, c.b, alpha);
}

static void
fill (int x, int y, int w, int h)
{
	SDL_Rect r = { x, y, w, h };
	SDL_RenderFillRect (renderer, &r);
}

static int
line_px (void)
{
	return MAX (1, (int) lround (scale));
}

/* Box-drawing lines are drawn rather than taken from the font, so that
   they join up across the line spacing. Returns false for other text. */
static bool
draw_box (const char *text, int x, int y, Rgb fg)
{
	set_color (fg, 255);
	int t = line_px ();
	if (strcmp (text, "\xe2\x94\x82") == 0)		/* U+2502 │ */
		fill (x + (cell_w - t) / 2, y, t, cell_h);
	else if (strcmp (text, "\xe2\x94\x80") == 0)	/* U+2500 ─ */
		fill (x, y + (cell_h - t) / 2, cell_w, t);
	else
		return false;
	return true;
}

static void
draw_text (const Cell *c, int x, int y, int width, Rgb fg)
{
	if (c->ch[0] == ' ' && c->ch[1] == '\0')
		return;
	if (draw_box (c->ch, x, y, fg))
		return;
	int style = (c->attr.flags & ATTR_BOLD ? STYLE_BOLD : 0) |
		(c->attr.flags & ATTR_ITALIC ? STYLE_ITALIC : 0);
	Glyph *g = glyph_get (c->ch, style);
	if (g->tex == NULL)
		return;
	SDL_Rect dst = { x, y + text_y + g->dy, g->w, g->h };
	int room = width * cell_w;
	if (g->w > room + cell_w / 4 + g->slant) {
		/* Shrink glyphs from wider fallback fonts to fit their cells. */
		dst.w = room;
		dst.h = g->h * room / g->w;
		dst.y = y + (cell_h - dst.h) / 2;
	} else if (width > 1 && g->w < room)
		dst.x += (room - g->w) / 2;	/* Centre wide characters. */
	SDL_SetTextureColorMod (g->tex, fg.r, fg.g, fg.b);
	SDL_RenderCopy (renderer, g->tex, NULL, &dst);
}

static void
draw_decorations (const Attr *a, int x, int y, int width, Rgb fg)
{
	set_color (fg, 255);
	int t = line_px ();
	if (a->flags & ATTR_UNDERLINE)
		fill (x, y + text_y + ascent + t, width * cell_w, t);
	if (a->flags & ATTR_STRIKE)
		fill (x, y + text_y + ascent * 2 / 3, width * cell_w, t);
}

/* The columns that the `len' bytes of `s' take. */
static size_t
text_columns (const char *s, size_t len)
{
	size_t cols = 0;
	for (size_t i = 0; i < len;) {
		size_t n;
		int w = utf8_char_display_width (utf8_decode (s, len, i, &n));
		cols += w > 0 ? (size_t) w : 0;
		i += n > 0 ? n : 1;
	}
	return cols;
}

/* Where the hint of the hovered link goes, in pixels: below the link,
   or above it if there is no room there, but never over the minibuffer
   line. Returns "below" or "above", or NULL if it fits neither. */
static const char *
tooltip_rect (SDL_Rect *r)
{
	size_t lines = 0, cols = 0;
	for (const char *p = hover.hint;; p++) {
		const char *e = strchr (p, '\n');
		size_t n = e != NULL ? (size_t) (e - p) : strlen (p);
		cols = MAX (cols, text_columns (p, n));
		lines++;
		if (e == NULL)
			break;
		p = e;
	}
	int gap = 2 * line_px ();
	r->w = MIN ((int) cols * cell_w + 2 * cell_w, out_w);
	r->h = (int) lines * cell_h + cell_h / 2;
	int top = pad + (int) hover.y * cell_h, bottom = top + cell_h;
	int limit = pad + (int) (phys_rows - 1) * cell_h;
	/* The text lines up with the link's. */
	r->x = MAX (0, MIN (pad + (int) hover.x0 * cell_w - cell_w, out_w - r->w));
	if (bottom + gap + r->h <= limit) {
		r->y = bottom + gap;
		return "below";
	}
	if (top - gap - r->h >= 0) {
		r->y = top - gap - r->h;
		return "above";
	}
	return NULL;
}

static void
draw_tooltip (void)
{
	SDL_Rect r;
	if (!hovering || hover.y + 1 >= phys_rows || tooltip_rect (&r) == NULL)
		return;
	Attr a = { TERM_COLOR_DEFAULT, TERM_COLOR_DEFAULT, 0 };
	Rgb fg = default_fg, bg = default_bg;
	apply_face_over (FACE_TOOLTIP, &a, &fg, &bg);
	set_color (bg, 255);
	fill (r.x, r.y, r.w, r.h);
	set_color (rgb_mix (fg, bg, 30), 255);
	for (int i = 0; i < line_px (); i++) {
		SDL_Rect b = { r.x + i, r.y + i, r.w - 2 * i, r.h - 2 * i };
		SDL_RenderDrawRect (renderer, &b);
	}

	SDL_RenderSetClipRect (renderer, &r);
	Cell c;
	c.attr = a;
	int py = r.y + cell_h / 4;
	for (const char *p = hover.hint;; p++, py += cell_h) {
		const char *e = strchr (p, '\n');
		size_t n = e != NULL ? (size_t) (e - p) : strlen (p);
		int px = r.x + cell_w;
		for (size_t i = 0; i < n;) {
			size_t len;
			int w = utf8_char_display_width (utf8_decode (p, n, i, &len));
			len = MAX (len, 1);
			if (w > 0) {
				size_t k = MIN (len, sizeof c.ch - 1);
				memcpy (c.ch, p + i, k);
				c.ch[k] = '\0';
				draw_text (&c, px, py, w, fg);
				px += w * cell_w;
			}
			i += len;
		}
		if (e == NULL)
			break;
		p = e;
	}
	SDL_RenderSetClipRect (renderer, NULL);
}

static size_t
cell_width (size_t y, size_t x)
{
	return x + 1 < phys_cols && phys[y * phys_cols + x + 1].ch[0] == '\0' ? 2 : 1;
}

static void
render (void)
{
	if (!initialized || phys == NULL)
		return;

	set_color (default_bg, 255);
	SDL_RenderClear (renderer);

	/* Backgrounds, in runs of one colour. A row whose ends are coloured,
	   such as a status line, extends into the padding. */
	for (size_t y = 0; y < phys_rows; y++) {
		int py = pad + (int) y * cell_h;
		const Cell *row = &phys[y * phys_cols];
		Rgb fg, first, last;
		cell_colors (&row[0].attr, &fg, &first);
		cell_colors (&row[phys_cols - 1].attr, &fg, &last);
		bool extend = !rgb_eq (first, default_bg) && !rgb_eq (last, default_bg);
		for (size_t x = 0; x < phys_cols;) {
			Rgb bg;
			Attr a;
			shown_colors (y, x, &a, &fg, &bg);
			size_t end = x + 1;
			for (Rgb next; end < phys_cols; end++) {
				shown_colors (y, end, &a, &fg, &next);
				if (!rgb_eq (next, bg))
					break;
			}
			if (!rgb_eq (bg, default_bg)) {
				int x0 = pad + (int) x * cell_w, x1 = pad + (int) end * cell_w;
				if (extend && x == 0)
					x0 = 0;
				if (extend && end == phys_cols)
					x1 = out_w;
				set_color (bg, 255);
				fill (x0, py, x1 - x0, cell_h);
			}
			x = end;
		}
	}

	/* Images and PDF pages, which are drawn over blank cells. */
	gui_preview_draw (phys, phys_rows, phys_cols);

	/* Text. */
	for (size_t y = 0; y < phys_rows; y++)
		for (size_t x = 0; x < phys_cols; x++) {
			const Cell *c = &phys[y * phys_cols + x];
			if (c->ch[0] == '\0')
				continue;
			Rgb fg, bg;
			Attr a;
			shown_colors (y, x, &a, &fg, &bg);
			int px = pad + (int) x * cell_w, py = pad + (int) y * cell_h;
			size_t w = cell_width (y, x);
			draw_text (c, px, py, (int) w, fg);
			draw_decorations (&a, px, py, (int) w, fg);
		}

	/* The cursor: a block over the character, or an outline when the
	   window does not have the focus. */
	if (phys_cursor_visible && phys_y < phys_rows && phys_x < phys_cols) {
		const Cell *c = &phys[phys_y * phys_cols + phys_x];
		size_t w = cell_width (phys_y, phys_x);
		int px = pad + (int) phys_x * cell_w, py = pad + (int) phys_y * cell_h;
		set_color (cursor_rgb, 255);
		if (focused) {
			fill (px, py, (int) w * cell_w, cell_h);
			Rgb fg, bg;
			Attr a;
			shown_colors (phys_y, phys_x, &a, &fg, &bg);
			draw_text (c, px, py, (int) w, rgb_eq (bg, cursor_rgb) ? default_bg : bg);
		} else {
			int t = line_px ();
			SDL_Rect r = { px, py, (int) w * cell_w, cell_h };
			for (int i = 0; i < t; i++) {
				SDL_RenderDrawRect (renderer, &r);
				r.x++, r.y++, r.w -= 2, r.h -= 2;
			}
		}
		/* Put input method windows next to the cursor. */
		SDL_Rect ime = { (int) (px / scale), (int) (py / scale), (int) (cell_w / scale), (int) (cell_h / scale) };
		SDL_SetTextInputRect (&ime);
	}

	draw_tooltip ();

	if (bell_until != 0) {
		SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_BLEND);
		set_color (rgb_mix (ansi[1], default_bg, 50), 40);
		fill (0, 0, out_w, out_h);
		SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_NONE);
	}

	SDL_RenderPresent (renderer);
}


/*--------------------------------------------------------------------------
 * Window geometry.
 *--------------------------------------------------------------------------*/

static void
update_scale (void)
{
	int ww, wh;
	SDL_GetWindowSize (window, &ww, &wh);
	SDL_GetRendererOutputSize (renderer, &out_w, &out_h);
	scale = ww > 0 ? (float) out_w / (float) ww : 1;
#ifndef __APPLE__
	/* X11 does not scale windows: follow the screen's DPI instead. */
	float dpi;
	if (scale == 1 && SDL_GetDisplayDPI (SDL_GetWindowDisplayIndex (window), NULL, &dpi, NULL) == 0 &&
	    dpi >= 120)
		scale = roundf (dpi / 96 * 4) / 4;
#endif
}

static void
grid_size (size_t *rows, size_t *cols)
{
	*cols = (size_t) MAX ((out_w - 2 * pad) / cell_w, 10);
	*rows = (size_t) MAX ((out_h - 2 * pad) / cell_h, 3);
}

static void point_at (bool moved, size_t y, size_t x);
static void revalidate_hover (void);

/* Fit the grid to the window, after a resize or a change of font. */
static void
apply_resize (void)
{
	pending_resize = false;
	point_at (false, 0, 0);
	float old_scale = scale;
	update_scale ();
	if (scale != old_scale)
		load_fonts ();
	size_t rows, cols;
	grid_size (&rows, &cols);
	if (rows != scr_rows || cols != scr_cols) {
		screen_resize (rows, cols);
		resize_windows ();
		redisplay_after_output ();
	} else
		render ();
}

static double
configured_font_size (void)
{
	long n;
	const char *v = font_size_var;
	return v != NULL && parse_long (v, &n) && n > 0 ? (double) n : DEFAULT_FONT_SIZE;
}

static void
set_zoom (double z)
{
	double size = configured_font_size () + z;
	if (size < MIN_FONT_SIZE || size > MAX_FONT_SIZE)
		return;
	zoom = z;
	font_size = size;
	load_fonts ();
	pending_resize = true;
}

/* Follow changes to the gui-font and gui-font-size variables. */
static void
check_font_variables (void)
{
	const char *f = get_variable ("gui-font"), *s = get_variable ("gui-font-size");
	if (f == NULL || s == NULL)
		return;
	if (font_var != NULL && strcmp (f, font_var) == 0 && strcmp (s, font_size_var) == 0)
		return;
	bool first = font_var == NULL;
	free (font_var);
	free (font_size_var);
	font_var = xstrdup (f);
	font_size_var = xstrdup (s);
	if (!first)
		set_zoom (zoom);
}


/*--------------------------------------------------------------------------
 * Initialisation.
 *--------------------------------------------------------------------------*/

static int signal_pipe[2] = { -1, -1 };
static volatile sig_atomic_t chld_pending = 0;

static void
chld_handler (int signo)
{
	(void) signo;
	chld_pending = 1;
	if (signal_pipe[1] >= 0) {
		int saved = errno;
		char c = 0;
		if (write (signal_pipe[1], &c, 1) < 0) { /* Ignore error. */ }
		errno = saved;
	}
}

/* Mouse pointers: over text, over dividers between windows, and over
   links. */
static SDL_Cursor *cursor_ibeam, *cursor_rows, *cursor_cols, *cursor_arrow, *cursor_hand, *cursor_shown;

static void
show_cursor (SDL_Cursor *c)
{
	if (c != NULL && c != cursor_shown) {
		SDL_SetCursor (c);
		cursor_shown = c;
	}
}

/* The window shows UTF-8 whatever the locale, but character widths
   come from wcwidth, which knows those of non-ASCII characters only in
   a UTF-8 locale; and an app launched from Finder or a desktop menu
   may have no locale at all. Then take a UTF-8 LC_CTYPE, so that the
   whole editor agrees on the widths. */
static void
use_utf8_ctype (void)
{
	static const char *const names[] = { "C.UTF-8", "C.utf8", "en_US.UTF-8", "UTF-8" };
	const char *codeset = nl_langinfo (CODESET);
	if (strcmp (codeset, "UTF-8") == 0 || strcmp (codeset, "utf8") == 0)
		return;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (setlocale (LC_CTYPE, names[i]) != NULL)
			return;
}

void
term_init (void)
{
	use_utf8_ctype ();

	/* An app launched from Finder or the Dock starts in `/'; start in
	   the home directory instead, as a terminal session would. */
	char *cwd = get_current_dir ();
	if (strcmp (cwd, "/") == 0) {
		char *home = get_home_dir ();
		if (chdir (home) != 0) { /* Stay in `/'. */ }
		free (home);
	}
	free (cwd);

	if (pipe (signal_pipe) == 0)
		for (int i = 0; i < 2; i++) {
			fcntl (signal_pipe[i], F_SETFL, fcntl (signal_pipe[i], F_GETFL) | O_NONBLOCK);
			fcntl (signal_pipe[i], F_SETFD, FD_CLOEXEC);
		}
	struct sigaction sa;
	memset (&sa, 0, sizeof sa);
	sa.sa_handler = chld_handler;
	sigemptyset (&sa.sa_mask);
	sa.sa_flags = SA_NOCLDSTOP;
	sigaction (SIGCHLD, &sa, NULL);

	SDL_SetHint (SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#ifdef SDL_HINT_IME_SHOW_UI
	SDL_SetHint (SDL_HINT_IME_SHOW_UI, "1");
#endif
	if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0 || TTF_Init () != 0) {
		fprintf (stderr, "%s: cannot open a window: %s\n"
			 "Use `%s' to edit in a terminal.\n", program_name, SDL_GetError (), PACKAGE);
		exit (EXIT_FAILURE);
	}

	window = SDL_CreateWindow (PACKAGE_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 640,
				   SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
	if (window != NULL)
		renderer = SDL_CreateRenderer (window, -1, SDL_RENDERER_ACCELERATED);
	if (renderer == NULL && window != NULL)
		renderer = SDL_CreateRenderer (window, -1, SDL_RENDERER_SOFTWARE);
	if (renderer == NULL) {
		fprintf (stderr, "%s: cannot open a window: %s\n", program_name, SDL_GetError ());
		exit (EXIT_FAILURE);
	}
	gui_preview_init (renderer);

	initialized = true;
	update_scale ();
	font_var = xstrdup ("nil");
	font_size_var = xstrdup ("14");
	load_fonts ();

	/* Size the window for a comfortable grid. */
	SDL_SetWindowSize (window, (int) ((INITIAL_COLS * cell_w + 2 * pad) / scale),
			   (int) ((INITIAL_ROWS * cell_h + 2 * pad) / scale));
	SDL_SetWindowMinimumSize (window, (int) ((20 * cell_w + 2 * pad) / scale),
				  (int) ((6 * cell_h + 2 * pad) / scale));
	SDL_SetWindowPosition (window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	update_scale ();
	size_t rows, cols;
	grid_size (&rows, &cols);
	screen_resize (rows, cols);

	cursor_ibeam = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_IBEAM);
	cursor_rows = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_SIZENS);
	cursor_cols = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_SIZEWE);
	cursor_arrow = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_ARROW);
	cursor_hand = SDL_CreateSystemCursor (SDL_SYSTEM_CURSOR_HAND);
	show_cursor (cursor_ibeam);
	SDL_StartTextInput ();
	update_theme_colors ();
	set_color (default_bg, 255);
	SDL_RenderClear (renderer);
	SDL_RenderPresent (renderer);
	SDL_ShowWindow (window);
	SDL_RaiseWindow (window);
}

void
term_close (void)
{
	if (!initialized)
		return;
	initialized = false;
	gui_preview_shutdown ();
	glyph_cache_clear ();
	close_fallbacks ();
	for (int s = 0; s < NSTYLES; s++)
		if (fonts[s] != NULL)
			TTF_CloseFont (fonts[s]);
	SDL_DestroyRenderer (renderer);
	SDL_DestroyWindow (window);
	TTF_Quit ();
	SDL_Quit ();
}

bool
term_initialized (void)
{
	return initialized;
}

bool
term_input_eof (void)
{
	return false;
}

const TerminalCapabilities *
term_get_capabilities (void)
{
	return &capabilities;
}

void
term_write_raw (const char *s, size_t len)
{
	(void) s;
	(void) len;
}

void
term_clear (void)
{
	for (size_t i = 0; i < scr_rows * scr_cols; i++)
		screen_blank_cell (&scr_cells[i]);
}

void
term_beep (void)
{
	if (!initialized)
		return;
	bell_until = monotonic_ms () + BELL_MS;
	render ();
}

void
term_suspend (void)
{
	SDL_MinimizeWindow (window);
}

void
term_refresh (void)
{
	if (!initialized)
		return;
	check_font_variables ();
	update_theme_colors ();

	if (phys_rows != scr_rows || phys_cols != scr_cols) {
		free (phys);
		phys = xmalloc (scr_rows * scr_cols * sizeof (Cell));
		phys_rows = scr_rows;
		phys_cols = scr_cols;
	}
	memcpy (phys, scr_cells, scr_rows * scr_cols * sizeof (Cell));
	phys_y = scr_y;
	phys_x = scr_x;
	phys_cursor_visible = scr_cursor_visible;
	revalidate_hover ();

	if (cur_bp != NULL) {
		char *t = xasprintf ("%s \xe2\x80\x94 %s", cur_bp->name, PACKAGE_NAME);
		if (title == NULL || strcmp (t, title) != 0) {
			SDL_SetWindowTitle (window, t);
			free (title);
			title = t;
		} else
			free (t);
	}
	render ();
}


/*--------------------------------------------------------------------------
 * Clipboard.
 *--------------------------------------------------------------------------*/

bool
term_clipboard_set (const char *text, size_t len)
{
	char *s = xstrndup (text, len);
	SDL_SetClipboardText (s);
	free (s);
	return true;
}

char *
term_clipboard_get (size_t *len)
{
	char *s = SDL_GetClipboardText ();
	if (s == NULL)
		return NULL;
	char *text = xstrdup (s);
	SDL_free (s);
	*len = strlen (text);
	return text;
}


/*--------------------------------------------------------------------------
 * Input.
 *--------------------------------------------------------------------------*/

/* Keys from the window, in order. */
static Keystroke *queue;
static size_t queue_head, queue_n, queue_cap;

/* Keys pushed back by ungetkey (a stack: the last is read first). */
static Keystroke *stack;
static size_t stack_n, stack_cap;

static char *pending_paste_text;
static size_t mouse_y, mouse_x;
static bool mouse_down;
static int shell_button;	/* The button (1-3) pressed in a program reading the mouse. */
static float wheel_acc;
static bool suppress_text;	/* The last event was a key down that was handled. */
static bool composing;		/* An input method has uncommitted text. */
static Keystroke altgr_key = KBD_NOKEY;	/* A Right Alt key waiting for its text, */
static int altgr_char;			/* and the character it types without Alt. */

static void
queue_push (Keystroke key)
{
	if (queue_n == queue_cap) {
		Keystroke *q = xmalloc ((queue_cap ? queue_cap * 2 : 64) * sizeof (Keystroke));
		for (size_t i = 0; i < queue_n; i++)
			q[i] = queue[(queue_head + i) % queue_cap];
		free (queue);
		queue = q;
		queue_head = 0;
		queue_cap = queue_cap ? queue_cap * 2 : 64;
	}
	queue[(queue_head + queue_n++) % queue_cap] = key;
}

static Keystroke
queue_pop (void)
{
	Keystroke key = queue[queue_head];
	queue_head = (queue_head + 1) % queue_cap;
	queue_n--;
	return key;
}

static void
push_keys (const char *keys)
{
	KeyVec kv;
	if (keystrtovec (keys, &kv)) {
		for (size_t i = 0; i < kv.n; i++)
			queue_push (kv.v[i]);
		kv_free (&kv);
	}
}

static void
push_text (const char *s)
{
	for (; *s != '\0'; s++)
		queue_push ((unsigned char) *s);
}

static void
paste_clipboard (void)
{
	size_t len;
	char *text = term_clipboard_get (&len);
	if (text == NULL || len == 0) {
		free (text);
		return;
	}
	free (pending_paste_text);
	pending_paste_text = text;
	queue_push (KBD_BRACKETED_PASTE);
}

/* Shortcuts with the Command key (macOS) or Super key. */
static void
command_key (SDL_Keycode sym, bool shift)
{
	switch (sym) {
	case SDLK_c: push_keys ("\\M-w"); break;
	case SDLK_x: push_keys ("\\C-w"); break;
	case SDLK_v: paste_clipboard (); break;
	case SDLK_z: push_keys (shift ? "\\C-x\\C-_" : "\\C-_"); break;
	case SDLK_s: push_keys ("\\C-x\\C-s"); break;
	case SDLK_o: push_keys ("\\C-x\\C-f"); break;
	case SDLK_a: push_keys ("\\C-xh"); break;
	case SDLK_f: push_keys ("\\C-s"); break;
	case SDLK_w: push_keys ("\\C-x0"); break;
	case SDLK_q: push_keys ("\\C-x\\C-c"); break;
	case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: set_zoom (zoom + 1); break;
	case SDLK_MINUS: case SDLK_KP_MINUS: set_zoom (zoom - 1); break;
	case SDLK_0: case SDLK_KP_0: set_zoom (0); break;
	default: break;
	}
}

/* The character a US keyboard types for `c' with Shift. */
static int
shifted (int c)
{
	static const char from[] = "`1234567890-=[]\\;',./", to[] = "~!@#$%^&*()_+{}|:\"<>?";
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 'A';
	const char *p = c != 0 ? strchr (from, c) : NULL;
	return p != NULL ? to[p - from] : c;
}

static Keystroke
special_key (SDL_Keycode sym)
{
	switch (sym) {
	case SDLK_RETURN: case SDLK_KP_ENTER: return KBD_RET;
	case SDLK_TAB: return KBD_TAB;
	case SDLK_BACKSPACE: return KBD_BS;
	case SDLK_DELETE: return KBD_DEL;
	case SDLK_INSERT: return KBD_INS;
	case SDLK_HOME: return KBD_HOME;
	case SDLK_END: return KBD_END;
	case SDLK_PAGEUP: return KBD_PGUP;
	case SDLK_PAGEDOWN: return KBD_PGDN;
	case SDLK_LEFT: return KBD_LEFT;
	case SDLK_RIGHT: return KBD_RIGHT;
	case SDLK_UP: return KBD_UP;
	case SDLK_DOWN: return KBD_DOWN;
	case SDLK_F1: return KBD_F1;
	case SDLK_F2: return KBD_F2;
	case SDLK_F3: return KBD_F3;
	case SDLK_F4: return KBD_F4;
	case SDLK_F5: return KBD_F5;
	case SDLK_F6: return KBD_F6;
	case SDLK_F7: return KBD_F7;
	case SDLK_F8: return KBD_F8;
	case SDLK_F9: return KBD_F9;
	case SDLK_F10: return KBD_F10;
	case SDLK_F11: return KBD_F11;
	case SDLK_F12: return KBD_F12;
	default: return KBD_NOKEY;
	}
}

static void
key_down (const SDL_KeyboardEvent *ev)
{
	SDL_Keycode sym = ev->keysym.sym;
	Uint16 mod = ev->keysym.mod;
	bool ctrl = (mod & KMOD_CTRL) != 0, shift = (mod & KMOD_SHIFT) != 0;
	/* AltGr types characters rather than acting as Meta. */
	bool meta = (mod & KMOD_ALT) != 0 && !(mod & KMOD_MODE);

	/* While composing, keys such as Return and Backspace belong to the
	   input method, which on some systems still reports them. */
	if (composing)
		return;
	if (mod & KMOD_GUI) {
		command_key (sym, shift);
		suppress_text = true;
		return;
	}

	Keystroke key = special_key (sym);
	if (key == KBD_NOKEY && sym == SDLK_ESCAPE)
		key = 033;
	if (key != KBD_NOKEY) {
		if (ctrl && key != 033)
			key |= KBD_CTRL;
		if (meta)
			key |= KBD_META;
		queue_push (key);
		suppress_text = true;
		return;
	}

	/* Printable keys come as text, unless Control or Meta is held. */
	if (!(ctrl || meta) || sym < ' ' || sym > '~')
		return;
	int c = shift ? shifted ((int) sym) : (int) sym;
	if (ctrl) {
		if (c == '[') {
			queue_push (meta ? KBD_META | 033 : 033);
			suppress_text = true;
			return;
		}
		/* As a terminal sends them. */
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';
		if (c == ' ' || c == '2')
			c = '@';
		else if (c == '/')
			c = '_';
		key = c == 'i' ? KBD_TAB : c == 'm' ? KBD_RET : KBD_CTRL | (Keystroke) c;
	} else
		key = (Keystroke) c;
	if (meta)
		key |= KBD_META;
#ifndef __APPLE__
	/* X11 and Wayland report AltGr as Right Alt: the key's text tells
	   whether it was AltGr, which types it, or Meta. */
	if (meta && !ctrl && (mod & KMOD_ALT) == KMOD_RALT) {
		altgr_key = key;
		altgr_char = c;
		return;
	}
#endif
	queue_push (key);
	suppress_text = true;
}

static void
pixel_to_cell (int wx, int wy, size_t *y, size_t *x)
{
	float px = (float) wx * scale - (float) pad, py = (float) wy * scale - (float) pad;
	long cx = (long) floorf (px / (float) cell_w), cy = (long) floorf (py / (float) cell_h);
	*x = (size_t) MAX (0, MIN (cx, (long) scr_cols - 1));
	*y = (size_t) MAX (0, MIN (cy, (long) scr_rows - 1));
}

/* The mouse pointer for cell `y', `x': what the toolkit region under
   it asks for, as a divider does, or else the I-beam. */
static SDL_Cursor *
pointer_at (size_t y, size_t x)
{
	switch (tui_cursor ()) {
	case TUI_CURSOR_POINTER:
		return cursor_hand;
	case TUI_CURSOR_EW_RESIZE:
		return cursor_cols;
	case TUI_CURSOR_NS_RESIZE:
		return cursor_rows;
	default:
		return preview_at (y, x) ? cursor_arrow : cursor_ibeam;
	}
}

/* Dragging a preview to move it. */
static bool preview_drag;
static size_t preview_drag_y, preview_drag_x;	/* The cell it began in. */
static int preview_drag_px, preview_drag_py;	/* The last position. */

/* The cell that the pointer last moved over with no button down. Only
   while `pointer_moved', when the pointer has moved over the window
   since the last key, click, or resize, and has not left it, is a link
   under it hovered. */
static bool pointer_moved;
static size_t pointer_y, pointer_x;

/* Find the link under the pointer again, without changing anything
   else; returns whether it changed. */
static bool
update_hover (void)
{
	ScreenLink l = { 0, 0, 0, NULL };
	bool now = pointer_moved && focused && !mouse_down && shell_button == 0 &&
		!preview_drag && pointer_y + 1 < phys_rows && link_at (pointer_y, pointer_x, &l);
	bool changed = now != hovering || (now && (l.y != hover.y || l.x0 != hover.x0 || l.x1 != hover.x1 ||
						     strcmp (l.hint, hover.hint) != 0));
	if (hovering)
		free (hover.hint);
	hovering = now;
	hover = l;
	if (changed && debug_enabled ("hover")) {
		if (hovering) {
			SDL_Rect r;
			const char *where = tooltip_rect (&r);
			StrBuf shown;
			sb_init (&shown);
			for (size_t x = hover.x0; x < hover.x1 && x < phys_cols; x++)
				sb_add (&shown, phys[hover.y * phys_cols + x].ch);
			char *hint = xstrdup (hover.hint);
			for (char *p = hint; (p = strchr (p, '\n')) != NULL;)
				*p = '|';
			debug_log ("hover", "row %zu, columns %zu-%zu \"%s\", hint %s: %s", hover.y, hover.x0,
				   hover.x1, shown.s != NULL ? shown.s : "", where != NULL ? where : "hidden", hint);
			sb_free (&shown);
			free (hint);
		} else
			debug_log ("hover", "none");
	}
	return changed;
}

/* Show the hand pointer just while a link is hovered: once none is,
   the pointer that the cell it was last over takes. */
static void
show_hand (void)
{
	if (hovering)
		show_cursor (cursor_hand);
	else if (cursor_shown == cursor_hand)
		show_cursor (pointer_at (pointer_y, pointer_x));
}

/* Show the link under the pointer, at `y', `x' if `moved', or else
   none, until the pointer moves again. */
static void
point_at (bool moved, size_t y, size_t x)
{
	pointer_moved = moved;
	if (moved) {
		pointer_y = y;
		pointer_x = x;
	}
	if (update_hover ())
		render ();
	show_hand ();
}

/* After a repaint, the link under the pointer may have moved, gone or
   changed, as when output arrived, the window scrolled or showed
   another buffer, or the theme changed. */
static void
revalidate_hover (void)
{
	update_hover ();
	show_hand ();
}

/* A pointer position, in pixels from the top left of the text grid. */
static void
grid_pixels (int wx, int wy, double *px, double *py)
{
	*px = (double) wx * scale - pad;
	*py = (double) wy * scale - pad;
}

static void
handle_event (const SDL_Event *ev)
{
	/* SDL sends the text of a key straight after its key down. Later
	   text, such as an input method's commit, is never the key's. */
	bool key_handled = suppress_text;
	suppress_text = false;
	if (altgr_key != KBD_NOKEY) {
		Keystroke key = altgr_key;
		altgr_key = KBD_NOKEY;
		if (ev->type == SDL_TEXTINPUT &&
		    !(ev->text.text[0] == altgr_char && ev->text.text[1] == '\0')) {
			push_text (ev->text.text);
			return;
		}
		queue_push (key);
		key_handled = true;
	}

	switch (ev->type) {
	case SDL_QUIT:
		push_keys ("\\C-x\\C-c");
		break;
	case SDL_KEYDOWN:
		point_at (false, 0, 0);
		key_down (&ev->key);
		break;
	case SDL_TEXTEDITING:
		point_at (false, 0, 0);
		/* A dead key after a key taken as Meta starts no composition
		   of ours. */
		if (!key_handled)
			composing = ev->edit.text[0] != '\0';
		break;
	case SDL_TEXTINPUT:
		point_at (false, 0, 0);
		composing = false;
		if (!key_handled)
			push_text (ev->text.text);
		break;
	case SDL_MOUSEBUTTONDOWN:
		point_at (false, 0, 0);
		if (ev->button.button == SDL_BUTTON_LEFT) {
			size_t y, x;
			pixel_to_cell (ev->button.x, ev->button.y, &y, &x);
			mouse_y = y;
			mouse_x = x;
			if (preview_at (y, x)) {
				/* Selects the window; dragging moves the preview. */
				preview_drag = true;
				preview_drag_y = y;
				preview_drag_x = x;
				preview_drag_px = ev->button.x;
				preview_drag_py = ev->button.y;
				SDL_CaptureMouse (SDL_TRUE);
			} else
				mouse_down = true;
			shell_button = shell_mouse_at (y, x) ? 1 : 0;
			queue_push (KBD_MOUSE_1);
		} else if (ev->button.button == SDL_BUTTON_MIDDLE || ev->button.button == SDL_BUTTON_RIGHT) {
			size_t y, x;
			pixel_to_cell (ev->button.x, ev->button.y, &y, &x);
			bool middle = ev->button.button == SDL_BUTTON_MIDDLE;
			if (shell_button == 0 && shell_mouse_at (y, x)) {
				mouse_y = y;
				mouse_x = x;
				shell_button = middle ? 2 : 3;
				queue_push (middle ? KBD_MOUSE_2 : KBD_MOUSE_3);
			} else if (middle)
				paste_clipboard ();
		}
		break;
	case SDL_MOUSEBUTTONUP: {
		static const int buttons[] = { 0, SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT };
		bool released = false;
		if (shell_button != 0 && ev->button.button == buttons[shell_button]) {
			pixel_to_cell (ev->button.x, ev->button.y, &mouse_y, &mouse_x);
			queue_push (KBD_UP_MOUSE_1 + (Keystroke) shell_button - 1);
			shell_button = 0;
			released = true;
		}
		if (ev->button.button == SDL_BUTTON_LEFT) {
			size_t y, x;
			/* Ends a selection in a terminal. */
			if (mouse_down && !released) {
				pixel_to_cell (ev->button.x, ev->button.y, &mouse_y, &mouse_x);
				queue_push (KBD_UP_MOUSE_1);
			}
			mouse_down = false;
			if (preview_drag) {
				preview_drag = false;
				SDL_CaptureMouse (SDL_FALSE);
			}
			pixel_to_cell (ev->button.x, ev->button.y, &y, &x);
			show_cursor (pointer_at (y, x));
		}
		break;
	}
	case SDL_MOUSEMOTION: {
		size_t y, x;
		pixel_to_cell (ev->motion.x, ev->motion.y, &y, &x);
		if (preview_drag) {
			if (!(ev->motion.state & SDL_BUTTON_LMASK)) {
				preview_drag = false;
				SDL_CaptureMouse (SDL_FALSE);
				break;
			}
			double dx = (double) (ev->motion.x - preview_drag_px) * scale;
			double dy = (double) (ev->motion.y - preview_drag_py) * scale;
			preview_drag_px = ev->motion.x;
			preview_drag_py = ev->motion.y;
			if (preview_pointer_scroll (preview_drag_y, preview_drag_x, -dx, -dy, false))
				redisplay_after_output ();
			break;
		}
		/* What is under the pointer shows it can be clicked. */
		if (ev->motion.state == 0 && get_variable_bool ("mouse-highlight") && tui_pointer_moved (true, y, x))
			redisplay_after_output ();
		if (ev->motion.state == 0 && (!pointer_moved || y != pointer_y || x != pointer_x))
			point_at (true, y, x);
		else if (ev->motion.state != 0 && pointer_moved)
			point_at (false, y, x);
		if (!mouse_down)
			show_cursor (hovering ? cursor_hand : pointer_at (y, x));
		Keystroke drag = KBD_NOKEY;
		if (mouse_down && (ev->motion.state & SDL_BUTTON_LMASK))
			drag = KBD_DRAG_MOUSE_1;
		else if (shell_button == 2 && (ev->motion.state & SDL_BUTTON_MMASK))
			drag = KBD_DRAG_MOUSE_2;
		else if (shell_button == 3 && (ev->motion.state & SDL_BUTTON_RMASK))
			drag = KBD_DRAG_MOUSE_3;
		if (drag != KBD_NOKEY) {
			if (y == mouse_y && x == mouse_x)
				break;
			mouse_y = y;
			mouse_x = x;
			/* One pending drag is enough: it reads the latest position. */
			if (queue_n == 0 || queue[(queue_head + queue_n - 1) % queue_cap] != drag)
				queue_push (drag);
		}
		break;
	}
	case SDL_MOUSEWHEEL: {
		float dy = ev->wheel.preciseY, dx = ev->wheel.preciseX;
		if (ev->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
			dy = -dy;
			dx = -dx;
		}
		int wx, wy;
		SDL_GetMouseState (&wx, &wy);
		size_t cy, cx;
		pixel_to_cell (wx, wy, &cy, &cx);
		SDL_Keymod mod = SDL_GetModState ();
		if (preview_at (cy, cx)) {
			/* Zoom about the pointer, or move by three lines a notch. */
			double px, py;
			grid_pixels (wx, wy, &px, &py);
			bool moved = (mod & (KMOD_CTRL | KMOD_GUI))
				? dy != 0 && preview_pointer_zoom (cy, cx, px, py, pow (1.25, dy))
				: (mod & KMOD_SHIFT)
				? preview_pointer_scroll (cy, cx, 3.0 * cell_h * (dx - dy), 0, false)
				: preview_pointer_scroll (cy, cx, 3.0 * cell_h * dx, -3.0 * cell_h * dy, true);
			if (moved)
				redisplay_after_output ();
			break;
		}
		if (mod & (KMOD_CTRL | KMOD_GUI)) {
			if (dy != 0)
				set_zoom (zoom + (dy > 0 ? 1 : -1));
			break;
		}
		mouse_y = cy;
		mouse_x = cx;
		for (wheel_acc += dy; wheel_acc >= 1; wheel_acc -= 1)
			queue_push (KBD_WHEEL_UP);
		for (; wheel_acc <= -1; wheel_acc += 1)
			queue_push (KBD_WHEEL_DOWN);
		break;
	}
	case SDL_WINDOWEVENT:
		switch (ev->window.event) {
		case SDL_WINDOWEVENT_SIZE_CHANGED:
		case SDL_WINDOWEVENT_DISPLAY_CHANGED:
			pending_resize = true;
			break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
		case SDL_WINDOWEVENT_FOCUS_LOST:
			focused = ev->window.event == SDL_WINDOWEVENT_FOCUS_GAINED;
			/* Input methods end or forget their composition then. */
			composing = false;
			if (!focused) {
				tui_pointer_moved (false, 0, 0);
				point_at (false, 0, 0);
			}
			render ();
			break;
		case SDL_WINDOWEVENT_LEAVE:
			if (tui_pointer_moved (false, 0, 0))
				redisplay_after_output ();
			point_at (false, 0, 0);
			break;
		case SDL_WINDOWEVENT_EXPOSED:
			render ();
			break;
		default:
			break;
		}
		break;
	case SDL_RENDER_TARGETS_RESET:
	case SDL_RENDER_DEVICE_RESET:
		glyph_cache_clear ();
		gui_preview_reset ();
		render ();
		break;
	default:
		if (ev->type == gui_preview_event () && gui_preview_poll ())
			redisplay_after_output ();
		break;
	}
}

/* Serve subprocesses without waiting. Returns true if any produced
   output or changed state. */
static bool
serve_processes (void)
{
	struct pollfd fds[1 + MAX_PROCESS_FDS] = { { signal_pipe[0], POLLIN, 0 } };
	size_t nproc = process_poll_fds (fds + 1, MAX_PROCESS_FDS);
	if (poll (fds, (nfds_t) (1 + nproc), 0) <= 0)
		return false;

	bool changed = false;
	if (fds[0].revents & POLLIN) {
		char buf[64];
		while (read (signal_pipe[0], buf, sizeof buf) > 0)
			;
		if (chld_pending) {
			chld_pending = 0;
			changed = process_reap ();
		}
	}
	if (nproc > 0 && process_handle_events (fds + 1, nproc))
		changed = true;
	return changed;
}

/* Wait up to `delay' ms (forever if negative) for a key from the
   window, serving subprocesses meanwhile. */
static Keystroke
read_key (int delay)
{
	int64_t deadline = delay >= 0 ? monotonic_ms () + delay : 0;
	for (;;) {
		SDL_Event ev;
		while (SDL_PollEvent (&ev))
			handle_event (&ev);
		if (altgr_key != KBD_NOKEY) {
			/* Its text, which comes at once, did not. */
			queue_push (altgr_key);
			altgr_key = KBD_NOKEY;
		}
		if (pending_resize)
			apply_resize ();
		if (queue_n > 0)
			return queue_pop ();

		if (serve_processes ()) {
			redisplay_after_output ();
			continue;
		}

		int64_t now = monotonic_ms ();
		if (bell_until != 0 && now >= bell_until) {
			bell_until = 0;
			render ();
		}
		if (delay >= 0 && now >= deadline)
			return KBD_NOKEY;

		/* SDL cannot wait on file descriptors, so check on running
		   subprocesses often. */
		int64_t timeout = delay >= 0 ? deadline - now : 1000;
		if (process_any_alive ())
			timeout = MIN (timeout, 10);
		if (bell_until != 0)
			timeout = MIN (timeout, bell_until - now);
		SDL_WaitEventTimeout (NULL, (int) MAX (timeout, 1));
	}
}

static Keystroke
next_key (int delay, bool *from_window)
{
	*from_window = stack_n == 0;
	return stack_n > 0 ? stack[--stack_n] : read_key (delay);
}

Keystroke
term_getkey (int delay)
{
	bool from_window;
	Keystroke key = next_key (delay, &from_window);
	if (key != 033)
		return key;

	/* ESC typed in a terminal buffer is itself; otherwise it is the
	   Meta prefix, as in a terminal. */
	if (from_window && cur_bp != NULL && shell_char_mode (cur_bp) && !term_minibuf_active ())
		return KBD_ESCAPE;
	key = next_key (GETKEY_DEFAULT, &from_window);
	while (key == 033)
		key = next_key (GETKEY_DEFAULT, &from_window) | KBD_META;
	return key | KBD_META;
}

/* The byte a terminal would send for `key', for quoted-insert. */
unsigned
term_getkey_unfiltered (int delay)
{
	bool from_window;
	Keystroke key = next_key (delay, &from_window);
	if (key == KBD_RET)
		return 015;
	if (key == KBD_TAB)
		return 011;
	if (key == KBD_BS)
		return 0177;
	if (key == (KBD_CTRL | '@'))
		return 0;
	if ((key & ~KBD_CTRL) <= 0xff && (key & KBD_CTRL)) {
		int c = (int) (key & 0xff);
		if (c >= 'a' && c <= 'z')
			return (unsigned) (c - 'a' + 1);
		if (c >= '[' && c <= '_')
			return (unsigned) (c - '@');
	}
	return key;
}

void
term_ungetkey (Keystroke key)
{
	if (key == KBD_NOKEY)
		return;
	if (stack_n == stack_cap) {
		stack_cap = stack_cap ? stack_cap * 2 : 32;
		stack = xrealloc (stack, stack_cap * sizeof (Keystroke));
	}
	stack[stack_n++] = key;
}

size_t
term_buf_len (void)
{
	return stack_n;
}

char *
term_take_pending_paste (void)
{
	char *text = pending_paste_text;
	pending_paste_text = NULL;
	return text;
}

bool
term_mouse_position (size_t *y, size_t *x)
{
	*y = mouse_y;
	*x = mouse_x;
	return true;
}
