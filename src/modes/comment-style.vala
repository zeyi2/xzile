/* Editor-facing comment style definition */

public class CommentStyle : Object {
	public string? line_comment_prefix;
	public string? block_comment_open;
	public string? block_comment_close;
	public bool prefer_line_comments;

	public CommentStyle (string? line_comment_prefix = null,
						 string? block_comment_open = null,
						 string? block_comment_close = null,
						 bool prefer_line_comments = true) {
		this.line_comment_prefix = line_comment_prefix;
		this.block_comment_open = block_comment_open;
		this.block_comment_close = block_comment_close;
		this.prefer_line_comments = prefer_line_comments;
	}
}
