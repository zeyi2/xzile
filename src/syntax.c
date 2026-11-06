/* Syntax highlighting modes and filetype mapping

   Copyright (c) 2025-2026 Zeyi2 <zeyi2@nekoarch.cc>

   This file is part of XZile.

   XZile is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   XZile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.  */

/* A highlighter reports a face name per byte of a line, and returns the
   parser state for the start of the next line. */

#include "xzile.h"

#include <ctype.h>

/*--------------------------------------------------------------------------
 * Shared helpers.
 *--------------------------------------------------------------------------*/

/* Comment scanning shared by the modes. */
typedef struct {
	const CommentStyle *style;
	int line_comment_state;
	int block_comment_state;
	int block_comment_close_state;
	bool skip_next;		/* The second byte of an opening delimiter. */
} CommentScanner;

static bool
matches_delimiter (const char *delimiter, char c, char next_c)
{
	if (delimiter == NULL || delimiter[0] == '\0')
		return false;
	if (delimiter[1] == '\0')
		return c == delimiter[0];
	return c == delimiter[0] && next_c == delimiter[1];
}

static bool
comment_scan (CommentScanner *cs, int *state, char c, char next_c, const char **face_name)
{
	*face_name = NULL;
	if (*state == cs->line_comment_state) {
		*face_name = FACE_FONT_LOCK_COMMENT;
		return true;
	}
	if (*state == cs->block_comment_state) {
		*face_name = FACE_FONT_LOCK_COMMENT;
		if (cs->skip_next)
			cs->skip_next = false;
		else if (matches_delimiter (cs->style->block_comment_close, c, next_c))
			*state = cs->block_comment_close_state;
		return true;
	}
	if (*state == cs->block_comment_close_state) {
		*face_name = FACE_FONT_LOCK_COMMENT;
		*state = SYNTAX_STATE_NORMAL;
		return true;
	}
	if (*state != SYNTAX_STATE_NORMAL)
		return false;
	if (matches_delimiter (cs->style->line_comment_prefix, c, next_c)) {
		*state = cs->line_comment_state;
		*face_name = FACE_FONT_LOCK_COMMENT;
		return true;
	}
	if (matches_delimiter (cs->style->block_comment_open, c, next_c)) {
		*state = cs->block_comment_state;
		cs->skip_next = strlen (cs->style->block_comment_open) == 2;
		*face_name = FACE_FONT_LOCK_COMMENT;
		return true;
	}
	return false;
}

static int
comment_finish_line (const CommentScanner *cs, int state)
{
	if (state == cs->line_comment_state || state == cs->block_comment_close_state)
		return SYNTAX_STATE_NORMAL;
	return state;
}

static bool
is_ident_start (char c)
{
	return isalpha ((unsigned char) c) || c == '_';
}

static bool
is_ident_part (char c)
{
	return isalnum ((unsigned char) c) || c == '_';
}

static bool
is_keyword (const Mode *mode, const char *token, size_t len)
{
	for (size_t i = 0; i < mode->nkeywords; i++)
		if (strlen (mode->keywords[i]) == len && memcmp (mode->keywords[i], token, len) == 0)
			return true;
	return false;
}

/* If a keyword starts at byte `i' of the line at `o', return its length. */
static size_t
match_keyword (const Mode *mode, Buffer *bp, size_t o, size_t i, size_t line_len)
{
	if (i > 0 && is_ident_part (buffer_get_char (bp, o + i - 1)))
		return 0;
	if (!is_ident_start (buffer_get_char (bp, o + i)))
		return 0;

	char token[64];
	size_t len = 0;
	while (i + len < line_len) {
		char c = buffer_get_char (bp, o + i + len);
		if (!is_ident_part (c))
			break;
		if (len < sizeof token)
			token[len] = c;
		len++;
	}

	bool keyword = len <= sizeof token && is_keyword (mode, token, len);
	if (debug_enabled ("syntax"))
		debug_log ("syntax", "%s candidate token=%.*s len=%zu offset=%zu keyword=%s",
			   mode->name, (int) MIN (len, sizeof token), token, len, o + i, keyword ? "yes" : "no");
	return keyword ? len : 0;
}


/*--------------------------------------------------------------------------
 * C family modes.
 *--------------------------------------------------------------------------*/

enum {
	C_LINE_COMMENT = 1,
	C_BLOCK_COMMENT = 2,
	C_STRING = 3,
	C_CHAR = 4,
	C_PREPROCESSOR = 5,
	C_BLOCK_COMMENT_END = 20,
};

static const CommentStyle c_comment_style = { "//", "/*", "*/", true };

static int
c_family_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		    size_t len, int start_state, const char **face_names)
{
	CommentScanner comments = { mode->comment_style, C_LINE_COMMENT, C_BLOCK_COMMENT,
				    C_BLOCK_COMMENT_END, false };
	int state = start_state;
	size_t skip_count = 0;
	size_t kw_remain = 0;
	bool is_first_non_space = true;

	for (size_t i = 0; i < len; i++) {
		const char *face_name = NULL;
		char c = buffer_get_char (bp, start_o + i);
		char next_c = i + 1 < len ? buffer_get_char (bp, start_o + i + 1) : '\0';

		if (state == SYNTAX_STATE_NORMAL) {
			if (kw_remain > 0) {
				face_name = FACE_FONT_LOCK_KEYWORD;
				kw_remain--;
			} else if (comment_scan (&comments, &state, c, next_c, &face_name)) {
			} else if (c == '"') {
				state = C_STRING;
				face_name = FACE_FONT_LOCK_STRING;
			} else if (c == '\'') {
				state = C_CHAR;
				face_name = FACE_FONT_LOCK_STRING;
			} else if (c == '#' && is_first_non_space) {
				state = C_PREPROCESSOR;
				face_name = FACE_FONT_LOCK_PREPROCESSOR;
			} else if (is_ident_start (c)) {
				size_t kw_len = match_keyword (mode, bp, start_o, i, len);
				if (kw_len > 0) {
					kw_remain = kw_len - 1;
					face_name = FACE_FONT_LOCK_KEYWORD;
				}
			}
		} else if (comment_scan (&comments, &state, c, next_c, &face_name)) {
		} else if (state == C_STRING || state == C_CHAR) {
			face_name = FACE_FONT_LOCK_STRING;
			if (skip_count > 0)
				skip_count--;
			else if (c == '\\')
				skip_count = 1;
			else if (c == (state == C_STRING ? '"' : '\''))
				state = SYNTAX_STATE_NORMAL;
		} else if (state == C_PREPROCESSOR) {
			face_name = FACE_FONT_LOCK_PREPROCESSOR;
			if (c == '/' && (next_c == '/' || next_c == '*')) {
				/* A comment ends the highlighted directive. */
				state = SYNTAX_STATE_NORMAL;
				comment_scan (&comments, &state, c, next_c, &face_name);
			}
		}

		face_names[i] = face_name;
		if (!isspace ((unsigned char) c) && state != C_BLOCK_COMMENT && state != C_BLOCK_COMMENT_END)
			is_first_non_space = false;
	}

	if (len > 0 && debug_enabled ("syntax")) {
		size_t keyword_bytes = 0, preprocessor_bytes = 0;
		for (size_t i = 0; i < len; i++) {
			if (face_names[i] == NULL)
				continue;
			if (strcmp (face_names[i], FACE_FONT_LOCK_KEYWORD) == 0)
				keyword_bytes++;
			else if (strcmp (face_names[i], FACE_FONT_LOCK_PREPROCESSOR) == 0)
				preprocessor_bytes++;
		}
		debug_log ("syntax", "%s line=%zu state_in=%d state_mid=%d len=%zu keyword_bytes=%zu preprocessor_bytes=%zu",
			   mode->name, line_idx, start_state, state, len, keyword_bytes, preprocessor_bytes);
	}

	state = comment_finish_line (&comments, state);

	if (state == C_PREPROCESSOR || state == C_STRING || state == C_CHAR)
		return SYNTAX_STATE_NORMAL;

	return state;
}

static const char *const c_keywords[] = {
	"alignas", "alignof", "auto", "bool", "break", "case", "char",
	"const", "constexpr", "continue", "default", "do", "double", "else",
	"enum", "extern", "false", "float", "for", "goto", "if", "inline",
	"int", "long", "nullptr", "register", "restrict", "return", "short",
	"signed", "sizeof", "static", "static_assert", "struct", "switch",
	"thread_local", "true", "typedef", "typeof", "typeof_unqual", "union",
	"unsigned", "void", "volatile", "while", "_Alignas", "_Alignof",
	"_Atomic", "_Bool", "_Complex", "_Decimal32", "_Decimal64",
	"_Decimal128", "_Generic", "_Imaginary", "_Noreturn",
	"_Static_assert", "_Thread_local",
};

static const char *const cpp_keywords[] = {
	"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
	"bitor", "bool", "break", "case", "catch", "char", "char8_t",
	"char16_t", "char32_t", "class", "co_await", "co_return",
	"co_yield", "compl", "concept", "const", "consteval", "constexpr",
	"constinit", "const_cast", "continue", "decltype", "default",
	"delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
	"export", "extern", "false", "float", "for", "friend", "goto", "if",
	"inline", "int", "long", "mutable", "namespace", "new", "noexcept",
	"not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
	"protected", "public", "register", "reinterpret_cast", "requires",
	"return", "short", "signed", "sizeof", "static", "static_assert",
	"static_cast", "struct", "switch", "template", "this", "thread_local",
	"throw", "true", "try", "typedef", "typeid", "typename", "union",
	"unsigned", "using", "virtual", "void", "volatile", "wchar_t",
	"while", "xor", "xor_eq",
};

static const Mode c_mode = {
	"C", c_family_scan_line, &c_comment_style,
	c_keywords, sizeof c_keywords / sizeof c_keywords[0],
};

static const Mode cpp_mode = {
	"C++", c_family_scan_line, &c_comment_style,
	cpp_keywords, sizeof cpp_keywords / sizeof cpp_keywords[0],
};


/*--------------------------------------------------------------------------
 * Python mode.
 *--------------------------------------------------------------------------*/

enum {
	PY_LINE_COMMENT = 1,
	PY_SINGLE_STRING = 2,
	PY_DOUBLE_STRING = 3,
	PY_TRIPLE_SINGLE_STRING = 4,
	PY_TRIPLE_DOUBLE_STRING = 5,
	PY_TRIPLE_SINGLE_COMMENT = 6,
	PY_TRIPLE_DOUBLE_COMMENT = 7,
	PY_UNUSED_BLOCK = 90,
	PY_UNUSED_BLOCK_END = 91,
};

static const CommentStyle python_comment_style = { "#", NULL, NULL, true };

static const char *const python_keywords[] = {
	"False", "None", "True", "_", "and", "as", "assert", "async",
	"await", "break", "case", "class", "continue", "def", "del",
	"elif", "else", "except", "finally", "for", "from", "global", "if",
	"import", "in", "is", "lambda", "match", "nonlocal", "not", "or",
	"pass", "raise", "return", "try", "type", "while", "with", "yield",
};

static bool
is_only_leading_whitespace (Buffer *bp, size_t start_o, size_t i)
{
	for (size_t k = 0; k < i; k++)
		if (!isspace ((unsigned char) buffer_get_char (bp, start_o + k)))
			return false;
	return true;
}

static bool
is_string_prefix_char (char c)
{
	return c == 'r' || c == 'R' || c == 'u' || c == 'U' ||
		c == 'b' || c == 'B' || c == 'f' || c == 'F';
}

static const char *
python_face_for_state (int state)
{
	if (state == PY_LINE_COMMENT || state == PY_TRIPLE_SINGLE_COMMENT ||
	    state == PY_TRIPLE_DOUBLE_COMMENT)
		return FACE_FONT_LOCK_COMMENT;
	return FACE_FONT_LOCK_STRING;
}

static bool
match_string_start (Buffer *bp, size_t start_o, size_t i, size_t line_len,
		    int *string_state, size_t *string_chars_after_current, const char **face_name)
{
	*string_state = SYNTAX_STATE_NORMAL;
	*string_chars_after_current = 0;
	*face_name = FACE_FONT_LOCK_STRING;

	if (i > 0 && is_ident_part (buffer_get_char (bp, start_o + i - 1)))
		return false;

	size_t prefix_len = 0;
	size_t pos = i;
	while (pos < line_len && prefix_len < 2 && is_string_prefix_char (buffer_get_char (bp, start_o + pos))) {
		prefix_len++;
		pos++;
	}

	if (pos >= line_len)
		return false;

	char quote = buffer_get_char (bp, start_o + pos);
	char next_c = pos + 1 < line_len ? buffer_get_char (bp, start_o + pos + 1) : '\0';
	char next_next_c = pos + 2 < line_len ? buffer_get_char (bp, start_o + pos + 2) : '\0';
	bool comment_like = is_only_leading_whitespace (bp, start_o, i);

	if ((quote == '\'' || quote == '"') && next_c == quote && next_next_c == quote) {
		if (quote == '\'')
			*string_state = comment_like ? PY_TRIPLE_SINGLE_COMMENT : PY_TRIPLE_SINGLE_STRING;
		else
			*string_state = comment_like ? PY_TRIPLE_DOUBLE_COMMENT : PY_TRIPLE_DOUBLE_STRING;
		*string_chars_after_current = prefix_len + 2;
		*face_name = python_face_for_state (*string_state);
		return true;
	}

	if (quote == '\'' || quote == '"') {
		*string_state = quote == '\'' ? PY_SINGLE_STRING : PY_DOUBLE_STRING;
		*string_chars_after_current = prefix_len;
		return true;
	}

	return false;
}

static bool
match_triple_delimiter (Buffer *bp, size_t start_o, size_t i, size_t line_len, char quote)
{
	if (i + 2 >= line_len)
		return false;

	return buffer_get_char (bp, start_o + i) == quote &&
		buffer_get_char (bp, start_o + i + 1) == quote &&
		buffer_get_char (bp, start_o + i + 2) == quote;
}

static int
python_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		  size_t len, int start_state, const char **face_names)
{
	(void) line_idx;
	CommentScanner comments = { mode->comment_style, PY_LINE_COMMENT, PY_UNUSED_BLOCK,
				    PY_UNUSED_BLOCK_END, false };
	int state = start_state;
	size_t skip_count = 0;
	size_t kw_remain = 0;
	size_t string_chars_after_current = 0;
	const char *delayed_face_name = FACE_FONT_LOCK_STRING;

	for (size_t i = 0; i < len; i++) {
		const char *face_name = NULL;
		char c = buffer_get_char (bp, start_o + i);
		char next_c = i + 1 < len ? buffer_get_char (bp, start_o + i + 1) : '\0';

		if (string_chars_after_current > 0) {
			face_name = delayed_face_name;
			string_chars_after_current--;
		} else if (state == SYNTAX_STATE_NORMAL) {
			int string_state;
			size_t string_remain;
			const char *string_face_name;
			if (kw_remain > 0) {
				face_name = FACE_FONT_LOCK_KEYWORD;
				kw_remain--;
			} else if (comment_scan (&comments, &state, c, next_c, &face_name)) {
			} else if (match_string_start (bp, start_o, i, len, &string_state, &string_remain,
						       &string_face_name)) {
				state = string_state;
				string_chars_after_current = string_remain;
				delayed_face_name = string_face_name;
				face_name = string_face_name;
			} else if (is_ident_start (c)) {
				size_t kw_len = match_keyword (mode, bp, start_o, i, len);
				if (kw_len > 0) {
					kw_remain = kw_len - 1;
					face_name = FACE_FONT_LOCK_KEYWORD;
				}
			}
		} else if (comment_scan (&comments, &state, c, next_c, &face_name)) {
		} else if (state == PY_SINGLE_STRING || state == PY_DOUBLE_STRING) {
			face_name = FACE_FONT_LOCK_STRING;
			if (skip_count > 0)
				skip_count--;
			else if (c == '\\')
				skip_count = 1;
			else if (c == (state == PY_SINGLE_STRING ? '\'' : '"'))
				state = SYNTAX_STATE_NORMAL;
		} else {
			char quote = state == PY_TRIPLE_SINGLE_STRING || state == PY_TRIPLE_SINGLE_COMMENT ? '\'' : '"';
			face_name = python_face_for_state (state);
			if (skip_count > 0)
				skip_count--;
			else if (c == '\\')
				skip_count = 1;
			else if (match_triple_delimiter (bp, start_o, i, len, quote)) {
				delayed_face_name = face_name;
				state = SYNTAX_STATE_NORMAL;
				string_chars_after_current = 2;
			}
		}

		face_names[i] = face_name;
	}

	return comment_finish_line (&comments, state);
}

static const Mode python_mode = {
	"Python", python_scan_line, &python_comment_style,
	python_keywords, sizeof python_keywords / sizeof python_keywords[0],
};


/*--------------------------------------------------------------------------
 * Filetype-to-mode mapping.
 *--------------------------------------------------------------------------*/

static bool
has_any_suffix (const char *basename, const char *const *suffixes)
{
	for (; *suffixes != NULL; suffixes++)
		if (str_has_suffix (basename, *suffixes))
			return true;
	return false;
}

static const char *
resolve_mode_id (const char *filename)
{
	if (filename == NULL || filename[0] == '\0')
		return NULL;

	static const char *const c_suffixes[] = { ".c", ".h", NULL };
	static const char *const cpp_suffixes[] = {
		".C", ".cc", ".cp", ".cpp", ".cxx", ".c++",
		".hh", ".hpp", ".hxx", ".h++", ".ipp", ".tpp", ".ixx", ".cppm", NULL
	};
	static const char *const python_suffixes[] = { ".py", ".pyi", ".pyw", NULL };

	char *basename = path_basename (filename);
	const char *id = NULL;
	if (has_any_suffix (basename, c_suffixes))
		id = "c";
	else if (has_any_suffix (basename, cpp_suffixes))
		id = "cpp";
	else if (has_any_suffix (basename, python_suffixes))
		id = "python";
	free (basename);
	return id;
}

const Mode *
filetype_resolve_highlighter (const char *filename)
{
	const char *mode_id = resolve_mode_id (filename);
	debug_log ("filetype", "resolve_highlighter filename=%s mode=%s",
		   filename != NULL ? filename : "(null)", mode_id != NULL ? mode_id : "(null)");
	if (mode_id == NULL)
		return NULL;
	if (strcmp (mode_id, "c") == 0)
		return &c_mode;
	if (strcmp (mode_id, "cpp") == 0)
		return &cpp_mode;
	return &python_mode;
}

void
filetype_apply_to_buffer (Buffer *bp)
{
	/* A buffer put in markdown-mode keeps it when renamed. */
	const Mode *markdown = markdown_highlighter (bp);
	buffer_set_highlighter (bp, markdown != NULL ? markdown : filetype_resolve_highlighter (bp->filename));
	debug_log ("filetype", "apply_to_buffer buffer=%s filename=%s highlighter=%s",
		   bp->name != NULL ? bp->name : "(null)",
		   bp->filename != NULL ? bp->filename : "(null)",
		   bp->highlighter != NULL ? bp->highlighter->name : "(null)");
}
