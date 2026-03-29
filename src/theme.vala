/* Theme and face registry */

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
		faces.insert (face.name, face);
	}

	public FaceSpec? lookup_face (string face_name) {
		return faces.lookup (face_name);
	}
}

HashTable<string, Theme> theme_table;
Theme? current_theme = null;

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

public void theme_init () {
	theme_table = new HashTable<string, Theme> (str_hash, str_equal);
	current_theme = null;
}
