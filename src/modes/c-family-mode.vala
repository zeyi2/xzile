/* Shared C-family mode implementation */

public class CFamilyMode : Object, SyntaxHighlighter, CommentableMode {
	private const int STATE_LINE_COMMENT = 1;
	private const int STATE_BLOCK_COMMENT = 2;
	private const int STATE_BLOCK_COMMENT_END = 20;
	private const int STATE_STRING = 3;
	private const int STATE_CHAR = 4;
	private const int STATE_PREPROCESSOR = 5;

	private string display_name;
	private HashTable<string, string> keyword_table;
	private CommentStyle family_comment_style;
	private SyntaxCommentDefinition comments;

	protected CFamilyMode (string display_name, string[] keywords) {
		this.display_name = display_name;
		keyword_table = new HashTable<string, string> (str_hash, str_equal);
		size_t keyword_count = 0;
		foreach (string kw in keywords) {
			keyword_table.insert (kw, kw);
			keyword_count++;
		}
		family_comment_style = new CommentStyle ("//", "/*", "*/", true);
		comments = new SyntaxCommentDefinition (
			family_comment_style,
			STATE_LINE_COMMENT,
			STATE_BLOCK_COMMENT,
			STATE_BLOCK_COMMENT_END
		);
		debug_log ("syntax", "%s init keyword_count=%zu has_int=%s has_if=%s has_return=%s",
			display_name,
			keyword_count,
			keyword_table.lookup ("int") != null ? "yes" : "no",
			keyword_table.lookup ("if") != null ? "yes" : "no",
			keyword_table.lookup ("return") != null ? "yes" : "no");
	}

	public string name {
		get { return display_name; }
	}

	public CommentStyle? comment_style {
		get { return family_comment_style; }
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
		if (!is_ident_start (bp.get_char (o + i)))
			return false;

		string token = "";
		size_t len = 0;
		while (i + len < line_len) {
			char c = bp.get_char (o + i + len);
			if (!is_ident_part (c))
				break;
			token += c.to_string ();
			len++;
		}

		if (len == 0)
			return false;
		string? keyword = keyword_table.lookup (token);
		if (debug_enabled ("syntax"))
			debug_log ("syntax", "%s candidate token=%s len=%zu offset=%zu keyword=%s",
				display_name, token, len, o + i, keyword != null ? "yes" : "no");
		if (keyword == null)
			return false;

		kw_len = len;
		debug_log ("syntax", "%s keyword token=%s offset=%zu", display_name, token, o + i);
		return true;
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
				} else if (comments.scan (ref state, c, next_c, out face_name)) {
				} else if (c == '"') {
					state = STATE_STRING;
					face_name = FACE_FONT_LOCK_STRING;
				} else if (c == '\'') {
					state = STATE_CHAR;
					face_name = FACE_FONT_LOCK_STRING;
				} else if (c == '#' && is_first_non_space) {
					state = STATE_PREPROCESSOR;
					face_name = FACE_FONT_LOCK_PREPROCESSOR;
				} else if (is_ident_start (c)) {
					size_t kw_len = 0;
					if (match_keyword (bp, start_o, i, len, out kw_len)) {
						kw_remain = kw_len > 0 ? kw_len - 1 : 0;
						face_name = FACE_FONT_LOCK_KEYWORD;
					}
				}
			} else if (comments.scan (ref state, c, next_c, out face_name)) {
			} else if (state == STATE_STRING) {
				face_name = FACE_FONT_LOCK_STRING;
				if (c == '\\')
					skip_count = 1;
				else if (c == '"' && skip_count == 0)
					state = 0;
				else if (skip_count > 0)
					skip_count--;
			} else if (state == STATE_CHAR) {
				face_name = FACE_FONT_LOCK_STRING;
				if (c == '\\')
					skip_count = 1;
				else if (c == '\'' && skip_count == 0)
					state = 0;
				else if (skip_count > 0)
					skip_count--;
			} else if (state == STATE_PREPROCESSOR) {
				face_name = FACE_FONT_LOCK_PREPROCESSOR;
				string? comment_face_name = null;
				if (comments.scan (ref state, c, next_c, out comment_face_name))
					face_name = comment_face_name;
			}

			if (i < face_names.length)
				face_names[(int) i] = face_name;
			if (!c.isspace () && state != STATE_BLOCK_COMMENT && state != STATE_BLOCK_COMMENT_END)
				is_first_non_space = false;
		}

		if (len > 0 && debug_enabled ("syntax")) {
			size_t keyword_bytes = 0;
			size_t preprocessor_bytes = 0;
			for (size_t i = 0; i < len && i < face_names.length; i++) {
				if (face_names[(int) i] == FACE_FONT_LOCK_KEYWORD)
					keyword_bytes++;
				else if (face_names[(int) i] == FACE_FONT_LOCK_PREPROCESSOR)
					preprocessor_bytes++;
			}
			debug_log ("syntax", "%s line=%zu state_in=%d state_mid=%d len=%zu keyword_bytes=%zu preprocessor_bytes=%zu",
				display_name, line_idx, start_state, state, len, keyword_bytes, preprocessor_bytes);
		}

		state = comments.finish_line (state);

		if (state == STATE_PREPROCESSOR || state == STATE_STRING || state == STATE_CHAR)
			return 0;

		return state;
	}
}
