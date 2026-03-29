/* Syntax highlighting abstraction
 *
 * The highlighter reports face names per byte of the input line and
 * returns the parser state for the start of the next line.
 */

public const int SYNTAX_STATE_NORMAL = 0;

public interface SyntaxHighlighter : Object {
	public abstract string name { get; }
	public abstract int scan_line (Buffer bp,
								   size_t line_idx,
								   size_t start_o,
								   size_t len,
								   int start_state,
								   string?[] face_names);
}
