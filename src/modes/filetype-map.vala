/* Filetype-to-mode mapping */

public class FiletypeMap : Object {
	private static bool has_any_suffix (string basename, string[] suffixes) {
		foreach (string suffix in suffixes) {
			if (basename.has_suffix (suffix))
				return true;
		}

		return false;
	}

	private static string? resolve_mode_id_from_basename (string basename) {
		if (has_any_suffix (basename, { ".c", ".h" }))
			return "c";
		if (has_any_suffix (basename, {
			".C", ".cc", ".cp", ".cpp", ".cxx", ".c++",
			".hh", ".hpp", ".hxx", ".h++", ".ipp", ".tpp", ".ixx", ".cppm"
		}))
			return "cpp";
		if (has_any_suffix (basename, { ".py", ".pyi", ".pyw" }))
			return "python";

		return null;
	}

	public static string? resolve_mode_id (string? filename) {
		if (filename == null || filename.length == 0)
			return null;

		return resolve_mode_id_from_basename (Path.get_basename (filename));
	}

	public static SyntaxHighlighter? resolve_highlighter (string? filename) {
		string? mode_id = resolve_mode_id (filename);
		debug_log ("filetype", "resolve_highlighter filename=%s mode=%s",
			filename ?? "(null)", mode_id ?? "(null)");

		if (mode_id == "c")
			return new CMode ();
		if (mode_id == "cpp")
			return new CppMode ();
		if (mode_id == "python")
			return new PythonMode ();

		return null;
	}

	public static void apply_to_buffer (Buffer bp) {
		bp.highlighter = resolve_highlighter (bp.filename);
		debug_log ("filetype", "apply_to_buffer buffer=%s filename=%s highlighter=%s",
			bp.name, bp.filename ?? "(null)", bp.highlighter != null ? bp.highlighter.name : "(null)");
	}
}
