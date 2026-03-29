/* C syntax highlighter */

public class CHighlighter : Object, SyntaxHighlighter {
	private const string[] keywords = {
		"auto", "break", "case", "char", "const", "continue", "default", "do",
		"double", "else", "enum", "extern", "float", "for", "goto", "if",
		"inline", "int", "long", "register", "restrict", "return", "short",
		"signed", "sizeof", "static", "struct", "switch", "typedef", "union",
		"unsigned", "void", "volatile", "while"
	};

	public string name {
		get { return "C"; }
	}

	private bool is_ident_start (char c) {
		return c.isalpha () || c == '_';
	}

	private bool is_ident_part (char c) {
		return c.isalnum () || c == '_';
	}

	private bool match_keyword (Buffer bp, size_t o, size_t i, size_t line_len, out size_t kw_len) {
		kw_len = 0;
		if (i > 0 && is_ident_part (bp.get_char (o + i - 1)))
			return false;

		foreach (string kw in keywords) {
			size_t len = kw.length;
			if (i + len > line_len)
				continue;

			bool match = true;
			for (size_t k = 0; k < len; k++) {
				if (bp.get_char (o + i + k) != kw[(long) k]) {
					match = false;
					break;
				}
			}

			if (!match)
				continue;
			if (i + len < line_len && is_ident_part (bp.get_char (o + i + len)))
				continue;

			kw_len = len;
			return true;
		}

		return false;
	}

	public int scan_line (Buffer bp,
						  size_t line_idx,
						  size_t start_o,
						  size_t len,
						  int start_state,
						  string?[] face_names) {
		int state = start_state;
		size_t skip_count = 0;
		size_t kw_remain = 0;
		bool is_first_non_space = true;

		for (size_t i = 0; i < len; i++) {
			string? face_name = null;
			char c = bp.get_char (start_o + i);
			char next_c = (i + 1 < len) ? bp.get_char (start_o + i + 1) : '\0';

			if (state == 0) {
				if (skip_count > 0) {
					face_name = FACE_FONT_LOCK_STRING;
					skip_count--;
				} else if (kw_remain > 0) {
					face_name = FACE_FONT_LOCK_KEYWORD;
					kw_remain--;
				} else if (c == '/' && next_c == '/') {
					state = 1;
					face_name = FACE_FONT_LOCK_COMMENT;
				} else if (c == '/' && next_c == '*') {
					state = 2;
					face_name = FACE_FONT_LOCK_COMMENT;
				} else if (c == '"') {
					state = 3;
					face_name = FACE_FONT_LOCK_STRING;
				} else if (c == '\'') {
					state = 4;
					face_name = FACE_FONT_LOCK_STRING;
				} else if (c == '#' && is_first_non_space) {
					state = 5;
					face_name = FACE_FONT_LOCK_PREPROCESSOR;
				} else if (is_ident_start (c)) {
					size_t kw_len = 0;
					if (match_keyword (bp, start_o, i, len, out kw_len)) {
						kw_remain = kw_len > 0 ? kw_len - 1 : 0;
						face_name = FACE_FONT_LOCK_KEYWORD;
					}
				}
			} else if (state == 1) {
				face_name = FACE_FONT_LOCK_COMMENT;
			} else if (state == 2) {
				face_name = FACE_FONT_LOCK_COMMENT;
				if (c == '*' && next_c == '/')
					state = 20;
			} else if (state == 20) {
				face_name = FACE_FONT_LOCK_COMMENT;
				state = 0;
			} else if (state == 3) {
				face_name = FACE_FONT_LOCK_STRING;
				if (c == '\\')
					skip_count = 1;
				else if (c == '"' && skip_count == 0)
					state = 0;
				else if (skip_count > 0)
					skip_count--;
			} else if (state == 4) {
				face_name = FACE_FONT_LOCK_STRING;
				if (c == '\\')
					skip_count = 1;
				else if (c == '\'' && skip_count == 0)
					state = 0;
				else if (skip_count > 0)
					skip_count--;
			} else if (state == 5) {
				face_name = FACE_FONT_LOCK_PREPROCESSOR;
				if (c == '/' && next_c == '/') {
					state = 1;
					face_name = FACE_FONT_LOCK_COMMENT;
				} else if (c == '/' && next_c == '*') {
					state = 2;
					face_name = FACE_FONT_LOCK_COMMENT;
				}
			}

			if (i < face_names.length)
				face_names[(int) i] = face_name;
			if (!c.isspace () && state != 2 && state != 20)
				is_first_non_space = false;
		}

		if (state == 1 || state == 5 || state == 20 || state == 3 || state == 4)
			return 0;

		return state;
	}
}
