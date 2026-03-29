/* Theme and face registry */

public const string FACE_DEFAULT = "default";
public const string FACE_REGION = "region";
public const string FACE_MODE_LINE = "mode-line";
public const string FACE_MODE_LINE_INACTIVE = "mode-line-inactive";
public const string FACE_MINIBUFFER_PROMPT = "minibuffer-prompt";
public const string FACE_VERTICAL_BORDER = "vertical-border";
public const string FACE_LINE_NUMBER = "line-number";
public const string FACE_LINE_NUMBER_CURRENT_LINE = "line-number-current-line";
public const string FACE_TRAILING_WHITESPACE = "trailing-whitespace";
public const string FACE_ISEARCH = "isearch";
public const string FACE_LAZY_HIGHLIGHT = "lazy-highlight";
public const string FACE_MATCH = "match";
public const string FACE_ERROR = "error";
public const string FACE_WARNING = "warning";
public const string FACE_SUCCESS = "success";
public const string FACE_FONT_LOCK_COMMENT = "font-lock-comment-face";
public const string FACE_FONT_LOCK_STRING = "font-lock-string-face";
public const string FACE_FONT_LOCK_KEYWORD = "font-lock-keyword-face";
public const string FACE_FONT_LOCK_FUNCTION_NAME = "font-lock-function-name-face";
public const string FACE_FONT_LOCK_VARIABLE_NAME = "font-lock-variable-name-face";
public const string FACE_FONT_LOCK_TYPE = "font-lock-type-face";
public const string FACE_FONT_LOCK_CONSTANT = "font-lock-constant-face";
public const string FACE_FONT_LOCK_BUILTIN = "font-lock-builtin-face";
public const string FACE_FONT_LOCK_PREPROCESSOR = "font-lock-preprocessor-face";
public const string FACE_FONT_LOCK_WARNING = "font-lock-warning-face";

public const string THEME_DEFAULT_DARK = "default-dark";
public const string THEME_DEFAULT_LIGHT = "default-light";
public const string THEME_TERMINAL_DEFAULT = "terminal-default";
public const int TERM_COLOR_UNSPECIFIED = -2;
public const int TERM_COLOR_DEFAULT = -1;
public const int TERM_COLOR_BLACK = 0;
public const int TERM_COLOR_WHITE = 7;

public errordomain ThemeError {
	UNKNOWN_FACE,
	INHERITANCE_CYCLE,
}

public class TerminalCapabilities {
	public bool has_colors { get; set; default = false; }
	public bool supports_default_colors { get; set; default = false; }
	public bool supports_reverse { get; set; default = true; }
	public bool supports_underline { get; set; default = true; }
	public int color_count { get; set; default = 0; }
	public int color_pair_count { get; set; default = 0; }
}

public class ResolvedFace {
	public string name { get; private set; }
	public string? foreground;
	public string? background;
	public bool bold;
	public bool underline;
	public bool reverse;
	public bool has_foreground;
	public bool has_background;
	public bool has_bold;
	public bool has_underline;
	public bool has_reverse;

	public ResolvedFace (string name) {
		this.name = name;
	}
}

public class TerminalStyle {
	public string face_name { get; private set; }
	public int foreground = TERM_COLOR_UNSPECIFIED;
	public int background = TERM_COLOR_UNSPECIFIED;
	public int default_foreground_fallback = TERM_COLOR_UNSPECIFIED;
	public int default_background_fallback = TERM_COLOR_UNSPECIFIED;
	public bool bold;
	public bool underline;
	public bool reverse;
	public bool has_foreground;
	public bool has_background;

	public TerminalStyle (string face_name) {
		this.face_name = face_name;
	}
}

public class FaceSpec {
	public string name { get; private set; }
	public string? inherit_name;
	public string? foreground;
	public string? background;
	public bool bold;
	public bool underline;
	public bool reverse;
	public bool has_foreground;
	public bool has_background;
	public bool has_bold;
	public bool has_underline;
	public bool has_reverse;

	public FaceSpec (string name) {
		this.name = name;
	}
}

public class Theme {
	public string name { get; private set; }
	public string? variant;
	private HashTable<string, FaceSpec> faces;

	public Theme (string name, string? variant = null) {
		this.name = name;
		this.variant = variant;
		this.faces = new HashTable<string, FaceSpec> (str_hash, str_equal);
	}

	public void set_face (FaceSpec face) throws ThemeError {
		require_registered_face_name (face.name);
		if (face.inherit_name != null)
			require_registered_face_name ((string) face.inherit_name);
		faces.insert (face.name, face);
	}

	public FaceSpec? lookup_face (string face_name) {
		return faces.lookup (face_name);
	}
}

HashTable<string, string> face_name_table;
HashTable<string, Theme> theme_table;
Theme? current_theme = null;

public void define_face_name (string name) {
	face_name_table.insert (name, name);
}

public bool face_name_exists (string name) {
	return face_name_table.lookup (name) != null;
}

static void require_registered_face_name (string name) throws ThemeError {
	if (!face_name_exists (name))
		throw new ThemeError.UNKNOWN_FACE ("Unknown face `%s'".printf (name));
}

public Theme define_theme (string name, string? variant = null) {
	Theme? old_theme = theme_table.lookup (name);
	Theme theme = new Theme (name, variant);
	theme_table.insert (name, theme);

	if (current_theme == old_theme)
		current_theme = theme;

	return theme;
}

public Theme? lookup_theme (string name) {
	return theme_table.lookup (name);
}

public bool activate_theme (string name) {
	Theme? theme = lookup_theme (name);
	if (theme == null)
		return false;

	current_theme = theme;
	return true;
}

public Theme? get_current_theme () {
	return current_theme;
}

public unowned string? get_current_theme_name () {
	return current_theme != null ? current_theme.name : null;
}

static void merge_face_spec (ResolvedFace resolved, FaceSpec face) {
	if (face.has_foreground) {
		resolved.foreground = face.foreground;
		resolved.has_foreground = true;
	}
	if (face.has_background) {
		resolved.background = face.background;
		resolved.has_background = true;
	}
	if (face.has_bold) {
		resolved.bold = face.bold;
		resolved.has_bold = true;
	}
	if (face.has_underline) {
		resolved.underline = face.underline;
		resolved.has_underline = true;
	}
	if (face.has_reverse) {
		resolved.reverse = face.reverse;
		resolved.has_reverse = true;
	}
}

static void resolve_face_into (Theme? theme,
							   string face_name,
							   HashTable<string, string> seen,
							   ResolvedFace resolved) throws ThemeError {
	if (seen.lookup (face_name) != null)
		throw new ThemeError.INHERITANCE_CYCLE ("Inheritance cycle detected at face `%s'".printf (face_name));
	seen.insert (face_name, face_name);

	FaceSpec? face = theme != null ? theme.lookup_face (face_name) : null;
	string? inherit_name = null;

	if (face != null && face.inherit_name != null)
		inherit_name = face.inherit_name;
	else if (face_name != FACE_DEFAULT)
		inherit_name = FACE_DEFAULT;

	if (inherit_name != null)
		resolve_face_into (theme, inherit_name, seen, resolved);

	if (face != null)
		merge_face_spec (resolved, face);

	seen.remove (face_name);
}

public ResolvedFace resolve_face (string face_name, Theme? theme = null) throws ThemeError {
	require_registered_face_name (face_name);

	if (theme == null)
		theme = current_theme;

	ResolvedFace resolved = new ResolvedFace (face_name);
	var seen = new HashTable<string, string> (str_hash, str_equal);
	resolve_face_into (theme, face_name, seen, resolved);
	return resolved;
}

static int lookup_terminal_color (string color_name) {
	switch (color_name) {
	case "default":
		return TERM_COLOR_DEFAULT;
	case "black":
		return 0;
	case "red":
		return 1;
	case "green":
		return 2;
	case "yellow":
		return 3;
	case "blue":
		return 4;
	case "magenta":
		return 5;
	case "cyan":
		return 6;
	case "white":
		return 7;
	case "brightblack":
	case "gray":
	case "grey":
		return 8;
	case "brightred":
		return 9;
	case "brightgreen":
		return 10;
	case "brightyellow":
		return 11;
	case "brightblue":
		return 12;
	case "brightmagenta":
		return 13;
	case "brightcyan":
		return 14;
	case "brightwhite":
		return 15;
	default:
		return TERM_COLOR_UNSPECIFIED;
	}
}

static int normalize_terminal_color (int color,
									 bool is_foreground,
									 TerminalCapabilities capabilities,
									 TerminalStyle style) {
	if (color == TERM_COLOR_DEFAULT)
		return capabilities.supports_default_colors ? color : TERM_COLOR_UNSPECIFIED;

	if (color < 0)
		return TERM_COLOR_UNSPECIFIED;

	if (!capabilities.has_colors || capabilities.color_count <= 0)
		return TERM_COLOR_UNSPECIFIED;

	if (capabilities.color_count >= 16)
		return color;

	if (capabilities.color_count >= 8) {
		if (color >= 8) {
			if (is_foreground)
				style.bold = true;
			return color - 8;
		}
		return color;
	}

	return TERM_COLOR_UNSPECIFIED;
}

static void apply_mono_fallback (string face_name,
								 TerminalCapabilities capabilities,
								 TerminalStyle style) {
	switch (face_name) {
	case FACE_REGION:
	case FACE_ISEARCH:
	case FACE_MATCH:
	case FACE_TRAILING_WHITESPACE:
	case FACE_MODE_LINE:
		if (capabilities.supports_reverse)
			style.reverse = true;
		else if (capabilities.supports_underline)
			style.underline = true;
		else
			style.bold = true;
		break;
	case FACE_LAZY_HIGHLIGHT:
	case FACE_MODE_LINE_INACTIVE:
	case FACE_MINIBUFFER_PROMPT:
	case FACE_LINE_NUMBER_CURRENT_LINE:
	case FACE_ERROR:
	case FACE_WARNING:
	case FACE_SUCCESS:
	case FACE_FONT_LOCK_COMMENT:
	case FACE_FONT_LOCK_KEYWORD:
	case FACE_FONT_LOCK_FUNCTION_NAME:
	case FACE_FONT_LOCK_TYPE:
	case FACE_FONT_LOCK_CONSTANT:
	case FACE_FONT_LOCK_BUILTIN:
	case FACE_FONT_LOCK_PREPROCESSOR:
	case FACE_FONT_LOCK_WARNING:
		if (capabilities.supports_underline)
			style.underline = true;
		else
			style.bold = true;
		break;
	case FACE_FONT_LOCK_STRING:
		if (capabilities.supports_underline)
			style.underline = true;
		break;
	default:
		break;
	}
}

static void populate_default_color_fallbacks (TerminalStyle style, Theme? theme) {
	bool prefer_light_defaults =
		theme != null && theme.variant != null && (string) theme.variant == "light";

	style.default_foreground_fallback =
		prefer_light_defaults ? TERM_COLOR_BLACK : TERM_COLOR_WHITE;
	style.default_background_fallback =
		prefer_light_defaults ? TERM_COLOR_WHITE : TERM_COLOR_BLACK;
}

public TerminalStyle resolve_terminal_style (string face_name,
											 Theme? theme = null,
											 TerminalCapabilities? capabilities = null) throws ThemeError {
	TerminalCapabilities resolved_capabilities =
		capabilities != null ? capabilities : term_get_capabilities ();
	Theme? resolved_theme = theme != null ? theme : current_theme;

	ResolvedFace resolved = resolve_face (face_name, resolved_theme);
	TerminalStyle style = new TerminalStyle (face_name);
	populate_default_color_fallbacks (style, resolved_theme);

	style.bold = resolved.bold;
	style.underline = resolved_capabilities.supports_underline && resolved.underline;
	style.reverse = resolved_capabilities.supports_reverse && resolved.reverse;

	if (!resolved_capabilities.has_colors) {
		apply_mono_fallback (face_name, resolved_capabilities, style);
		return style;
	}

	if (resolved.has_foreground && resolved.foreground != null) {
		int fg = normalize_terminal_color (
			lookup_terminal_color ((string) resolved.foreground),
			true,
			resolved_capabilities,
			style);
		if (fg != TERM_COLOR_UNSPECIFIED) {
			style.foreground = fg;
			style.has_foreground = true;
		}
	}

	if (resolved.has_background && resolved.background != null) {
		int bg = normalize_terminal_color (
			lookup_terminal_color ((string) resolved.background),
			false,
			resolved_capabilities,
			style);
		if (bg != TERM_COLOR_UNSPECIFIED) {
			style.background = bg;
			style.has_background = true;
		}
	}

	return style;
}

static FaceSpec make_face (string name,
						   string? foreground = null,
						   string? background = null,
						   string? inherit_name = null,
						   bool? bold = null,
						   bool? underline = null,
						   bool? reverse = null) {
	FaceSpec face = new FaceSpec (name);

	if (foreground != null) {
		face.foreground = foreground;
		face.has_foreground = true;
	}
	if (background != null) {
		face.background = background;
		face.has_background = true;
	}
	if (inherit_name != null)
		face.inherit_name = inherit_name;
	if (bold != null) {
		face.bold = (bool) bold;
		face.has_bold = true;
	}
	if (underline != null) {
		face.underline = (bool) underline;
		face.has_underline = true;
	}
	if (reverse != null) {
		face.reverse = (bool) reverse;
		face.has_reverse = true;
	}

	return face;
}

static void set_builtin_face (Theme theme, FaceSpec face) {
	try {
		theme.set_face (face);
	} catch (ThemeError e) {
		assert_not_reached ();
	}
}

static void define_builtin_faces () {
	string[] builtin_faces = {
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
	};

	foreach (unowned string face_name in builtin_faces)
		define_face_name (face_name);
}

static void define_default_dark_theme () {
	Theme theme = define_theme (THEME_DEFAULT_DARK, "dark");

	set_builtin_face (theme, make_face (FACE_DEFAULT, "default", "default"));
	set_builtin_face (theme, make_face (FACE_REGION, null, "blue"));
	set_builtin_face (theme, make_face (FACE_MODE_LINE, "black", "cyan", null, true));
	set_builtin_face (theme, make_face (FACE_MODE_LINE_INACTIVE, "white", "blue", FACE_MODE_LINE, false));
	set_builtin_face (theme, make_face (FACE_MINIBUFFER_PROMPT, "cyan", null, null, true));
	set_builtin_face (theme, make_face (FACE_VERTICAL_BORDER, "brightblack"));
	set_builtin_face (theme, make_face (FACE_LINE_NUMBER, "brightblack"));
	set_builtin_face (theme, make_face (FACE_LINE_NUMBER_CURRENT_LINE, "white", null, FACE_LINE_NUMBER, true));
	set_builtin_face (theme, make_face (FACE_TRAILING_WHITESPACE, null, "red"));
	set_builtin_face (theme, make_face (FACE_ISEARCH, "black", "yellow", null, true));
	set_builtin_face (theme, make_face (FACE_LAZY_HIGHLIGHT, null, "blue"));
	set_builtin_face (theme, make_face (FACE_MATCH, "black", "green"));
	set_builtin_face (theme, make_face (FACE_ERROR, "red", null, null, true));
	set_builtin_face (theme, make_face (FACE_WARNING, "yellow", null, null, true));
	set_builtin_face (theme, make_face (FACE_SUCCESS, "green", null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_COMMENT, "brightblack"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_STRING, "green"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_KEYWORD, "cyan", null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_FUNCTION_NAME, "blue"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_VARIABLE_NAME, "default"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_TYPE, "magenta"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_CONSTANT, "magenta"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_BUILTIN, "cyan"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_PREPROCESSOR, "yellow"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_WARNING, null, null, FACE_WARNING));
}

static void define_default_light_theme () {
	Theme theme = define_theme (THEME_DEFAULT_LIGHT, "light");

	set_builtin_face (theme, make_face (FACE_DEFAULT, "default", "default"));
	set_builtin_face (theme, make_face (FACE_REGION, "black", "cyan"));
	set_builtin_face (theme, make_face (FACE_MODE_LINE, "white", "blue", null, true));
	set_builtin_face (theme, make_face (FACE_MODE_LINE_INACTIVE, "black", "white", FACE_MODE_LINE, false));
	set_builtin_face (theme, make_face (FACE_MINIBUFFER_PROMPT, "blue", null, null, true));
	set_builtin_face (theme, make_face (FACE_VERTICAL_BORDER, "blue"));
	set_builtin_face (theme, make_face (FACE_LINE_NUMBER, "blue"));
	set_builtin_face (theme, make_face (FACE_LINE_NUMBER_CURRENT_LINE, "black", null, FACE_LINE_NUMBER, true));
	set_builtin_face (theme, make_face (FACE_TRAILING_WHITESPACE, null, "red"));
	set_builtin_face (theme, make_face (FACE_ISEARCH, "black", "yellow", null, true));
	set_builtin_face (theme, make_face (FACE_LAZY_HIGHLIGHT, null, "yellow"));
	set_builtin_face (theme, make_face (FACE_MATCH, "white", "green"));
	set_builtin_face (theme, make_face (FACE_ERROR, "red", null, null, true));
	set_builtin_face (theme, make_face (FACE_WARNING, "magenta", null, null, true));
	set_builtin_face (theme, make_face (FACE_SUCCESS, "green", null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_COMMENT, "magenta"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_STRING, "green"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_KEYWORD, "blue", null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_FUNCTION_NAME, "blue"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_VARIABLE_NAME, "default"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_TYPE, "magenta"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_CONSTANT, "magenta"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_BUILTIN, "blue"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_PREPROCESSOR, "red"));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_WARNING, null, null, FACE_WARNING));
}

static void define_terminal_default_theme () {
	Theme theme = define_theme (THEME_TERMINAL_DEFAULT, null);

	set_builtin_face (theme, make_face (FACE_DEFAULT, "default", "default"));
	set_builtin_face (theme, make_face (FACE_REGION, null, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_MODE_LINE, null, null, null, true, null, true));
	set_builtin_face (theme, make_face (FACE_MODE_LINE_INACTIVE, null, null, FACE_MODE_LINE, false, true, null));
	set_builtin_face (theme, make_face (FACE_MINIBUFFER_PROMPT, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_VERTICAL_BORDER));
	set_builtin_face (theme, make_face (FACE_LINE_NUMBER));
	set_builtin_face (theme, make_face (FACE_LINE_NUMBER_CURRENT_LINE, null, null, FACE_LINE_NUMBER, true));
	set_builtin_face (theme, make_face (FACE_TRAILING_WHITESPACE, null, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_ISEARCH, null, null, null, true, null, true));
	set_builtin_face (theme, make_face (FACE_LAZY_HIGHLIGHT, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_MATCH, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_ERROR, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_WARNING, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_SUCCESS, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_COMMENT));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_STRING, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_KEYWORD, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_FUNCTION_NAME, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_VARIABLE_NAME));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_TYPE, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_CONSTANT, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_BUILTIN, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_PREPROCESSOR, null, null, null, null, true));
	set_builtin_face (theme, make_face (FACE_FONT_LOCK_WARNING, null, null, FACE_WARNING));
}

public void theme_init () {
	face_name_table = new HashTable<string, string> (str_hash, str_equal);
	theme_table = new HashTable<string, Theme> (str_hash, str_equal);
	current_theme = null;
	define_builtin_faces ();
	define_default_dark_theme ();
	define_default_light_theme ();
	define_terminal_default_theme ();
	activate_theme (THEME_TERMINAL_DEFAULT);
}
