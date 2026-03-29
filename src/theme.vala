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

	public void set_face (FaceSpec face) {
		assert_registered_face_name (face.name);
		if (face.inherit_name != null)
			assert_registered_face_name ((string) face.inherit_name);
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

static void assert_registered_face_name (string name) {
	assert (face_name_exists (name));
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

	theme.set_face (make_face (FACE_DEFAULT, "default", "default"));
	theme.set_face (make_face (FACE_REGION, null, "blue"));
	theme.set_face (make_face (FACE_MODE_LINE, "black", "cyan", null, true));
	theme.set_face (make_face (FACE_MODE_LINE_INACTIVE, "white", "blue", FACE_MODE_LINE, false));
	theme.set_face (make_face (FACE_MINIBUFFER_PROMPT, "cyan", null, null, true));
	theme.set_face (make_face (FACE_VERTICAL_BORDER, "brightblack"));
	theme.set_face (make_face (FACE_LINE_NUMBER, "brightblack"));
	theme.set_face (make_face (FACE_LINE_NUMBER_CURRENT_LINE, "white", null, FACE_LINE_NUMBER, true));
	theme.set_face (make_face (FACE_TRAILING_WHITESPACE, null, "red"));
	theme.set_face (make_face (FACE_ISEARCH, "black", "yellow", null, true));
	theme.set_face (make_face (FACE_LAZY_HIGHLIGHT, null, "blue"));
	theme.set_face (make_face (FACE_MATCH, "black", "green"));
	theme.set_face (make_face (FACE_ERROR, "red", null, null, true));
	theme.set_face (make_face (FACE_WARNING, "yellow", null, null, true));
	theme.set_face (make_face (FACE_SUCCESS, "green", null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_COMMENT, "brightblack"));
	theme.set_face (make_face (FACE_FONT_LOCK_STRING, "green"));
	theme.set_face (make_face (FACE_FONT_LOCK_KEYWORD, "cyan", null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_FUNCTION_NAME, "blue"));
	theme.set_face (make_face (FACE_FONT_LOCK_VARIABLE_NAME, "default"));
	theme.set_face (make_face (FACE_FONT_LOCK_TYPE, "magenta"));
	theme.set_face (make_face (FACE_FONT_LOCK_CONSTANT, "magenta"));
	theme.set_face (make_face (FACE_FONT_LOCK_BUILTIN, "cyan"));
	theme.set_face (make_face (FACE_FONT_LOCK_PREPROCESSOR, "yellow"));
	theme.set_face (make_face (FACE_FONT_LOCK_WARNING, null, null, FACE_WARNING));
}

static void define_default_light_theme () {
	Theme theme = define_theme (THEME_DEFAULT_LIGHT, "light");

	theme.set_face (make_face (FACE_DEFAULT, "default", "default"));
	theme.set_face (make_face (FACE_REGION, "black", "cyan"));
	theme.set_face (make_face (FACE_MODE_LINE, "white", "blue", null, true));
	theme.set_face (make_face (FACE_MODE_LINE_INACTIVE, "black", "white", FACE_MODE_LINE, false));
	theme.set_face (make_face (FACE_MINIBUFFER_PROMPT, "blue", null, null, true));
	theme.set_face (make_face (FACE_VERTICAL_BORDER, "blue"));
	theme.set_face (make_face (FACE_LINE_NUMBER, "blue"));
	theme.set_face (make_face (FACE_LINE_NUMBER_CURRENT_LINE, "black", null, FACE_LINE_NUMBER, true));
	theme.set_face (make_face (FACE_TRAILING_WHITESPACE, null, "red"));
	theme.set_face (make_face (FACE_ISEARCH, "black", "yellow", null, true));
	theme.set_face (make_face (FACE_LAZY_HIGHLIGHT, null, "yellow"));
	theme.set_face (make_face (FACE_MATCH, "white", "green"));
	theme.set_face (make_face (FACE_ERROR, "red", null, null, true));
	theme.set_face (make_face (FACE_WARNING, "magenta", null, null, true));
	theme.set_face (make_face (FACE_SUCCESS, "green", null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_COMMENT, "magenta"));
	theme.set_face (make_face (FACE_FONT_LOCK_STRING, "green"));
	theme.set_face (make_face (FACE_FONT_LOCK_KEYWORD, "blue", null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_FUNCTION_NAME, "blue"));
	theme.set_face (make_face (FACE_FONT_LOCK_VARIABLE_NAME, "default"));
	theme.set_face (make_face (FACE_FONT_LOCK_TYPE, "magenta"));
	theme.set_face (make_face (FACE_FONT_LOCK_CONSTANT, "magenta"));
	theme.set_face (make_face (FACE_FONT_LOCK_BUILTIN, "blue"));
	theme.set_face (make_face (FACE_FONT_LOCK_PREPROCESSOR, "red"));
	theme.set_face (make_face (FACE_FONT_LOCK_WARNING, null, null, FACE_WARNING));
}

static void define_terminal_default_theme () {
	Theme theme = define_theme (THEME_TERMINAL_DEFAULT, null);

	theme.set_face (make_face (FACE_DEFAULT, "default", "default"));
	theme.set_face (make_face (FACE_REGION, null, null, null, null, null, true));
	theme.set_face (make_face (FACE_MODE_LINE, null, null, null, true, null, true));
	theme.set_face (make_face (FACE_MODE_LINE_INACTIVE, null, null, FACE_MODE_LINE, false, true, null));
	theme.set_face (make_face (FACE_MINIBUFFER_PROMPT, null, null, null, true));
	theme.set_face (make_face (FACE_VERTICAL_BORDER));
	theme.set_face (make_face (FACE_LINE_NUMBER));
	theme.set_face (make_face (FACE_LINE_NUMBER_CURRENT_LINE, null, null, FACE_LINE_NUMBER, true));
	theme.set_face (make_face (FACE_TRAILING_WHITESPACE, null, null, null, null, null, true));
	theme.set_face (make_face (FACE_ISEARCH, null, null, null, true, null, true));
	theme.set_face (make_face (FACE_LAZY_HIGHLIGHT, null, null, null, null, true));
	theme.set_face (make_face (FACE_MATCH, null, null, null, null, true));
	theme.set_face (make_face (FACE_ERROR, null, null, null, true));
	theme.set_face (make_face (FACE_WARNING, null, null, null, null, true));
	theme.set_face (make_face (FACE_SUCCESS, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_COMMENT));
	theme.set_face (make_face (FACE_FONT_LOCK_STRING, null, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_KEYWORD, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_FUNCTION_NAME, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_VARIABLE_NAME));
	theme.set_face (make_face (FACE_FONT_LOCK_TYPE, null, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_CONSTANT, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_BUILTIN, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_PREPROCESSOR, null, null, null, null, true));
	theme.set_face (make_face (FACE_FONT_LOCK_WARNING, null, null, FACE_WARNING));
}

public void theme_init () {
	face_name_table = new HashTable<string, string> (str_hash, str_equal);
	theme_table = new HashTable<string, Theme> (str_hash, str_equal);
	current_theme = null;
	define_builtin_faces ();
	define_default_dark_theme ();
	define_default_light_theme ();
	define_terminal_default_theme ();
}
