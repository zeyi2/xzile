/* Theme and face registry

   Copyright (c) 2025-2026 Zeyi2 <zeyi2@nekoarch.cc>

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

#define THEME_DEFAULT_DARK "default-dark"
#define THEME_DEFAULT_LIGHT "default-light"
#define THEME_TERMINAL_DEFAULT "terminal-default"
#define THEME_XZILE_DARK "xzile-dark"
#define THEME_XZILE_LIGHT "xzile-light"

/* Faces may inherit; this bounds the inheritance depth. */
#define MAX_INHERIT_DEPTH 32

typedef struct {
	char *name;
	char *inherit_name;
	char *foreground;
	char *background;
	bool bold;
	bool underline;
	bool reverse;
	bool italic;
	bool strike;
	bool has_foreground;
	bool has_background;
	bool has_bold;
	bool has_underline;
	bool has_reverse;
	bool has_italic;
	bool has_strike;
} FaceSpec;

typedef struct {
	char *name;
	char *variant;
	Map *faces;
} Theme;

/* The result of resolving a face through its inheritance chain. */
typedef struct {
	const char *foreground;
	const char *background;
	bool bold, underline, reverse, italic, strike;
	bool has_foreground, has_background;
} ResolvedFace;

static Map *face_name_table;
static Map *theme_table;
static Theme *current_theme = NULL;

static const char *const builtin_faces[] = {
	FACE_DEFAULT,
	FACE_REGION,
	FACE_MODE_LINE,
	FACE_MODE_LINE_INACTIVE,
	FACE_MINIBUFFER_PROMPT,
	FACE_VERTICAL_BORDER,
	FACE_LINE_NUMBER,
	FACE_LINE_NUMBER_CURRENT_LINE,
	FACE_TRAILING_WHITESPACE,
	FACE_ISEARCH,
	FACE_LAZY_HIGHLIGHT,
	FACE_MATCH,
	FACE_ERROR,
	FACE_WARNING,
	FACE_SUCCESS,
	FACE_FONT_LOCK_COMMENT,
	FACE_FONT_LOCK_STRING,
	FACE_FONT_LOCK_KEYWORD,
	FACE_FONT_LOCK_FUNCTION_NAME,
	FACE_FONT_LOCK_VARIABLE_NAME,
	FACE_FONT_LOCK_TYPE,
	FACE_FONT_LOCK_CONSTANT,
	FACE_FONT_LOCK_BUILTIN,
	FACE_FONT_LOCK_PREPROCESSOR,
	FACE_FONT_LOCK_WARNING,
	FACE_MAGIT_SECTION_HEADING,
	FACE_MAGIT_DIFF_FILE_HEADING,
	FACE_MAGIT_DIFF_HUNK_HEADING,
	FACE_MAGIT_DIFF_ADDED,
	FACE_MAGIT_DIFF_REMOVED,
	FACE_MAGIT_HASH,
	FACE_MAGIT_BRANCH_LOCAL,
	FACE_MAGIT_BRANCH_REMOTE,
	FACE_MAGIT_LOG_DATE,
	FACE_PICKER_SELECTION,
	FACE_PICKER_MATCH,
	FACE_PICKER_ANNOTATION,
	FACE_PICKER_GROUP,
	FACE_SHOW_PAREN_MATCH,
	FACE_SHOW_PAREN_MISMATCH,
	FACE_CURSOR,
	FACE_LINK,
	FACE_LINK_HOVER,
	FACE_TOOLTIP,
	FACE_MODE_LINE_HIGHLIGHT,
	FACE_TUI_PANEL,
	FACE_TUI_BORDER,
	FACE_TUI_TITLE,
	FACE_TUI_MUTED,
	FACE_TUI_KEY,
	FACE_TUI_HOVER,
	FACE_TUI_SCROLLBAR_TRACK,
	FACE_TUI_SCROLLBAR_THUMB,
	FACE_TUI_SCROLLBAR_THUMB_HOVER,
	FACE_TUI_DIVIDER_HOVER,
	FACE_MARKDOWN_HEADER_1,
	FACE_MARKDOWN_HEADER_2,
	FACE_MARKDOWN_HEADER_3,
	FACE_MARKDOWN_HEADER_4,
	FACE_MARKDOWN_HEADER_5,
	FACE_MARKDOWN_HEADER_6,
	FACE_MARKDOWN_BOLD,
	FACE_MARKDOWN_ITALIC,
	FACE_MARKDOWN_STRIKE,
	FACE_MARKDOWN_INLINE_CODE,
	FACE_MARKDOWN_CODE,
	FACE_MARKDOWN_LINK,
	FACE_MARKDOWN_MARKUP,
	FACE_MARKDOWN_BLOCKQUOTE,
	FACE_MARKDOWN_LIST,
	FACE_MARKDOWN_TABLE,
	FACE_MARKDOWN_HR,
	FACE_MARKDOWN_TASK_DONE,
};

static bool
face_name_exists (const char *name)
{
	return map_get (face_name_table, name) != NULL;
}

static FaceSpec *
face_spec_new (const char *name)
{
	FaceSpec *face = xzalloc (sizeof (FaceSpec));
	face->name = xstrdup (name);
	return face;
}

static void
face_spec_free (void *p)
{
	FaceSpec *face = p;
	if (face == NULL)
		return;
	free (face->name);
	free (face->inherit_name);
	free (face->foreground);
	free (face->background);
	free (face);
}

static void
set_string (char **field, const char *value)
{
	char *old = *field;
	*field = xstrdup_or_null (value);
	free (old);
}

/* Store `face' in `theme', taking ownership. Returns an error message
   (leaving `face' to the caller) if the face or its parent is unknown. */
static char *
theme_set_face (Theme *theme, FaceSpec *face)
{
	if (!face_name_exists (face->name))
		return xasprintf ("Unknown face `%s'", face->name);
	if (face->inherit_name != NULL && !face_name_exists (face->inherit_name))
		return xasprintf ("Unknown face `%s'", face->inherit_name);
	face_spec_free (map_get (theme->faces, face->name));
	map_set (theme->faces, face->name, face);
	return NULL;
}

static Theme *
define_theme (const char *name, const char *variant)
{
	Theme *old_theme = map_get (theme_table, name);
	Theme *theme = xzalloc (sizeof (Theme));
	theme->name = xstrdup (name);
	theme->variant = xstrdup_or_null (variant);
	theme->faces = map_new ();
	map_set (theme_table, name, theme);

	/* The old theme may still be displayed, so it is not freed. */
	if (current_theme == old_theme)
		current_theme = theme;

	return theme;
}

static bool
activate_theme (const char *name)
{
	Theme *theme = map_get (theme_table, name);
	if (theme == NULL)
		return false;

	current_theme = theme;
	return true;
}

static void
merge_face_spec (ResolvedFace *resolved, const FaceSpec *face)
{
	if (face->has_foreground) {
		resolved->foreground = face->foreground;
		resolved->has_foreground = true;
	}
	if (face->has_background) {
		resolved->background = face->background;
		resolved->has_background = true;
	}
	if (face->has_bold)
		resolved->bold = face->bold;
	if (face->has_underline)
		resolved->underline = face->underline;
	if (face->has_reverse)
		resolved->reverse = face->reverse;
	if (face->has_italic)
		resolved->italic = face->italic;
	if (face->has_strike)
		resolved->strike = face->strike;
}

/* How the faces of links look in a theme that leaves them out, which
   the default face alone would not show. */
static void
merge_link_fallback (ResolvedFace *resolved, const char *face_name)
{
	if (strcmp (face_name, FACE_LINK) == 0 || strcmp (face_name, FACE_LINK_HOVER) == 0)
		resolved->underline = true;
	if (strcmp (face_name, FACE_LINK_HOVER) == 0 || strcmp (face_name, FACE_TOOLTIP) == 0)
		resolved->reverse = true;
}

static bool
resolve_face_into (const Theme *theme, const char *face_name, const char **seen,
		   size_t depth, ResolvedFace *resolved)
{
	if (depth >= MAX_INHERIT_DEPTH)
		return false;
	for (size_t i = 0; i < depth; i++)
		if (strcmp (seen[i], face_name) == 0)
			return false; /* Inheritance cycle. */
	seen[depth] = face_name;

	const FaceSpec *face = theme != NULL ? map_get (theme->faces, face_name) : NULL;
	const char *inherit_name = NULL;

	if (face != NULL && face->inherit_name != NULL)
		inherit_name = face->inherit_name;
	else if (strcmp (face_name, FACE_DEFAULT) != 0)
		inherit_name = FACE_DEFAULT;

	if (inherit_name != NULL && !resolve_face_into (theme, inherit_name, seen, depth + 1, resolved))
		return false;

	if (face != NULL)
		merge_face_spec (resolved, face);
	else
		merge_link_fallback (resolved, face_name);

	return true;
}

static int
parse_hex_nibble (char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= 'a' && ch <= 'f')
		return 10 + ch - 'a';
	if (ch >= 'A' && ch <= 'F')
		return 10 + ch - 'A';
	return -1;
}

static bool
parse_hex_color (const char *color_name, int *red, int *green, int *blue)
{
	if (strlen (color_name) != 7 || color_name[0] != '#')
		return false;

	int n[6];
	for (int i = 0; i < 6; i++)
		if ((n[i] = parse_hex_nibble (color_name[i + 1])) < 0)
			return false;

	*red = (n[0] << 4) | n[1];
	*green = (n[2] << 4) | n[3];
	*blue = (n[4] << 4) | n[5];
	return true;
}

static int
color_distance_squared (int red_a, int green_a, int blue_a, int red_b, int green_b, int blue_b)
{
	int dr = red_a - red_b;
	int dg = green_a - green_b;
	int db = blue_a - blue_b;
	return dr * dr + dg * dg + db * db;
}

int
color_rgb_to_16 (int red, int green, int blue)
{
	static const int reds[] = { 0, 205, 0, 205, 0, 205, 0, 229, 127, 255, 0, 255, 92, 255, 0, 255 };
	static const int greens[] = { 0, 0, 205, 205, 0, 0, 205, 229, 127, 0, 255, 255, 92, 0, 255, 255 };
	static const int blues[] = { 0, 0, 0, 0, 238, 205, 205, 229, 127, 0, 0, 0, 255, 255, 255, 255 };

	int best_color = 0;
	int best_distance = INT_MAX;
	for (int i = 0; i < 16; i++) {
		int distance = color_distance_squared (red, green, blue, reds[i], greens[i], blues[i]);
		if (distance < best_distance) {
			best_distance = distance;
			best_color = i;
		}
	}

	return best_color;
}

static const int cube_levels[] = { 0, 95, 135, 175, 215, 255 };

static int
nearest_xterm_cube_level (int component)
{
	int best_index = 0;
	int best_distance = INT_MAX;
	for (int i = 0; i < 6; i++) {
		int distance = (component - cube_levels[i]) * (component - cube_levels[i]);
		if (distance < best_distance) {
			best_distance = distance;
			best_index = i;
		}
	}
	return best_index;
}

int
color_rgb_to_256 (int red, int green, int blue)
{
	int red_index = nearest_xterm_cube_level (red);
	int green_index = nearest_xterm_cube_level (green);
	int blue_index = nearest_xterm_cube_level (blue);
	int cube_color = 16 + 36 * red_index + 6 * green_index + blue_index;
	int cube_distance = color_distance_squared (red, green, blue, cube_levels[red_index],
						    cube_levels[green_index], cube_levels[blue_index]);

	int grayscale_index = 0;
	int grayscale_distance = INT_MAX;
	for (int i = 0; i < 24; i++) {
		int value = 8 + 10 * i;
		int distance = color_distance_squared (red, green, blue, value, value, value);
		if (distance < grayscale_distance) {
			grayscale_distance = distance;
			grayscale_index = i;
		}
	}

	if (grayscale_distance < cube_distance)
		return 232 + grayscale_index;
	return cube_color;
}

static int
lookup_named_terminal_color (const char *color_name)
{
	static const struct {
		const char *name;
		int color;
	} names[] = {
		{ "default", TERM_COLOR_DEFAULT },
		{ "black", 0 }, { "red", 1 }, { "green", 2 }, { "yellow", 3 },
		{ "blue", 4 }, { "magenta", 5 }, { "cyan", 6 }, { "white", 7 },
		{ "brightblack", 8 }, { "gray", 8 }, { "grey", 8 },
		{ "brightred", 9 }, { "brightgreen", 10 }, { "brightyellow", 11 },
		{ "brightblue", 12 }, { "brightmagenta", 13 }, { "brightcyan", 14 },
		{ "brightwhite", 15 },
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strcmp (names[i].name, color_name) == 0)
			return names[i].color;
	return TERM_COLOR_UNSPECIFIED;
}

static int
lookup_terminal_color (const char *color_name, const TerminalCapabilities *capabilities)
{
	int named_color = lookup_named_terminal_color (color_name);
	if (named_color != TERM_COLOR_UNSPECIFIED)
		return named_color;

	int red, green, blue;
	if (!parse_hex_color (color_name, &red, &green, &blue))
		return TERM_COLOR_UNSPECIFIED;

	if (capabilities->truecolor)
		return VCOLOR_RGB (red, green, blue);
	if (capabilities->color_count >= 256)
		return color_rgb_to_256 (red, green, blue);
	return color_rgb_to_16 (red, green, blue);
}

static int
normalize_terminal_color (int color, bool is_foreground,
			  const TerminalCapabilities *capabilities, TerminalStyle *style)
{
	if (color == TERM_COLOR_DEFAULT)
		return capabilities->supports_default_colors ? color : TERM_COLOR_UNSPECIFIED;

	if (color < 0)
		return TERM_COLOR_UNSPECIFIED;

	if (!capabilities->has_colors || capabilities->color_count <= 0)
		return TERM_COLOR_UNSPECIFIED;

	if (capabilities->color_count >= 16)
		return color;

	if (capabilities->color_count >= 8) {
		if (color >= 16)
			return TERM_COLOR_UNSPECIFIED;
		if (color >= 8) {
			if (is_foreground)
				style->bold = true;
			return color - 8;
		}
		return color;
	}

	return TERM_COLOR_UNSPECIFIED;
}

static void
apply_mono_fallback (const char *face_name, const TerminalCapabilities *capabilities,
		     TerminalStyle *style)
{
	static const char *const strong[] = {
		FACE_REGION, FACE_ISEARCH, FACE_MATCH, FACE_TRAILING_WHITESPACE, FACE_MODE_LINE,
		FACE_SHOW_PAREN_MATCH, FACE_SHOW_PAREN_MISMATCH,
	};
	static const char *const subtle[] = {
		FACE_LAZY_HIGHLIGHT, FACE_MODE_LINE_INACTIVE, FACE_MINIBUFFER_PROMPT,
		FACE_LINE_NUMBER_CURRENT_LINE, FACE_ERROR, FACE_WARNING, FACE_SUCCESS,
		FACE_FONT_LOCK_COMMENT, FACE_FONT_LOCK_KEYWORD, FACE_FONT_LOCK_FUNCTION_NAME,
		FACE_FONT_LOCK_TYPE, FACE_FONT_LOCK_CONSTANT, FACE_FONT_LOCK_BUILTIN,
		FACE_FONT_LOCK_PREPROCESSOR, FACE_FONT_LOCK_WARNING,
		FACE_MARKDOWN_INLINE_CODE, FACE_MARKDOWN_CODE,
	};

	for (size_t i = 0; i < sizeof strong / sizeof strong[0]; i++)
		if (strcmp (face_name, strong[i]) == 0) {
			if (capabilities->supports_reverse)
				style->reverse = true;
			else if (capabilities->supports_underline)
				style->underline = true;
			else
				style->bold = true;
			return;
		}
	for (size_t i = 0; i < sizeof subtle / sizeof subtle[0]; i++)
		if (strcmp (face_name, subtle[i]) == 0) {
			if (capabilities->supports_underline)
				style->underline = true;
			else
				style->bold = true;
			return;
		}
	if (strcmp (face_name, FACE_FONT_LOCK_STRING) == 0 && capabilities->supports_underline)
		style->underline = true;
}

bool
resolve_terminal_style (const char *face_name, TerminalStyle *style)
{
	const TerminalCapabilities *capabilities = term_get_capabilities ();
	const Theme *theme = current_theme;

	if (!face_name_exists (face_name))
		return false;

	ResolvedFace resolved;
	memset (&resolved, 0, sizeof resolved);
	const char *seen[MAX_INHERIT_DEPTH];
	if (!resolve_face_into (theme, face_name, seen, 0, &resolved))
		return false;

	bool prefer_light_defaults = theme != NULL && theme->variant != NULL &&
		strcmp (theme->variant, "light") == 0;

	memset (style, 0, sizeof *style);
	style->foreground = style->background = TERM_COLOR_UNSPECIFIED;
	style->default_foreground_fallback = prefer_light_defaults ? TERM_COLOR_BLACK : TERM_COLOR_WHITE;
	style->default_background_fallback = prefer_light_defaults ? TERM_COLOR_WHITE : TERM_COLOR_BLACK;
	style->bold = resolved.bold;
	style->underline = capabilities->supports_underline && resolved.underline;
	style->reverse = capabilities->supports_reverse && resolved.reverse;
	style->italic = resolved.italic;
	style->strike = resolved.strike;

	if (!capabilities->has_colors) {
		apply_mono_fallback (face_name, capabilities, style);
		return true;
	}

	if (resolved.has_foreground && resolved.foreground != NULL) {
		int fg = normalize_terminal_color (lookup_terminal_color (resolved.foreground, capabilities),
						   true, capabilities, style);
		if (fg != TERM_COLOR_UNSPECIFIED) {
			style->foreground = fg;
			style->has_foreground = true;
		}
	}

	if (resolved.has_background && resolved.background != NULL) {
		int bg = normalize_terminal_color (lookup_terminal_color (resolved.background, capabilities),
						   false, capabilities, style);
		if (bg != TERM_COLOR_UNSPECIFIED) {
			style->background = bg;
			style->has_background = true;
		}
	}

	return true;
}

bool
resolve_terminal_faces (const char *const *faces, size_t n, TerminalStyle *style)
{
	TerminalStyle dflt;
	bool have_dflt = resolve_terminal_style (FACE_DEFAULT, &dflt), any = false;
	for (size_t i = 0; i < n; i++) {
		TerminalStyle st;
		if (faces[i] == NULL || !resolve_terminal_style (faces[i], &st))
			continue;
		if (!any) {
			*style = st;
			any = true;
			continue;
		}
		/* Colours count where they are not the default face's, which
		   every face inherits. */
		if (st.has_foreground && !(have_dflt && dflt.has_foreground && dflt.foreground == st.foreground)) {
			style->foreground = st.foreground;
			style->has_foreground = true;
		}
		if (st.has_background && !(have_dflt && dflt.has_background && dflt.background == st.background)) {
			style->background = st.background;
			style->has_background = true;
		}
		style->bold |= st.bold;
		style->underline |= st.underline;
		style->reverse |= st.reverse;
		style->italic |= st.italic;
		style->strike |= st.strike;
	}
	return any;
}

/* Tri-state booleans for face definitions. */
enum { U = -1, F = 0, T = 1 };

static void
set_face_slant (Theme *theme, const char *name, int italic, int strike)
{
	FaceSpec *face = map_get (theme->faces, name);
	if (face == NULL)
		return;
	if (italic != U) {
		face->italic = italic;
		face->has_italic = true;
	}
	if (strike != U) {
		face->strike = strike;
		face->has_strike = true;
	}
}

static void
set_builtin_face (Theme *theme, const char *name, const char *foreground,
		  const char *background, const char *inherit_name, int bold, int underline, int reverse)
{
	FaceSpec *face = face_spec_new (name);
	if (foreground != NULL) {
		face->foreground = xstrdup (foreground);
		face->has_foreground = true;
	}
	if (background != NULL) {
		face->background = xstrdup (background);
		face->has_background = true;
	}
	face->inherit_name = xstrdup_or_null (inherit_name);
	if (bold != U) {
		face->bold = bold;
		face->has_bold = true;
	}
	if (underline != U) {
		face->underline = underline;
		face->has_underline = true;
	}
	if (reverse != U) {
		face->reverse = reverse;
		face->has_reverse = true;
	}
	free (theme_set_face (theme, face));
}

/* The colours of a theme's Markdown faces: headings of levels 1, 2, 3
   and deeper; inline code and its background; the background of code
   blocks; links, markup (delimiters, table rules), quotes and list
   markers. NULL leaves a colour to the default face. */
typedef struct {
	const char *heading[4];
	const char *code, *code_bg, *block_bg, *link, *markup, *quote, *list;
} MarkdownColors;

static void
set_markdown_faces (Theme *t, const MarkdownColors *c)
{
	static const char *const headings[] = {
		FACE_MARKDOWN_HEADER_1, FACE_MARKDOWN_HEADER_2, FACE_MARKDOWN_HEADER_3,
		FACE_MARKDOWN_HEADER_4, FACE_MARKDOWN_HEADER_5, FACE_MARKDOWN_HEADER_6,
	};
	for (size_t i = 0; i < 6; i++)
		set_builtin_face (t, headings[i], c->heading[MIN (i, 3)], NULL, NULL, T, i == 0 ? T : U, U);
	set_builtin_face (t, FACE_MARKDOWN_BOLD, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MARKDOWN_ITALIC, NULL, NULL, NULL, U, U, U);
	set_face_slant (t, FACE_MARKDOWN_ITALIC, T, U);
	set_builtin_face (t, FACE_MARKDOWN_STRIKE, NULL, NULL, NULL, U, U, U);
	set_face_slant (t, FACE_MARKDOWN_STRIKE, U, T);
	set_builtin_face (t, FACE_MARKDOWN_INLINE_CODE, c->code, c->code_bg, NULL, U, U, U);
	set_builtin_face (t, FACE_MARKDOWN_CODE, c->code, c->block_bg, NULL, U, U, U);
	set_builtin_face (t, FACE_MARKDOWN_LINK, c->link, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_MARKDOWN_MARKUP, c->markup, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MARKDOWN_BLOCKQUOTE, c->quote, NULL, NULL, U, U, U);
	set_face_slant (t, FACE_MARKDOWN_BLOCKQUOTE, T, U);
	set_builtin_face (t, FACE_MARKDOWN_LIST, c->list, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MARKDOWN_TABLE, c->markup, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MARKDOWN_HR, c->markup, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MARKDOWN_TASK_DONE, c->markup, NULL, NULL, U, U, U);
	set_face_slant (t, FACE_MARKDOWN_TASK_DONE, U, T);
}

/* The colours of a theme's toolkit faces (see tui.c): the panel
   background, borders, titles, muted text and key names; the
   background of a hovered row; the scroll bar track (a background) and
   thumb, normal and hovered; a hovered window divider; and the
   background of a hovered part of the mode line. NULL leaves a colour
   out; the light and dark themes of few colours underline instead. */
typedef struct {
	const char *panel, *border, *title, *muted, *key, *hover;
	const char *track, *thumb, *thumb_hover, *divider_hover, *mode_line_highlight;
} TuiColors;

static void
set_tui_faces (Theme *t, const TuiColors *c)
{
	set_builtin_face (t, FACE_TUI_PANEL, NULL, c->panel, NULL, U, U, U);
	set_builtin_face (t, FACE_TUI_BORDER, c->border, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_TUI_TITLE, c->title, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_TUI_MUTED, c->muted, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_TUI_KEY, c->key, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_TUI_HOVER, NULL, c->hover, NULL, U, c->hover == NULL ? T : U, U);
	set_builtin_face (t, FACE_TUI_SCROLLBAR_TRACK, c->muted, c->track, NULL, U, U, U);
	set_builtin_face (t, FACE_TUI_SCROLLBAR_THUMB, c->thumb, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_TUI_SCROLLBAR_THUMB_HOVER, c->thumb_hover, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_TUI_DIVIDER_HOVER, c->divider_hover, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MODE_LINE_HIGHLIGHT, NULL, c->mode_line_highlight, NULL, U,
			  c->mode_line_highlight == NULL ? T : U, U);
}

static void
define_default_dark_theme (void)
{
	Theme *t = define_theme (THEME_DEFAULT_DARK, "dark");

	set_builtin_face (t, FACE_DEFAULT, "default", "default", NULL, U, U, U);
	set_builtin_face (t, FACE_REGION, NULL, "blue", NULL, U, U, U);
	set_builtin_face (t, FACE_MODE_LINE, "black", "cyan", NULL, T, U, U);
	set_builtin_face (t, FACE_MODE_LINE_INACTIVE, "white", "blue", FACE_MODE_LINE, F, U, U);
	set_builtin_face (t, FACE_MINIBUFFER_PROMPT, "cyan", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_VERTICAL_BORDER, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER_CURRENT_LINE, "white", NULL, FACE_LINE_NUMBER, T, U, U);
	set_builtin_face (t, FACE_TRAILING_WHITESPACE, NULL, "red", NULL, U, U, U);
	set_builtin_face (t, FACE_ISEARCH, "black", "yellow", NULL, T, U, U);
	set_builtin_face (t, FACE_LAZY_HIGHLIGHT, NULL, "blue", NULL, U, U, U);
	set_builtin_face (t, FACE_MATCH, "black", "green", NULL, U, U, U);
	set_builtin_face (t, FACE_ERROR, "red", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_WARNING, "yellow", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_SUCCESS, "green", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_COMMENT, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_STRING, "green", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_KEYWORD, "cyan", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_FUNCTION_NAME, "blue", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_VARIABLE_NAME, "default", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_TYPE, "magenta", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_CONSTANT, "magenta", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_BUILTIN, "cyan", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_PREPROCESSOR, "yellow", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_WARNING, NULL, NULL, FACE_WARNING, U, U, U);
	set_builtin_face (t, FACE_MAGIT_SECTION_HEADING, "yellow", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_FILE_HEADING, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_HUNK_HEADING, "white", "brightblack", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_ADDED, "green", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_REMOVED, "red", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_HASH, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_LOCAL, "cyan", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_REMOTE, "green", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_LOG_DATE, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_SELECTION, NULL, "brightblack", NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_MATCH, "yellow", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_ANNOTATION, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_GROUP, "magenta", NULL, NULL, T, T, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MATCH, "brightwhite", "magenta", NULL, T, U, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MISMATCH, "brightwhite", "red", NULL, T, U, U);
	set_builtin_face (t, FACE_LINK, "cyan", NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_LINK_HOVER, NULL, "brightblack", NULL, U, T, U);
	set_builtin_face (t, FACE_TOOLTIP, "brightwhite", "brightblack", NULL, U, U, U);
	set_tui_faces (t, &(TuiColors) { NULL, "brightblack", "brightwhite", "brightblack", "cyan", NULL,
		NULL, "brightblack", "white", "cyan", NULL });
	set_markdown_faces (t, &(MarkdownColors) {
		{ "cyan", "magenta", "yellow", "green" }, "green", NULL, NULL, "cyan", "brightblack",
		"brightblack", "yellow" });
}

static void
define_default_light_theme (void)
{
	Theme *t = define_theme (THEME_DEFAULT_LIGHT, "light");

	set_builtin_face (t, FACE_DEFAULT, "black", "white", NULL, U, U, U);
	set_builtin_face (t, FACE_REGION, "black", "cyan", NULL, U, U, U);
	set_builtin_face (t, FACE_MODE_LINE, "white", "blue", NULL, T, U, U);
	set_builtin_face (t, FACE_MODE_LINE_INACTIVE, "black", "white", FACE_MODE_LINE, F, U, U);
	set_builtin_face (t, FACE_MINIBUFFER_PROMPT, "blue", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_VERTICAL_BORDER, "blue", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER, "blue", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER_CURRENT_LINE, "black", NULL, FACE_LINE_NUMBER, T, U, U);
	set_builtin_face (t, FACE_TRAILING_WHITESPACE, NULL, "red", NULL, U, U, U);
	set_builtin_face (t, FACE_ISEARCH, "black", "yellow", NULL, T, U, U);
	set_builtin_face (t, FACE_LAZY_HIGHLIGHT, NULL, "yellow", NULL, U, U, U);
	set_builtin_face (t, FACE_MATCH, "white", "green", NULL, U, U, U);
	set_builtin_face (t, FACE_ERROR, "red", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_WARNING, "magenta", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_SUCCESS, "green", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_COMMENT, "magenta", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_STRING, "green", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_KEYWORD, "blue", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_FUNCTION_NAME, "blue", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_VARIABLE_NAME, "default", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_TYPE, "magenta", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_CONSTANT, "magenta", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_BUILTIN, "blue", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_PREPROCESSOR, "red", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_WARNING, NULL, NULL, FACE_WARNING, U, U, U);
	set_builtin_face (t, FACE_MAGIT_SECTION_HEADING, "magenta", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_FILE_HEADING, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_HUNK_HEADING, "black", "brightwhite", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_ADDED, "green", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_REMOVED, "red", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_HASH, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_LOCAL, "blue", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_REMOTE, "green", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_LOG_DATE, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_SELECTION, "black", "cyan", NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_MATCH, "yellow", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_ANNOTATION, "brightblack", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_GROUP, "magenta", NULL, NULL, T, T, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MATCH, "white", "magenta", NULL, T, U, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MISMATCH, "white", "red", NULL, T, U, U);
	set_builtin_face (t, FACE_LINK, "blue", NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_LINK_HOVER, NULL, "cyan", NULL, U, T, U);
	set_builtin_face (t, FACE_TOOLTIP, "black", "yellow", NULL, U, U, U);
	set_tui_faces (t, &(TuiColors) { NULL, "blue", "black", "blue", "blue", NULL,
		NULL, "blue", "black", "blue", NULL });
	set_markdown_faces (t, &(MarkdownColors) {
		{ "blue", "magenta", "red", "green" }, "green", NULL, NULL, "blue", "brightblack",
		"brightblack", "red" });
}

static void
define_terminal_default_theme (void)
{
	Theme *t = define_theme (THEME_TERMINAL_DEFAULT, NULL);

	set_builtin_face (t, FACE_DEFAULT, "default", "default", NULL, U, U, U);
	set_builtin_face (t, FACE_REGION, NULL, NULL, NULL, U, U, T);
	set_builtin_face (t, FACE_MODE_LINE, NULL, NULL, NULL, T, U, T);
	set_builtin_face (t, FACE_MODE_LINE_INACTIVE, NULL, NULL, FACE_MODE_LINE, F, T, U);
	set_builtin_face (t, FACE_MINIBUFFER_PROMPT, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_VERTICAL_BORDER, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER_CURRENT_LINE, NULL, NULL, FACE_LINE_NUMBER, T, U, U);
	set_builtin_face (t, FACE_TRAILING_WHITESPACE, NULL, NULL, NULL, U, U, T);
	set_builtin_face (t, FACE_ISEARCH, NULL, NULL, NULL, T, U, T);
	set_builtin_face (t, FACE_LAZY_HIGHLIGHT, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_MATCH, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_ERROR, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_WARNING, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_SUCCESS, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_COMMENT, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_STRING, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_FONT_LOCK_KEYWORD, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_FUNCTION_NAME, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_VARIABLE_NAME, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_TYPE, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_FONT_LOCK_CONSTANT, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_BUILTIN, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_PREPROCESSOR, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_FONT_LOCK_WARNING, NULL, NULL, FACE_WARNING, U, U, U);
	set_builtin_face (t, FACE_MAGIT_SECTION_HEADING, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_FILE_HEADING, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_HUNK_HEADING, NULL, NULL, NULL, U, U, T);
	set_builtin_face (t, FACE_MAGIT_DIFF_ADDED, "green", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_REMOVED, "red", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_HASH, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_LOCAL, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_REMOTE, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_MAGIT_LOG_DATE, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_SELECTION, NULL, NULL, NULL, T, U, T);
	set_builtin_face (t, FACE_PICKER_MATCH, NULL, NULL, NULL, T, T, U);
	set_builtin_face (t, FACE_PICKER_ANNOTATION, NULL, NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_GROUP, NULL, NULL, NULL, T, T, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MATCH, NULL, NULL, NULL, T, T, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MISMATCH, NULL, NULL, NULL, T, U, T);
	set_builtin_face (t, FACE_LINK, NULL, NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_LINK_HOVER, NULL, NULL, NULL, U, T, T);
	set_builtin_face (t, FACE_TOOLTIP, NULL, NULL, NULL, U, U, T);
	set_tui_faces (t, &(TuiColors) { NULL, NULL, NULL, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL, NULL });
	set_markdown_faces (t, &(MarkdownColors) { { NULL, NULL, NULL, NULL }, NULL, NULL, NULL,
		NULL, NULL, NULL, NULL });
}

/* Themes in hex colours for truecolor displays such as the GUI; other
   terminals get the nearest palette colours. */
static void
define_xzile_dark_theme (void)
{
	Theme *t = define_theme (THEME_XZILE_DARK, "dark");

	set_builtin_face (t, FACE_DEFAULT, "#d5d9e0", "#17191e", NULL, U, U, U);
	set_builtin_face (t, FACE_CURSOR, NULL, "#82aaff", NULL, U, U, U);
	set_builtin_face (t, FACE_REGION, NULL, "#2d3a55", NULL, U, U, U);
	set_builtin_face (t, FACE_MODE_LINE, "#e3e7ee", "#272b35", NULL, U, U, U);
	set_builtin_face (t, FACE_MODE_LINE_INACTIVE, "#7c8494", "#1d2026", FACE_MODE_LINE, U, U, U);
	set_builtin_face (t, FACE_MINIBUFFER_PROMPT, "#82aaff", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_VERTICAL_BORDER, "#2b2f39", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER, "#4a505e", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER_CURRENT_LINE, "#c3c9d4", NULL, FACE_LINE_NUMBER, U, U, U);
	set_builtin_face (t, FACE_TRAILING_WHITESPACE, NULL, "#5c2b33", NULL, U, U, U);
	set_builtin_face (t, FACE_ISEARCH, "#17191e", "#ffc777", NULL, T, U, U);
	set_builtin_face (t, FACE_LAZY_HIGHLIGHT, NULL, "#3b4463", NULL, U, U, U);
	set_builtin_face (t, FACE_MATCH, "#17191e", "#a6da95", NULL, U, U, U);
	set_builtin_face (t, FACE_ERROR, "#ff7a93", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_WARNING, "#ffc777", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_SUCCESS, "#a6da95", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_COMMENT, "#5f6779", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_STRING, "#a6da95", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_KEYWORD, "#c099ff", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_FUNCTION_NAME, "#82aaff", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_VARIABLE_NAME, "#d5d9e0", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_TYPE, "#65d1e6", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_CONSTANT, "#ff9e64", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_BUILTIN, "#86e1fc", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_PREPROCESSOR, "#f5a97f", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_WARNING, NULL, NULL, FACE_WARNING, U, U, U);
	set_builtin_face (t, FACE_MAGIT_SECTION_HEADING, "#ffc777", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_FILE_HEADING, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_HUNK_HEADING, "#aab1c0", "#272b35", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_ADDED, "#a6da95", "#1f2b22", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_REMOVED, "#ff7a93", "#2e1d22", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_HASH, "#5f6779", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_LOCAL, "#86e1fc", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_REMOTE, "#a6da95", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_LOG_DATE, "#5f6779", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_SELECTION, NULL, "#2a3142", NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_MATCH, "#ffc777", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_ANNOTATION, "#5f6779", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_GROUP, "#c099ff", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MATCH, "#ffffff", "#3e5687", NULL, T, U, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MISMATCH, "#ffffff", "#c53b53", NULL, T, U, U);
	set_builtin_face (t, FACE_LINK, "#82aaff", NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_LINK_HOVER, NULL, "#2a3142", NULL, U, T, U);
	set_builtin_face (t, FACE_TOOLTIP, "#e3e7ee", "#2b303b", NULL, U, U, U);
	set_tui_faces (t, &(TuiColors) { "#1b1e24", "#3a4050", "#e3e7ee", "#6b7385", "#82aaff", "#232731",
		"#202329", "#4a5163", "#7c8598", "#82aaff", "#363c4a" });
	set_markdown_faces (t, &(MarkdownColors) {
		{ "#82aaff", "#c099ff", "#65d1e6", "#ffc777" }, "#f5a97f", "#252933", "#1e2128",
		"#82aaff", "#5f6779", "#9aa3b2", "#ff9e64" });
}

static void
define_xzile_light_theme (void)
{
	Theme *t = define_theme (THEME_XZILE_LIGHT, "light");

	set_builtin_face (t, FACE_DEFAULT, "#383a42", "#fbfbfc", NULL, U, U, U);
	set_builtin_face (t, FACE_CURSOR, NULL, "#3b6fe0", NULL, U, U, U);
	set_builtin_face (t, FACE_REGION, NULL, "#d6e2fb", NULL, U, U, U);
	set_builtin_face (t, FACE_MODE_LINE, "#2b2d33", "#e8eaef", NULL, U, U, U);
	set_builtin_face (t, FACE_MODE_LINE_INACTIVE, "#8a8f99", "#f1f2f5", FACE_MODE_LINE, U, U, U);
	set_builtin_face (t, FACE_MINIBUFFER_PROMPT, "#3b6fe0", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_VERTICAL_BORDER, "#e1e3e8", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER, "#b4b8c1", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_LINE_NUMBER_CURRENT_LINE, "#4b4f58", NULL, FACE_LINE_NUMBER, U, U, U);
	set_builtin_face (t, FACE_TRAILING_WHITESPACE, NULL, "#f9d4d8", NULL, U, U, U);
	set_builtin_face (t, FACE_ISEARCH, "#2b2d33", "#ffd27a", NULL, T, U, U);
	set_builtin_face (t, FACE_LAZY_HIGHLIGHT, NULL, "#fdecc0", NULL, U, U, U);
	set_builtin_face (t, FACE_MATCH, "#2b2d33", "#c8ecc0", NULL, U, U, U);
	set_builtin_face (t, FACE_ERROR, "#d6304b", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_WARNING, "#b86e00", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_SUCCESS, "#3a8a2e", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_COMMENT, "#9da2ad", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_STRING, "#3a8a2e", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_KEYWORD, "#a626a4", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_FUNCTION_NAME, "#3b6fe0", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_VARIABLE_NAME, "#383a42", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_TYPE, "#0f7f8f", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_CONSTANT, "#c4570d", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_BUILTIN, "#0b7fbf", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_PREPROCESSOR, "#b86e00", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_FONT_LOCK_WARNING, NULL, NULL, FACE_WARNING, U, U, U);
	set_builtin_face (t, FACE_MAGIT_SECTION_HEADING, "#a626a4", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_FILE_HEADING, NULL, NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_HUNK_HEADING, "#4b4f58", "#eceef2", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_ADDED, "#2f7a24", "#e6f6e2", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_DIFF_REMOVED, "#c42b44", "#fbe7ea", NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_HASH, "#9da2ad", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_LOCAL, "#0b7fbf", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_BRANCH_REMOTE, "#3a8a2e", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_MAGIT_LOG_DATE, "#9da2ad", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_SELECTION, NULL, "#e3e9f8", NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_MATCH, "#c4570d", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_PICKER_ANNOTATION, "#9da2ad", NULL, NULL, U, U, U);
	set_builtin_face (t, FACE_PICKER_GROUP, "#a626a4", NULL, NULL, T, U, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MATCH, "#2b2d33", "#c9d8fb", NULL, T, U, U);
	set_builtin_face (t, FACE_SHOW_PAREN_MISMATCH, "#ffffff", "#d6304b", NULL, T, U, U);
	set_builtin_face (t, FACE_LINK, "#3b6fe0", NULL, NULL, U, T, U);
	set_builtin_face (t, FACE_LINK_HOVER, NULL, "#e3e9f8", NULL, U, T, U);
	set_builtin_face (t, FACE_TOOLTIP, "#2b2d33", "#eceef2", NULL, U, U, U);
	set_tui_faces (t, &(TuiColors) { "#f4f5f8", "#d3d6dd", "#2b2d33", "#8f95a1", "#3b6fe0", "#eceef3",
		"#eef0f3", "#c5c9d1", "#9097a3", "#3b6fe0", "#d9dde5" });
	set_markdown_faces (t, &(MarkdownColors) {
		{ "#3b6fe0", "#a626a4", "#0f7f8f", "#b86e00" }, "#c4570d", "#eceef2", "#f3f4f7",
		"#3b6fe0", "#9da2ad", "#696c77", "#c4570d" });
}

static char *
minibuf_read_theme_name (const char *fmt, ...)
{
	Completion *cp = completion_new (false);
	MapIter it;
	const char *name;
	map_iter_init (&it);
	while (map_iter_next (theme_table, &it, &name, NULL))
		sv_add_sorted (&cp->completions, name);

	va_list ap;
	va_start (ap, fmt);
	char *prompt = xvasprintf (fmt, ap);
	va_end (ap);
	char *ms = picker_read_name (prompt, &cp->completions, NULL, NULL, true, -1);
	free (prompt);
	completion_free (cp);
	return ms;
}

static bool
is_lisp_nil (const char *value)
{
	return strcmp (value, "nil") == 0;
}

static bool
is_lisp_unspecified (const char *value)
{
	return strcmp (value, "unspecified") == 0;
}

static FaceSpec *
copy_theme_face_for_edit (const Theme *theme, const char *face_name, char **err)
{
	*err = NULL;
	if (!face_name_exists (face_name)) {
		*err = xasprintf ("Unknown face `%s'", face_name);
		return NULL;
	}

	FaceSpec *face = face_spec_new (face_name);
	const FaceSpec *existing = map_get (theme->faces, face_name);
	if (existing == NULL)
		return face;

	face->inherit_name = xstrdup_or_null (existing->inherit_name);
	face->foreground = xstrdup_or_null (existing->foreground);
	face->background = xstrdup_or_null (existing->background);
	face->bold = existing->bold;
	face->underline = existing->underline;
	face->reverse = existing->reverse;
	face->italic = existing->italic;
	face->strike = existing->strike;
	face->has_foreground = existing->has_foreground;
	face->has_background = existing->has_background;
	face->has_bold = existing->has_bold;
	face->has_underline = existing->has_underline;
	face->has_reverse = existing->has_reverse;
	face->has_italic = existing->has_italic;
	face->has_strike = existing->has_strike;
	return face;
}

static bool
apply_bool_attribute (const char *attr_name, const char *attr_value,
		      bool *value, bool *has, char **err)
{
	if (is_lisp_unspecified (attr_value))
		*has = false;
	else if (strcmp (attr_value, "t") == 0) {
		*value = true;
		*has = true;
	} else if (is_lisp_nil (attr_value)) {
		*value = false;
		*has = true;
	} else {
		*err = xasprintf ("Face attribute `%s' must be t, nil, or unspecified", attr_name);
		return false;
	}
	return true;
}

static bool
apply_face_attribute (FaceSpec *face, const char *attr_name, const char *attr_value, char **err)
{
	bool clear = is_lisp_nil (attr_value) || is_lisp_unspecified (attr_value);

	if (strcmp (attr_name, ":foreground") == 0 || strcmp (attr_name, ":fg") == 0) {
		set_string (&face->foreground, clear ? NULL : attr_value);
		face->has_foreground = !clear;
	} else if (strcmp (attr_name, ":background") == 0 || strcmp (attr_name, ":bg") == 0) {
		set_string (&face->background, clear ? NULL : attr_value);
		face->has_background = !clear;
	} else if (strcmp (attr_name, ":inherit") == 0)
		set_string (&face->inherit_name, clear ? NULL : attr_value);
	else if (strcmp (attr_name, ":bold") == 0)
		return apply_bool_attribute (attr_name, attr_value, &face->bold, &face->has_bold, err);
	else if (strcmp (attr_name, ":underline") == 0)
		return apply_bool_attribute (attr_name, attr_value, &face->underline, &face->has_underline, err);
	else if (strcmp (attr_name, ":reverse") == 0)
		return apply_bool_attribute (attr_name, attr_value, &face->reverse, &face->has_reverse, err);
	else if (strcmp (attr_name, ":italic") == 0)
		return apply_bool_attribute (attr_name, attr_value, &face->italic, &face->has_italic, err);
	else if (strcmp (attr_name, ":strike-through") == 0)
		return apply_bool_attribute (attr_name, attr_value, &face->strike, &face->has_strike, err);
	else {
		*err = xasprintf ("Unknown face attribute `%s'", attr_name);
		return false;
	}
	return true;
}

static bool
apply_face_attributes_from_args (FaceSpec *face, Args *args, char **err)
{
	*err = NULL;
	while (!args_empty (args)) {
		const char *attr_name = args_poll (args);
		const char *attr_value = args_poll (args);
		if (attr_name == NULL || attr_value == NULL) {
			*err = xasprintf ("Face attribute `%s' requires a value",
					  attr_name != NULL ? attr_name : "(null)");
			return false;
		}
		if (!apply_face_attribute (face, attr_name, attr_value, err))
			return false;
	}
	return true;
}

static void
refresh_theme_display (void)
{
	if (cur_wp == NULL)
		return;

	term_clear ();
	term_redisplay ();
	term_refresh ();
}

static bool
F_define_theme (long uniarg, Args *args)
{
	(void) uniarg;
	const char *theme_name = args_poll (args);
	if (theme_name == NULL) {
		minibuf_error ("define-theme requires a theme name");
		return false;
	}

	char *variant = NULL;
	while (!args_empty (args)) {
		const char *attr_name = args_poll (args);
		const char *attr_value = args_poll (args);
		if (attr_name == NULL || attr_value == NULL) {
			minibuf_error ("Theme attribute `%s' requires a value",
				       attr_name != NULL ? attr_name : "(null)");
			free (variant);
			return false;
		}

		if (strcmp (attr_name, ":variant") == 0) {
			if (is_lisp_nil (attr_value) || is_lisp_unspecified (attr_value))
				set_string (&variant, NULL);
			else if (strcmp (attr_value, "light") == 0 || strcmp (attr_value, "dark") == 0)
				set_string (&variant, attr_value);
			else {
				minibuf_error ("Theme attribute `%s' must be light, dark, nil, or unspecified",
					       attr_name);
				free (variant);
				return false;
			}
		} else {
			minibuf_error ("Unknown theme attribute `%s'", attr_name);
			free (variant);
			return false;
		}
	}

	Theme *previously_active_theme = current_theme;
	bool preserve_active_display = previously_active_theme != NULL &&
		strcmp (previously_active_theme->name, theme_name) == 0;

	define_theme (theme_name, variant);
	free (variant);
	if (preserve_active_display)
		current_theme = previously_active_theme;
	return true;
}

static bool
F_load_theme (long uniarg, Args *args)
{
	(void) uniarg;
	char *theme_name = xstrdup_or_null (args_poll (args));
	if (theme_name == NULL)
		theme_name = minibuf_read_theme_name ("Load theme: ");
	if (theme_name == NULL)
		return false;
	if (!activate_theme (theme_name)) {
		minibuf_error ("Undefined theme `%s'", theme_name);
		free (theme_name);
		return false;
	}

	refresh_theme_display ();
	minibuf_write ("Loaded theme `%s'", theme_name);
	free (theme_name);
	return true;
}

static bool
set_face_in_theme (Theme *theme, const char *face_name, Args *args)
{
	char *err;
	FaceSpec *face = copy_theme_face_for_edit (theme, face_name, &err);
	if (face != NULL) {
		if (apply_face_attributes_from_args (face, args, &err))
			err = theme_set_face (theme, face);
		if (err != NULL)
			face_spec_free (face);
	}

	if (err != NULL) {
		minibuf_error ("%s", err);
		free (err);
		return false;
	}
	return true;
}

static bool
F_set_face_attribute (long uniarg, Args *args)
{
	(void) uniarg;
	if (args_empty (args)) {
		minibuf_error ("set-face-attribute requires a face name");
		return false;
	}

	Theme *theme = current_theme;
	if (theme == NULL) {
		minibuf_error ("No active theme");
		return false;
	}

	const char *face_name = args_poll (args);
	if (face_name == NULL) {
		minibuf_error ("set-face-attribute requires a face name");
		return false;
	}

	if (!set_face_in_theme (theme, face_name, args))
		return false;

	refresh_theme_display ();
	return true;
}

static bool
F_theme_set_face (long uniarg, Args *args)
{
	(void) uniarg;
	const char *theme_name = args_poll (args);
	if (theme_name == NULL) {
		minibuf_error ("theme-set-face requires a theme name");
		return false;
	}

	Theme *theme = map_get (theme_table, theme_name);
	if (theme == NULL) {
		minibuf_error ("Undefined theme `%s'", theme_name);
		return false;
	}

	const char *face_name = args_poll (args);
	if (face_name == NULL) {
		minibuf_error ("theme-set-face requires a face name");
		return false;
	}

	if (!set_face_in_theme (theme, face_name, args))
		return false;

	if (theme == current_theme)
		refresh_theme_display ();
	return true;
}

void
theme_init (void)
{
	face_name_table = map_new ();
	theme_table = map_new ();
	current_theme = NULL;
	for (size_t i = 0; i < sizeof builtin_faces / sizeof builtin_faces[0]; i++)
		map_set (face_name_table, builtin_faces[i], (void *) builtin_faces[i]);
	define_default_dark_theme ();
	define_default_light_theme ();
	define_terminal_default_theme ();
	define_xzile_dark_theme ();
	define_xzile_light_theme ();
	activate_theme (term_get_capabilities ()->gui ? THEME_XZILE_DARK : THEME_TERMINAL_DEFAULT);

	lisp_func_new ("define-theme", F_define_theme, false,
		       "Define or replace the theme named THEME.\n"
		       "\n"
		       "Arguments are THEME followed by optional :ATTRIBUTE VALUE pairs.\n"
		       "Currently only :variant is supported.");

	lisp_func_new ("load-theme", F_load_theme, true,
		       "Load and enable the theme named THEME.\n"
		       "\n"
		       "Interactively, prompt for one of the registered theme names.");

	lisp_func_new ("set-face-attribute", F_set_face_attribute, false,
		       "Set attribute overrides for FACE in the current theme.\n"
		       "\n"
		       "Arguments are FACE followed by :ATTRIBUTE VALUE pairs such as\n"
		       ":fg, :bg, :bold, :underline, :reverse, :italic, :strike-through,\n"
		       "and :inherit.");

	lisp_func_new ("theme-set-face", F_theme_set_face, false,
		       "Set attribute overrides for FACE in THEME.\n"
		       "\n"
		       "Arguments are THEME, FACE, and then :ATTRIBUTE VALUE pairs such as\n"
		       ":fg, :bg, :bold, :underline, :reverse, :italic, :strike-through,\n"
		       "and :inherit.");
}
