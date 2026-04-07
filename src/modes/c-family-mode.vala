/* Shared C-family mode implementation */

public class CFamilyMode : Object, SyntaxHighlighter, CommentableMode {
	private const int STATE_LINE_COMMENT = 1;
	private const int STATE_BLOCK_COMMENT = 2;
	private const int STATE_BLOCK_COMMENT_END = 20;
	private const int STATE_STRING = 3;
	private const int STATE_CHAR = 4;
	private const int STATE_PREPROCESSOR = 5;

	private string display_name;
	private string[] keywords;
	private CommentStyle family_comment_style;
	private SyntaxCommentDefinition comments;

	protected CFamilyMode (string display_name, string[] keywords) {
		this.display_name = display_name;
		this.keywords = keywords;
		family_comment_style = new CommentStyle ("//", "/*", "*/", true);
		comments = new SyntaxCommentDefinition (
			family_comment_style,
			STATE_LINE_COMMENT,
			STATE_BLOCK_COMMENT,
			STATE_BLOCK_COMMENT_END
		);
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

		state = comments.finish_line (state);

		if (state == STATE_PREPROCESSOR || state == STATE_STRING || state == STATE_CHAR)
			return 0;

		return state;
	}
}
