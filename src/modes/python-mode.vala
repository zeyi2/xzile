/* Python mode */

public class PythonMode : Object, SyntaxHighlighter, CommentableMode {
	private const int STATE_LINE_COMMENT = 1;
	private const int STATE_SINGLE_STRING = 2;
	private const int STATE_DOUBLE_STRING = 3;
	private const int STATE_TRIPLE_SINGLE_STRING = 4;
	private const int STATE_TRIPLE_DOUBLE_STRING = 5;
	private const int STATE_TRIPLE_SINGLE_COMMENT = 6;
	private const int STATE_TRIPLE_DOUBLE_COMMENT = 7;
	private static string[] keywords = {
		"False", "None", "True", "_", "and", "as", "assert", "async",
		"await", "break", "case", "class", "continue", "def", "del",
		"elif", "else", "except", "finally", "for", "from", "global", "if",
		"import", "in", "is", "lambda", "match", "nonlocal", "not", "or",
		"pass", "raise", "return", "try", "type", "while", "with", "yield"
	};

	private CommentStyle python_comment_style;
	private SyntaxCommentDefinition comments;

	public PythonMode () {
		python_comment_style = new CommentStyle ("#", null, null, true);
		comments = new SyntaxCommentDefinition (
			python_comment_style,
			STATE_LINE_COMMENT,
			90,
			91
		);
	}

	public string name {
		get { return "Python"; }
	}

	public CommentStyle? comment_style {
		get { return python_comment_style; }
	}

	private bool is_ident_start (char c) {
		return c.isalpha () || c == '_';
	}

	private bool is_ident_part (char c) {
		return c.isalnum () || c == '_';
	}

	private bool is_only_leading_whitespace (Buffer bp, size_t start_o, size_t i) {
		for (size_t k = 0; k < i; k++) {
			if (!bp.get_char (start_o + k).isspace ())
				return false;
		}

		return true;
	}

	private bool is_string_prefix_char (char c) {
		return c == 'r' || c == 'R' || c == 'u' || c == 'U' ||
			   c == 'b' || c == 'B' || c == 'f' || c == 'F';
	}

	private string face_for_state (int state) {
		if (state == STATE_LINE_COMMENT ||
			state == STATE_TRIPLE_SINGLE_COMMENT ||
			state == STATE_TRIPLE_DOUBLE_COMMENT)
			return FACE_FONT_LOCK_COMMENT;

		return FACE_FONT_LOCK_STRING;
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

	private bool match_string_start (Buffer bp,
									 size_t start_o,
									 size_t i,
									 size_t line_len,
									 out int string_state,
									 out size_t string_chars_after_current,
									 out string face_name) {
		string_state = SYNTAX_STATE_NORMAL;
		string_chars_after_current = 0;
		face_name = FACE_FONT_LOCK_STRING;

		if (i > 0 && is_ident_part (bp.get_char (start_o + i - 1)))
			return false;

		size_t prefix_len = 0;
		size_t pos = i;
		while (pos < line_len && prefix_len < 2 && is_string_prefix_char (bp.get_char (start_o + pos))) {
			prefix_len++;
			pos++;
		}

		if (pos >= line_len)
			return false;

		char quote = bp.get_char (start_o + pos);
		char next_c = (pos + 1 < line_len) ? bp.get_char (start_o + pos + 1) : '\0';
		char next_next_c = (pos + 2 < line_len) ? bp.get_char (start_o + pos + 2) : '\0';
		bool comment_like = is_only_leading_whitespace (bp, start_o, i);

		if (quote == '\'' && next_c == '\'' && next_next_c == '\'') {
			string_state = comment_like ? STATE_TRIPLE_SINGLE_COMMENT : STATE_TRIPLE_SINGLE_STRING;
			string_chars_after_current = prefix_len + 2;
			face_name = face_for_state (string_state);
			return true;
		}

		if (quote == '"' && next_c == '"' && next_next_c == '"') {
			string_state = comment_like ? STATE_TRIPLE_DOUBLE_COMMENT : STATE_TRIPLE_DOUBLE_STRING;
			string_chars_after_current = prefix_len + 2;
			face_name = face_for_state (string_state);
			return true;
		}

		if (quote == '\'') {
			string_state = STATE_SINGLE_STRING;
			string_chars_after_current = prefix_len;
			face_name = FACE_FONT_LOCK_STRING;
			return true;
		}

		if (quote == '"') {
			string_state = STATE_DOUBLE_STRING;
			string_chars_after_current = prefix_len;
			face_name = FACE_FONT_LOCK_STRING;
			return true;
		}

		return false;
	}

	private bool match_triple_delimiter (Buffer bp, size_t start_o, size_t i, size_t line_len, char quote) {
		if (i + 2 >= line_len)
			return false;

		return bp.get_char (start_o + i) == quote &&
			   bp.get_char (start_o + i + 1) == quote &&
			   bp.get_char (start_o + i + 2) == quote;
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
		size_t string_chars_after_current = 0;
		string delayed_face_name = FACE_FONT_LOCK_STRING;

		for (size_t i = 0; i < len; i++) {
			string? face_name = null;
			char c = bp.get_char (start_o + i);
			char next_c = (i + 1 < len) ? bp.get_char (start_o + i + 1) : '\0';

			if (string_chars_after_current > 0) {
				face_name = delayed_face_name;
				string_chars_after_current--;
			} else if (state == SYNTAX_STATE_NORMAL) {
				if (kw_remain > 0) {
					face_name = FACE_FONT_LOCK_KEYWORD;
					kw_remain--;
				} else if (comments.scan (ref state, c, next_c, out face_name)) {
				} else {
					int string_state = SYNTAX_STATE_NORMAL;
					size_t string_remain = 0;
					string string_face_name;
					if (match_string_start (bp, start_o, i, len, out string_state, out string_remain, out string_face_name)) {
						state = string_state;
						string_chars_after_current = string_remain;
						delayed_face_name = string_face_name;
						face_name = string_face_name;
					} else if (is_ident_start (c)) {
						size_t kw_len = 0;
						if (match_keyword (bp, start_o, i, len, out kw_len)) {
							kw_remain = kw_len > 0 ? kw_len - 1 : 0;
							face_name = FACE_FONT_LOCK_KEYWORD;
						}
					}
				}
			} else if (comments.scan (ref state, c, next_c, out face_name)) {
			} else if (state == STATE_SINGLE_STRING) {
				face_name = FACE_FONT_LOCK_STRING;
				if (skip_count > 0)
					skip_count--;
				else if (c == '\\')
					skip_count = 1;
				else if (c == '\'')
					state = SYNTAX_STATE_NORMAL;
			} else if (state == STATE_DOUBLE_STRING) {
				face_name = FACE_FONT_LOCK_STRING;
				if (skip_count > 0)
					skip_count--;
				else if (c == '\\')
					skip_count = 1;
				else if (c == '"')
					state = SYNTAX_STATE_NORMAL;
			} else if (state == STATE_TRIPLE_SINGLE_STRING) {
				face_name = FACE_FONT_LOCK_STRING;
				if (skip_count > 0)
					skip_count--;
				else if (c == '\\')
					skip_count = 1;
				else if (match_triple_delimiter (bp, start_o, i, len, '\'')) {
					state = SYNTAX_STATE_NORMAL;
					string_chars_after_current = 2;
					delayed_face_name = FACE_FONT_LOCK_STRING;
				}
			} else if (state == STATE_TRIPLE_DOUBLE_STRING) {
				face_name = FACE_FONT_LOCK_STRING;
				if (skip_count > 0)
					skip_count--;
				else if (c == '\\')
					skip_count = 1;
				else if (match_triple_delimiter (bp, start_o, i, len, '"')) {
					state = SYNTAX_STATE_NORMAL;
					string_chars_after_current = 2;
					delayed_face_name = FACE_FONT_LOCK_STRING;
				}
			} else if (state == STATE_TRIPLE_SINGLE_COMMENT) {
				face_name = FACE_FONT_LOCK_COMMENT;
				if (skip_count > 0)
					skip_count--;
				else if (c == '\\')
					skip_count = 1;
				else if (match_triple_delimiter (bp, start_o, i, len, '\'')) {
					state = SYNTAX_STATE_NORMAL;
					string_chars_after_current = 2;
					delayed_face_name = FACE_FONT_LOCK_COMMENT;
				}
			} else if (state == STATE_TRIPLE_DOUBLE_COMMENT) {
				face_name = FACE_FONT_LOCK_COMMENT;
				if (skip_count > 0)
					skip_count--;
				else if (c == '\\')
					skip_count = 1;
				else if (match_triple_delimiter (bp, start_o, i, len, '"')) {
					state = SYNTAX_STATE_NORMAL;
					string_chars_after_current = 2;
					delayed_face_name = FACE_FONT_LOCK_COMMENT;
				}
			}

			if (i < face_names.length)
				face_names[(int) i] = face_name;
		}

		return comments.finish_line (state);
	}
}
