/* Shared comment definition helpers for syntax modes */

public class SyntaxCommentDefinition : Object {
	public CommentStyle style;
	public int line_comment_state;
	public int block_comment_state;
	public int block_comment_close_state;
	public string comment_face_name;

	public SyntaxCommentDefinition (CommentStyle style,
									int line_comment_state,
									int block_comment_state,
									int block_comment_close_state,
									string comment_face_name = FACE_FONT_LOCK_COMMENT) {
		this.style = style;
		this.line_comment_state = line_comment_state;
		this.block_comment_state = block_comment_state;
		this.block_comment_close_state = block_comment_close_state;
		this.comment_face_name = comment_face_name;
	}

	private bool matches_delimiter (string? delimiter, char c, char next_c) {
		if (delimiter == null || delimiter.length == 0)
			return false;

		if (delimiter.length == 1)
			return c == delimiter[0];

		if (delimiter.length == 2)
			return c == delimiter[0] && next_c == delimiter[1];

		return false;
	}

	public bool scan (ref int state, char c, char next_c, out string? face_name) {
		face_name = null;

		if (state == line_comment_state) {
			face_name = comment_face_name;
			return true;
		}

		if (state == block_comment_state) {
			face_name = comment_face_name;
			if (matches_delimiter (style.block_comment_close, c, next_c))
				state = block_comment_close_state;
			return true;
		}

		if (state == block_comment_close_state) {
			face_name = comment_face_name;
			state = SYNTAX_STATE_NORMAL;
			return true;
		}

		if (state != SYNTAX_STATE_NORMAL)
			return false;

		if (matches_delimiter (style.line_comment_prefix, c, next_c)) {
			state = line_comment_state;
			face_name = comment_face_name;
			return true;
		}

		if (matches_delimiter (style.block_comment_open, c, next_c)) {
			state = block_comment_state;
			face_name = comment_face_name;
			return true;
		}

		return false;
	}

	public int finish_line (int state) {
		if (state == line_comment_state || state == block_comment_close_state)
			return SYNTAX_STATE_NORMAL;

		return state;
	}
}
