/* Markdown mode, with a live preview in the text

   Copyright (c) 2026 Zeyi2 <zeyi2@nekoarch.cc>

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

/* Markdown is edited as source: the buffer holds the text that is
   saved, and every command edits it as usual. What changes is how the
   lines are drawn. The line point is on, and every line of an active
   region, are drawn as source; the others are drawn rendered, with the
   markup of headings, emphasis, code spans, links, quotes, lists, task
   boxes, rules and tables hidden or replaced by symbols (the "live
   preview"). C-c C-v shows the whole buffer as highlighted source.

   Lines are parsed one at a time, as redisplay draws them. What a line
   needs to know of those before it (whether it is in a fenced code
   block, a list, a table, or follows a paragraph) is kept in the
   highlighter's line-start state, which the buffer caches; a line also
   looks at the line after it, for setext headings and table headers.
   This is a subset of CommonMark and GitHub tables, not the whole of
   either: what is not recognized is shown as it is.

   A click on a rendered line is mapped to the text by the layout it was
   drawn with, so that point lands on the character clicked even though
   the line is then drawn as source. */

#include "../src/xzile.h"

#include <ctype.h>
#include <wchar.h>

/* The styles of the bytes of a line, combined. */
enum {
	MD_BOLD = 1u << 0,
	MD_ITALIC = 1u << 1,
	MD_STRIKE = 1u << 2,
	MD_ICODE = 1u << 3,	/* A code span. */
	MD_LINK = 1u << 4,
	MD_MARKUP = 1u << 5,	/* Markup shown, such as fences. */
	MD_QUOTE = 1u << 6,
	MD_LIST = 1u << 7,	/* List markers and task boxes. */
	MD_TABLE = 1u << 8,	/* Table pipes and rules. */
	MD_HR = 1u << 9,
	MD_DONE = 1u << 10,	/* A task that is done. */
	MD_CODE = 1u << 11,	/* A code block. */
	MD_THEAD = 1u << 12,	/* A table's header cells. */
	MD_HEADING_SHIFT = 13,	/* Then the heading level, 1-6, in 3 bits. */
};
#define MD_HEADING(level) ((unsigned) (level) << MD_HEADING_SHIFT)
#define MD_LEVEL(style) (((style) >> MD_HEADING_SHIFT) & 7)

/* The line-start state, as the highlighter keeps it. */
enum {
	S_FENCE = 1 << 0,	/* In a fenced code block, */
	S_TILDE = 1 << 1,	/* of tildes rather than backticks, */
	/* bits 2-7: as many of them as opened it (at most 63); bits
	   8-13: the indentation of the opening fence. */
	S_NONBLANK = 1 << 14,	/* The line before is not blank. */
	S_PARA = 1 << 15,	/* It is paragraph text. */
	S_LIST = 1 << 16,	/* In a list. */
	S_TABLE = 1 << 17,	/* In the rows of a table. */
	S_CELLS_SHIFT = 19,	/* The cells of a paragraph line with pipes. */
};
#define FENCE_LEN(state) ((size_t) (((state) >> 2) & 63))
#define FENCE_INDENT(state) ((size_t) (((state) >> 8) & 63))
#define STATE_CELLS(state) ((size_t) (((state) >> S_CELLS_SHIFT) & 255))

enum BlockKind {
	B_BLANK,
	B_PARA,
	B_ATX,
	B_SETEXT_TEXT,		/* The text of a setext heading. */
	B_SETEXT,		/* Its underline. */
	B_HR,
	B_FENCE_OPEN,
	B_FENCE,		/* A closing fence. */
	B_CODE,			/* In a fenced code block. */
	B_ICODE,		/* An indented code block. */
	B_QUOTE,
	B_LIST,
	B_THEAD,
	B_TRULE,		/* A table's delimiter row. */
	B_TROW,
};

#define MAX_CELLS 100
#define MAX_QUOTES 8

/* A line of a pipe table: the text between its pipes, and the pipes. */
typedef struct {
	size_t start[MAX_CELLS], end[MAX_CELLS];
	size_t n;
	size_t pipe[MAX_CELLS + 1];
	size_t npipes;
	size_t first, last;	/* Where its text starts and ends. */
	bool lead, trail;	/* It has outer pipes. */
} Row;

typedef struct {
	enum BlockKind kind;
	int level;		/* Of a heading, or 0. */
	size_t hstart;		/* Where an ATX heading's #s start. */
	size_t cs, ce;		/* The text parsed for inline markup. */
	size_t quote[MAX_QUOTES], nquote;
	bool list, bullet;
	size_t mstart, mend;	/* The list marker. */
	int task;		/* 0, or 1 for [ ] and 2 for [x]. */
	size_t tpos;		/* The [ of a task box. */
	Row row;		/* Of a table line. */
} LineInfo;

/* A line, parsed. */
typedef struct {
	char *s;
	size_t len;
	LineInfo li;
	unsigned *style;
	unsigned char *hide;	/* Markup not shown when rendered. */
	unsigned char *subst;	/* A symbol shown instead, if not 0. */
} Analysis;

enum { SYM_NONE, SYM_BULLET, SYM_QUOTE, SYM_BOX, SYM_CHECKED, SYM_VBAR, SYM_HBAR };

static const char *const symbols[] = {
	"", "\xe2\x80\xa2", "\xe2\x94\x82", "\xe2\x98\x90", "\xe2\x98\x91", "\xe2\x94\x82", "\xe2\x94\x80",
};

/* The windows a buffer was last drawn in, and which of its lines (as
   line starts, inclusive) were shown as source there. */
#define MAX_SHOWN 8
struct Markdown {
	bool source;		/* Show all of it as source. */
	struct {
		unsigned long win;
		size_t lo, hi;
	} shown[MAX_SHOWN];
	size_t nshown, next_shown;
};

static Binding *markdown_map;
static const Mode markdown_syntax;

static bool
markdown_active (const Buffer *bp)
{
	return bp->markdown != NULL && markdown_map != NULL && bp->keymap == markdown_map;
}

const Mode *
markdown_highlighter (const Buffer *bp)
{
	return markdown_active (bp) ? &markdown_syntax : NULL;
}

void
markdown_free (Buffer *bp)
{
	free (bp->markdown);
	bp->markdown = NULL;
}

/* Symbols are shown only where the locale can show them. */
static bool
use_symbols (void)
{
	return MB_CUR_MAX > 1;
}


/*--------------------------------------------------------------------------
 * Lines and their blocks.
 *--------------------------------------------------------------------------*/

static bool
is_ws (char c)
{
	return c == ' ' || c == '\t';
}

static bool
is_space (unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static bool
is_punct (unsigned char c)
{
	return c < 0x80 && ispunct (c);
}

static char *
fetch_line (Buffer *bp, size_t lo, size_t *len)
{
	size_t n = buffer_end_of_line (bp, lo) - lo;
	char *s = xmalloc (n + 1);
	for (size_t i = 0; i < n; i++)
		s[i] = buffer_get_char (bp, lo + i);
	s[n] = '\0';
	*len = n;
	return s;
}

/* The end of the indentation from byte `i', with its columns. */
static size_t
skip_indent (const char *s, size_t len, size_t i, size_t *cols)
{
	size_t col = 0;
	for (; i < len && is_ws (s[i]); i++)
		col = s[i] == '\t' ? col + 4 - col % 4 : col + 1;
	*cols = col;
	return i;
}

static size_t
run_len (const char *s, size_t len, size_t i, char c)
{
	size_t n = 0;
	while (i + n < len && s[i + n] == c)
		n++;
	return n;
}

static bool
blank_from (const char *s, size_t len, size_t i)
{
	for (; i < len; i++)
		if (!is_ws (s[i]))
			return false;
	return true;
}

static size_t
trim_end (const char *s, size_t start, size_t end)
{
	while (end > start && is_ws (s[end - 1]))
		end--;
	return end;
}

/* The end of the code span whose backticks start at `i', at the start
   of its closing backticks, or NOPOS if it is not closed. */
static size_t
code_span_close (const char *s, size_t len, size_t i, size_t n)
{
	for (size_t j = i + n; j < len;) {
		if (s[j] != '`') {
			j++;
			continue;
		}
		size_t m = run_len (s, len, j, '`');
		if (m == n)
			return j;
		j += m;
	}
	return NOPOS;
}

/* Split a line at its pipes, leaving out escaped pipes and those in
   code spans; false if it has none. */
static bool
row_split (const char *s, size_t len, Row *row)
{
	size_t col, i = skip_indent (s, len, 0, &col), e = trim_end (s, i, len);
	row->n = row->npipes = 0;
	row->first = i;
	row->last = e;
	if (i == e)
		return false;
	for (size_t j = i; j < e;) {
		if (s[j] == '\\' && j + 1 < e) {
			j += 2;
			continue;
		}
		if (s[j] == '`') {
			size_t n = run_len (s, e, j, '`'), close = code_span_close (s, e, j, n);
			j = close != NOPOS ? close + n : j + n;
			continue;
		}
		if (s[j] == '|') {
			if (row->npipes > MAX_CELLS)
				return false;
			row->pipe[row->npipes++] = j;
		}
		j++;
	}
	if (row->npipes == 0)
		return false;
	row->lead = s[i] == '|';
	row->trail = row->pipe[row->npipes - 1] == e - 1 && !(row->lead && row->npipes == 1);
	size_t p = row->lead ? 1 : 0, start = row->lead ? row->pipe[0] + 1 : i;
	for (;;) {
		if (p >= row->npipes && row->trail)
			break;
		if (row->n >= MAX_CELLS)
			return false;
		row->start[row->n] = start;
		row->end[row->n] = p < row->npipes ? row->pipe[p] : e;
		row->n++;
		if (p >= row->npipes)
			break;
		start = row->pipe[p++] + 1;
	}
	return row->n > 0;
}

/* The text of cell `k' of `row', without its surrounding spaces. */
static void
cell_text (const char *s, const Row *row, size_t k, size_t *ts, size_t *te)
{
	size_t a = row->start[k], b = row->end[k];
	while (a < b && is_ws (s[a]))
		a++;
	*ts = a;
	*te = trim_end (s, a, b);
}

/* Whether `row' is a table's delimiter row, such as |:--|--:|. */
static bool
delim_row (const char *s, const Row *row)
{
	for (size_t k = 0; k < row->n; k++) {
		size_t a, b;
		cell_text (s, row, k, &a, &b);
		if (a < b && s[a] == ':')
			a++;
		if (b > a && s[b - 1] == ':')
			b--;
		if (a == b)
			return false;
		for (; a < b; a++)
			if (s[a] != '-')
				return false;
	}
	return true;
}

static bool
is_hr (const char *s, size_t len, size_t i)
{
	if (i >= len || (s[i] != '-' && s[i] != '*' && s[i] != '_'))
		return false;
	char c = s[i];
	size_t n = 0;
	for (; i < len; i++)
		if (s[i] == c)
			n++;
		else if (!is_ws (s[i]))
			return false;
	return n >= 3;
}

static bool
parse_atx (const char *s, size_t len, size_t i, LineInfo *li)
{
	size_t n = run_len (s, len, i, '#');
	if (n == 0 || n > 6 || (i + n < len && !is_ws (s[i + n])))
		return false;
	li->level = (int) n;
	li->hstart = i;
	size_t cs = i + n;
	while (cs < len && is_ws (s[cs]))
		cs++;
	size_t ce = trim_end (s, cs, len), k = ce;
	while (k > cs && s[k - 1] == '#')
		k--;
	if (k < ce && (k == cs || is_ws (s[k - 1])))
		ce = trim_end (s, cs, k);
	li->cs = cs;
	li->ce = ce;
	return true;
}

static bool
list_marker (const char *s, size_t len, size_t i, size_t *end, bool *bullet)
{
	if (i >= len)
		return false;
	size_t j = i;
	if (s[i] == '-' || s[i] == '*' || s[i] == '+') {
		j = i + 1;
		*bullet = true;
	} else {
		while (j < len && j - i < 9 && isdigit ((unsigned char) s[j]))
			j++;
		if (j == i || j >= len || (s[j] != '.' && s[j] != ')'))
			return false;
		j++;
		*bullet = false;
	}
	if (j < len && !is_ws (s[j]))
		return false;
	*end = j;
	return true;
}

static bool
is_setext_line (const char *s, size_t len)
{
	size_t col, i = skip_indent (s, len, 0, &col);
	return col < 4 && i < len && (s[i] == '=' || s[i] == '-') &&
		blank_from (s, len, i + run_len (s, len, i, s[i]));
}

/* Classify the line `s', given the state at its start and the next
   line (NULL if there is none); return the state at the next line. The
   state depends only on this line and those before it. */
static int
classify (const char *s, size_t len, int state, const char *next, size_t nlen, LineInfo *li)
{
	memset (li, 0, sizeof *li);
	size_t col, i = skip_indent (s, len, 0, &col);
	int list = state & S_LIST;

	if (state & S_FENCE) {
		char fc = state & S_TILDE ? '~' : '`';
		size_t n = run_len (s, len, i, fc);
		if (n >= FENCE_LEN (state) && col < FENCE_INDENT (state) + 4 && blank_from (s, len, i + n)) {
			li->kind = B_FENCE;
			return S_NONBLANK | list;
		}
		li->kind = B_CODE;
		return state;
	}
	if (i == len) {
		li->kind = B_BLANK;
		return list;
	}

	bool para = (state & S_PARA) != 0;
	/* A list goes on until a line that starts at the margin follows a
	   blank line. */
	int in_list = list && ((state & S_NONBLANK) || col > 0) ? S_LIST : 0;

	if ((col < 4 || list) && (s[i] == '`' || s[i] == '~')) {
		char c = s[i];
		size_t n = run_len (s, len, i, c);
		if (n >= 3 && (c == '~' || memchr (s + i + n, '`', len - i - n) == NULL)) {
			li->kind = B_FENCE_OPEN;
			li->cs = i + n;
			return S_FENCE | (c == '~' ? S_TILDE : 0) | (int) (MIN (n, 63) << 2) |
				(int) (MIN (col, 63) << 8) | in_list;
		}
	}
	if (col >= 4 && !para && !list) {
		li->kind = B_ICODE;
		return S_NONBLANK;
	}
	if (col < 4 && parse_atx (s, len, i, li)) {
		li->kind = B_ATX;
		return S_NONBLANK;
	}
	size_t hcells = STATE_CELLS (state);
	if (hcells > 0 && col < 4 && row_split (s, len, &li->row) && li->row.n == hcells &&
	    delim_row (s, &li->row)) {
		li->kind = B_TRULE;
		return S_NONBLANK | S_TABLE;
	}
	if (para && is_setext_line (s, len)) {
		li->kind = B_SETEXT;
		li->level = s[i] == '=' ? 1 : 2;
		return S_NONBLANK;
	}
	if (col < 4 && is_hr (s, len, i)) {
		li->kind = B_HR;
		return S_NONBLANK;
	}
	size_t me;
	bool bullet;
	if ((state & S_TABLE) && col < 4 && s[i] != '>' && !list_marker (s, len, i, &me, &bullet) &&
	    row_split (s, len, &li->row)) {
		li->kind = B_TROW;
		return S_NONBLANK | S_TABLE;
	}

	size_t j = i;
	if (col < 4 && s[i] == '>') {
		while (j < len && s[j] == '>' && li->nquote < MAX_QUOTES) {
			li->quote[li->nquote++] = j++;
			if (j < len && is_ws (s[j]))
				j++;
			size_t c2, k = skip_indent (s, len, j, &c2);
			if (k < len && s[k] == '>' && c2 < 4)
				j = k;
		}
		size_t c2;
		j = skip_indent (s, len, j, &c2);
		if (parse_atx (s, len, j, li)) {
			li->kind = B_QUOTE;
			return S_NONBLANK;
		}
	}
	if (list_marker (s, len, j, &me, &bullet)) {
		li->list = true;
		li->bullet = bullet;
		li->mstart = j;
		li->mend = me;
		size_t p = me;
		while (p < len && is_ws (s[p]))
			p++;
		if (p + 2 < len && s[p] == '[' && (s[p + 1] == ' ' || s[p + 1] == 'x' || s[p + 1] == 'X') &&
		    s[p + 2] == ']' && (p + 3 == len || is_ws (s[p + 3]))) {
			li->task = s[p + 1] == ' ' ? 1 : 2;
			li->tpos = p;
			p += 3;
			while (p < len && is_ws (s[p]))
				p++;
		}
		li->cs = p;
		li->ce = trim_end (s, p, len);
		li->kind = li->nquote > 0 ? B_QUOTE : B_LIST;
		return S_NONBLANK | (li->nquote > 0 ? 0 : S_LIST);
	}
	li->cs = j;
	li->ce = trim_end (s, j, len);
	if (li->nquote > 0) {
		li->kind = B_QUOTE;
		return S_NONBLANK;
	}

	li->kind = B_PARA;
	size_t cells = col < 4 && row_split (s, len, &li->row) ? li->row.n : 0;
	if (next != NULL && is_setext_line (next, nlen)) {
		size_t c2, k = skip_indent (next, nlen, 0, &c2);
		li->kind = B_SETEXT_TEXT;
		li->level = next[k] == '=' ? 1 : 2;
	} else if (next != NULL && cells > 0) {
		Row nr;
		size_t c2;
		skip_indent (next, nlen, 0, &c2);
		if (c2 < 4 && row_split (next, nlen, &nr) && nr.n == cells && delim_row (next, &nr))
			li->kind = B_THEAD;
	}
	return S_NONBLANK | S_PARA | (int) (MIN (cells, 255) << S_CELLS_SHIFT) | in_list;
}


/*--------------------------------------------------------------------------
 * Inline markup.
 *--------------------------------------------------------------------------*/

/* An autolink such as <https://example.org> or <me@example.org> at
   byte `i'; `*end' is its >. */
static bool
autolink (const char *s, size_t i, size_t b, size_t *end)
{
	size_t j = i + 1, k = j;
	if (j < b && isalpha ((unsigned char) s[j])) {
		while (k < b && (isalnum ((unsigned char) s[k]) || s[k] == '+' || s[k] == '.' || s[k] == '-'))
			k++;
		if (k - j >= 2 && k - j <= 32 && k < b && s[k] == ':') {
			for (k++; k < b && s[k] != '>' && s[k] != '<' && !is_space ((unsigned char) s[k]) &&
				     (unsigned char) s[k] >= 0x20; k++)
				;
			if (k < b && s[k] == '>') {
				*end = k;
				return true;
			}
		}
	}
	bool at = false;
	for (k = j; k < b && s[k] != '\0' &&
		     (isalnum ((unsigned char) s[k]) || strchr (".!#$%&'*+/=?^_`{|}~-@", s[k]) != NULL); k++)
		at |= s[k] == '@';
	if (at && k > j && k < b && s[k] == '>' && s[j] != '@' && s[k - 1] != '@') {
		*end = k;
		return true;
	}
	return false;
}

typedef struct {
	size_t pos, len, orig;
	char ch;
	bool open, close;
	int group;		/* Delimiters match only in the same link text. */
} Delim;

/* Mark the inline markup of bytes [a, b) of `s': code spans, escapes,
   autolinks, links, and emphasis, strong emphasis and strikethrough by
   CommonMark's delimiter rules. Images are left as source. */
static void
inline_parse (const char *s, size_t a, size_t b, unsigned base, unsigned *style, unsigned char *hide)
{
	for (size_t i = a; i < b; i++)
		style[i] |= base;
	if (b <= a)
		return;
	size_t n = b - a;
	unsigned char *lit = xzalloc (n);	/* Not markup. */
	int *group = xzalloc (n * sizeof (int));

	for (size_t i = a, end; i < b;) {
		char c = s[i];
		if (c == '\\' && i + 1 < b && is_punct ((unsigned char) s[i + 1])) {
			hide[i] = 1;
			lit[i - a] = lit[i + 1 - a] = 1;
			i += 2;
		} else if (c == '`') {
			size_t r = run_len (s, b, i, '`'), close = code_span_close (s, b, i, r);
			if (close == NOPOS) {
				memset (lit + (i - a), 1, r);
				i += r;
				continue;
			}
			memset (lit + (i - a), 1, close + r - i);
			memset (hide + i, 1, r);
			memset (hide + close, 1, r);
			size_t cs = i + r, ce = close;
			bool all_spaces = true;
			for (size_t k = cs; k < ce; k++)
				all_spaces &= s[k] == ' ';
			if (ce - cs >= 2 && s[cs] == ' ' && s[ce - 1] == ' ' && !all_spaces)
				hide[cs] = hide[ce - 1] = 1;
			for (size_t k = cs; k < ce; k++)
				style[k] |= MD_ICODE;
			i = close + r;
		} else if (c == '<' && autolink (s, i, b, &end)) {
			memset (lit + (i - a), 1, end + 1 - i);
			hide[i] = hide[end] = 1;
			for (size_t k = i + 1; k < end; k++)
				style[k] |= MD_LINK;
			i = end + 1;
		} else
			i++;
	}

	int ngroups = 0;
	for (size_t i = a; i < b; i++) {
		if (s[i] != '[' || lit[i - a])
			continue;
		size_t depth = 0, j;
		for (j = i; j < b; j++) {
			if (lit[j - a])
				continue;
			if (s[j] == '[')
				depth++;
			else if (s[j] == ']' && --depth == 0)
				break;
		}
		if (j + 1 >= b || s[j + 1] != '(')
			continue;
		size_t k, parens = 0;
		for (k = j + 1; k < b; k++) {
			if (s[k] == '\\' && k + 1 < b)
				k++;
			else if (s[k] == '(')
				parens++;
			else if (s[k] == ')' && --parens == 0)
				break;
		}
		if (k >= b)
			continue;
		if (i > a && s[i - 1] == '!' && !lit[i - 1 - a]) {
			for (size_t m = i - 1; m <= k; m++) {
				lit[m - a] = 1;
				style[m] |= MD_LINK;
			}
		} else {
			hide[i] = 1;
			lit[i - a] = 1;
			for (size_t m = j; m <= k; m++) {
				hide[m] = 1;
				lit[m - a] = 1;
			}
			ngroups++;
			for (size_t m = i + 1; m < j; m++) {
				style[m] |= MD_LINK;
				if (group[m - a] == 0)
					group[m - a] = ngroups;
			}
		}
		i = k;
	}

	Delim *d = NULL;
	size_t nd = 0, cap = 0;
	for (size_t i = a; i < b;) {
		char c = s[i];
		if ((c != '*' && c != '_' && c != '~') || lit[i - a]) {
			i++;
			continue;
		}
		size_t r = 1;
		while (i + r < b && s[i + r] == c && !lit[i + r - a])
			r++;
		unsigned char before = i > a ? (unsigned char) s[i - 1] : ' ';
		unsigned char after = i + r < b ? (unsigned char) s[i + r] : ' ';
		bool left = !is_space (after) && (!is_punct (after) || is_space (before) || is_punct (before));
		bool right = !is_space (before) && (!is_punct (before) || is_space (after) || is_punct (after));
		bool op = left, cl = right;
		if (c == '_') {
			op = left && (!right || is_punct (before));
			cl = right && (!left || is_punct (after));
		} else if (c == '~' && r > 2)
			op = cl = false;
		if ((op || cl) && nd < 1024) {
			if (nd == cap) {
				cap = cap ? cap * 2 : 16;
				d = xrealloc (d, cap * sizeof (Delim));
			}
			d[nd++] = (Delim) { i, r, r, c, op, cl, group[i - a] };
		}
		i += r;
	}
	for (size_t ci = 0; ci < nd; ci++) {
		Delim *cd = &d[ci];
		while (cd->close && cd->len > 0) {
			size_t oi = ci;
			bool found = false;
			while (oi-- > 0) {
				Delim *od = &d[oi];
				if (od->ch != cd->ch || !od->open || od->len == 0 || od->group != cd->group)
					continue;
				if (cd->ch == '~') {
					if (od->len != cd->len)
						continue;
				} else if ((od->close || cd->open) && (od->orig + cd->orig) % 3 == 0 &&
					   !(od->orig % 3 == 0 && cd->orig % 3 == 0))
					continue;
				found = true;
				break;
			}
			if (!found)
				break;
			Delim *od = &d[oi];
			size_t use = cd->ch == '~' ? cd->len : od->len >= 2 && cd->len >= 2 ? 2 : 1;
			unsigned st = cd->ch == '~' ? MD_STRIKE : use == 2 ? MD_BOLD : MD_ITALIC;
			size_t ostart = od->pos + od->len - use, cstart = cd->pos + (cd->orig - cd->len);
			memset (hide + ostart, 1, use);
			memset (hide + cstart, 1, use);
			for (size_t k = ostart + use; k < cstart; k++)
				style[k] |= st;
			od->len -= use;
			cd->len -= use;
			for (size_t m = oi + 1; m < ci; m++)
				d[m].open = d[m].close = false;
		}
	}
	free (d);
	free (group);
	free (lit);
}

/* Mark the styles, hidden markup and symbols of a classified line. */
static void
decorate (Analysis *an)
{
	LineInfo *li = &an->li;
	const char *s = an->s;
	size_t len = an->len;
	bool sym = use_symbols ();
	unsigned all = 0;

	switch (li->kind) {
	case B_BLANK:
		return;
	case B_FENCE_OPEN:
	case B_FENCE:
		all = MD_CODE | MD_MARKUP;
		break;
	case B_CODE:
	case B_ICODE:
		all = MD_CODE;
		break;
	case B_SETEXT:
		all = MD_MARKUP;
		break;
	case B_HR:
		all = MD_HR;
		break;
	case B_TRULE:
		for (size_t i = 0; i < len; i++) {
			an->style[i] = MD_TABLE;
			if (sym && i >= li->row.first && i < li->row.last)
				an->subst[i] = s[i] == '|' ? SYM_VBAR : s[i] == ':' ? SYM_NONE : SYM_HBAR;
		}
		return;
	case B_THEAD:
	case B_TROW:
		for (size_t p = 0; p < li->row.npipes; p++) {
			an->style[li->row.pipe[p]] = MD_TABLE;
			if (sym)
				an->subst[li->row.pipe[p]] = SYM_VBAR;
		}
		for (size_t k = 0; k < li->row.n; k++) {
			size_t ts, te;
			cell_text (s, &li->row, k, &ts, &te);
			inline_parse (s, ts, te, li->kind == B_THEAD ? MD_THEAD : 0, an->style, an->hide);
		}
		return;
	default:
		break;
	}
	if (all != 0) {
		for (size_t i = 0; i < len; i++)
			an->style[i] = all;
		return;
	}

	unsigned base = 0;
	for (size_t q = 0; q < li->nquote; q++) {
		an->style[li->quote[q]] |= MD_QUOTE | MD_MARKUP;
		if (sym)
			an->subst[li->quote[q]] = SYM_QUOTE;
		base |= MD_QUOTE;
	}
	for (size_t i = li->nquote > 0 ? li->quote[li->nquote - 1] + 1 : 0; i < len; i++)
		an->style[i] |= base;
	if (li->list) {
		for (size_t i = li->mstart; i < li->mend; i++)
			an->style[i] |= MD_LIST;
		if (sym && li->bullet)
			an->subst[li->mstart] = SYM_BULLET;
		if (li->task) {
			for (size_t i = li->tpos; i < li->tpos + 3; i++)
				an->style[i] |= MD_LIST;
			if (sym) {
				an->subst[li->tpos] = li->task == 2 ? SYM_CHECKED : SYM_BOX;
				an->hide[li->tpos + 1] = an->hide[li->tpos + 2] = 1;
			}
			if (li->task == 2)
				base |= MD_DONE;
		}
	}
	if (li->level > 0) {
		base |= MD_HEADING (li->level);
		if (li->kind == B_ATX || li->kind == B_QUOTE) {
			memset (an->hide + li->hstart, 1, li->cs - li->hstart);
			memset (an->hide + li->ce, 1, len - li->ce);
		}
	}
	inline_parse (s, li->cs, li->ce, base, an->style, an->hide);
}

static void
analyze (Buffer *bp, size_t lo, int state, Analysis *an, int *next_state)
{
	an->s = fetch_line (bp, lo, &an->len);
	size_t nlo = buffer_next_line (bp, lo), nlen = 0;
	char *next = nlo != NOPOS ? fetch_line (bp, nlo, &nlen) : NULL;
	int ns = classify (an->s, an->len, state, next, nlen, &an->li);
	free (next);
	if (next_state != NULL)
		*next_state = ns;
	size_t n = MAX (an->len, 1);
	an->style = xzalloc (n * sizeof (unsigned));
	an->hide = xzalloc (n);
	an->subst = xzalloc (n);
	decorate (an);
}

static void
analysis_free (Analysis *an)
{
	free (an->s);
	free (an->style);
	free (an->hide);
	free (an->subst);
}

/* The analysis of the line at `lo', in the state the buffer has for it. */
static void
analyze_line (Buffer *bp, size_t lo, Analysis *an)
{
	int state = buffer_get_line_start_state (bp, buffer_offset_to_line (bp, lo));
	analyze (bp, lo, state, an, NULL);
}


/*--------------------------------------------------------------------------
 * The source view's highlighting.
 *--------------------------------------------------------------------------*/

static const char *const heading_faces[] = {
	NULL, FACE_MARKDOWN_HEADER_1, FACE_MARKDOWN_HEADER_2, FACE_MARKDOWN_HEADER_3,
	FACE_MARKDOWN_HEADER_4, FACE_MARKDOWN_HEADER_5, FACE_MARKDOWN_HEADER_6, FACE_MARKDOWN_HEADER_6,
};

/* The one face for a byte of source with style `st'. */
static const char *
source_face (unsigned st)
{
	if (st & MD_CODE)
		return FACE_MARKDOWN_CODE;
	if (st & MD_TABLE)
		return FACE_MARKDOWN_TABLE;
	if (st & MD_HR)
		return FACE_MARKDOWN_HR;
	if (st & MD_MARKUP)
		return FACE_MARKDOWN_MARKUP;
	if (st & MD_ICODE)
		return FACE_MARKDOWN_INLINE_CODE;
	if (st & MD_LINK)
		return FACE_MARKDOWN_LINK;
	if (st & MD_LIST)
		return FACE_MARKDOWN_LIST;
	if (MD_LEVEL (st))
		return heading_faces[MD_LEVEL (st)];
	if (st & (MD_BOLD | MD_THEAD))
		return FACE_MARKDOWN_BOLD;
	if (st & MD_ITALIC)
		return FACE_MARKDOWN_ITALIC;
	if (st & MD_STRIKE)
		return FACE_MARKDOWN_STRIKE;
	if (st & MD_DONE)
		return FACE_MARKDOWN_TASK_DONE;
	if (st & MD_QUOTE)
		return FACE_MARKDOWN_BLOCKQUOTE;
	return NULL;
}

static int
markdown_scan_line (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
		    size_t len, int start_state, const char **face_names)
{
	(void) mode;
	(void) line_idx;
	Analysis an;
	int state;
	analyze (bp, start_o, start_state, &an, &state);
	for (size_t i = 0; i < len && i < an.len; i++)
		face_names[i] = an.hide[i] ? FACE_MARKDOWN_MARKUP : source_face (an.style[i]);
	analysis_free (&an);
	return state;
}

static const CommentStyle markdown_comment_style = { NULL, "<!--", "-->", false };

static const Mode markdown_syntax = {
	"Markdown", markdown_scan_line, &markdown_comment_style, NULL, 0,
};


/*--------------------------------------------------------------------------
 * The rendered view.
 *--------------------------------------------------------------------------*/

typedef struct {
	char text[8];
	unsigned char len, width;
	bool pad;		/* Not a character of the line. */
	size_t src;		/* The byte of the line it shows. */
	unsigned style;
} Glyph;

typedef struct {
	Glyph *v;
	size_t n, cap;
	size_t cols;
} Glyphs;

static void
glyph_add (Glyphs *g, const char *text, size_t len, size_t width, size_t src, unsigned style, bool pad)
{
	if (g->n == g->cap) {
		g->cap = g->cap ? g->cap * 2 : 64;
		g->v = xrealloc (g->v, g->cap * sizeof (Glyph));
	}
	Glyph *gl = &g->v[g->n++];
	len = MIN (len, sizeof gl->text - 1);
	memcpy (gl->text, text, len);
	gl->text[len] = '\0';
	gl->len = (unsigned char) len;
	gl->width = (unsigned char) width;
	gl->src = src;
	gl->style = style;
	gl->pad = pad;
	g->cols += width;
}

static void
glyph_pad (Glyphs *g, size_t n, size_t src, unsigned style)
{
	for (size_t i = 0; i < n; i++)
		glyph_add (g, " ", 1, 1, src, style, true);
}

static void
glyph_symbol (Glyphs *g, int sym, size_t src, unsigned style)
{
	const char *t = symbols[sym];
	size_t n;
	uint32_t ch = utf8_decode (t, strlen (t), 0, &n);
	glyph_add (g, t, strlen (t), (size_t) utf8_char_display_width (ch), src, style, false);
}

/* Add the character of `s' at byte `i', as draw_line shows it. */
static size_t
glyph_char (Glyphs *g, const char *s, size_t len, size_t i, unsigned style, size_t tab)
{
	size_t n;
	uint32_t ch = utf8_decode (s, len, i, &n);
	char buf[8];
	if (ch == '\t') {
		for (size_t k = tab - g->cols % tab; k > 0; k--)
			glyph_add (g, " ", 1, 1, i, style, false);
	} else if (ch < 0x20 || ch == 0x7F) {
		buf[0] = '^';
		buf[1] = ch == 0x7F ? '?' : (char) ('@' + ch);
		glyph_add (g, buf, 1, 1, i, style, false);
		glyph_add (g, buf + 1, 1, 1, i, style, false);
	} else if ((ch >= 0x80 && ch < 0xA0) || ch == UTF8_REPLACEMENT) {
		int k = ch == UTF8_REPLACEMENT ? snprintf (buf, sizeof buf, "?")
			: snprintf (buf, sizeof buf, "\\%o", (unsigned) ch);
		for (int m = 0; m < k; m++)
			glyph_add (g, buf + m, 1, 1, i, style, false);
	} else
		glyph_add (g, s + i, n, (size_t) utf8_char_display_width (ch), i, style, false);
	return n;
}

static void
glyph_range (Glyphs *g, const Analysis *an, size_t from, size_t to, size_t tab)
{
	for (size_t i = from; i < to;) {
		if (an->hide[i])
			i++;
		else if (an->subst[i]) {
			glyph_symbol (g, an->subst[i], i, an->style[i]);
			size_t n;
			utf8_decode (an->s, an->len, i, &n);
			i += n;
		} else
			i += glyph_char (g, an->s, an->len, i, an->style[i], tab);
	}
}

/* The source width of bytes [from, to) of `s', which starts at column
   `col'. */
static size_t
source_width (const char *s, size_t len, size_t from, size_t to, size_t col, size_t tab)
{
	size_t c = col;
	for (size_t i = from; i < to;) {
		size_t n;
		uint32_t ch = utf8_decode (s, len, i, &n);
		c += ch == '\t' ? tab - c % tab : (size_t) utf8_char_display_width (ch);
		i += n;
	}
	return c - col;
}

/* The glyphs of the line at `lo' rendered, `fill' columns wide for
   rules and code blocks. */
static void
layout (Buffer *bp, size_t lo, size_t fill, Glyphs *g)
{
	Analysis an;
	analyze_line (bp, lo, &an);
	size_t tab = buffer_tab_width (bp);
	memset (g, 0, sizeof *g);
	const LineInfo *li = &an.li;

	switch (li->kind) {
	case B_HR:
		if (use_symbols ()) {
			size_t col, start = skip_indent (an.s, an.len, 0, &col);
			glyph_range (g, &an, 0, start, tab);
			while (g->cols < MIN (fill, 1000))
				glyph_symbol (g, SYM_HBAR, start, MD_HR);
		} else
			glyph_range (g, &an, 0, an.len, tab);
		break;
	case B_THEAD:
	case B_TROW: {
		/* Each cell is padded to its width in the source, so that an
		   aligned table stays aligned with its markup hidden. */
		const Row *row = &li->row;
		size_t at = 0;
		for (size_t k = 0; k < row->n; k++) {
			size_t ts, te;
			cell_text (an.s, row, k, &ts, &te);
			glyph_range (g, &an, at, row->start[k], tab);
			size_t start_cols = g->cols;
			glyph_range (g, &an, row->start[k], te, tab);
			size_t shown = g->cols - start_cols;
			size_t width = source_width (an.s, an.len, row->start[k], te, start_cols, tab);
			if (shown < width)
				glyph_pad (g, width - shown, te, 0);
			at = te;
		}
		glyph_range (g, &an, at, an.len, tab);
		break;
	}
	default:
		glyph_range (g, &an, 0, an.len, tab);
		if (li->kind == B_CODE || li->kind == B_ICODE || li->kind == B_FENCE_OPEN || li->kind == B_FENCE)
			if (g->cols < fill)
				glyph_pad (g, MIN (fill, 1000) - MIN (g->cols, 1000), an.len, MD_CODE);
		break;
	}
	analysis_free (&an);
}

static void
apply_style (unsigned st, bool region)
{
	if (region) {
		term_apply_face (FACE_REGION);
		return;
	}
	const char *faces[16];
	size_t n = 0;
	faces[n++] = FACE_DEFAULT;
	if (st & MD_QUOTE)
		faces[n++] = FACE_MARKDOWN_BLOCKQUOTE;
	if (st & MD_LIST)
		faces[n++] = FACE_MARKDOWN_LIST;
	if (st & MD_TABLE)
		faces[n++] = FACE_MARKDOWN_TABLE;
	if (st & MD_HR)
		faces[n++] = FACE_MARKDOWN_HR;
	if (st & MD_CODE)
		faces[n++] = FACE_MARKDOWN_CODE;
	if (st & MD_MARKUP)
		faces[n++] = FACE_MARKDOWN_MARKUP;
	if (MD_LEVEL (st))
		faces[n++] = heading_faces[MD_LEVEL (st)];
	if (st & (MD_BOLD | MD_THEAD))
		faces[n++] = FACE_MARKDOWN_BOLD;
	if (st & MD_ITALIC)
		faces[n++] = FACE_MARKDOWN_ITALIC;
	if (st & MD_STRIKE)
		faces[n++] = FACE_MARKDOWN_STRIKE;
	if (st & MD_DONE)
		faces[n++] = FACE_MARKDOWN_TASK_DONE;
	if (st & MD_LINK)
		faces[n++] = FACE_MARKDOWN_LINK;
	if (st & MD_ICODE)
		faces[n++] = FACE_MARKDOWN_INLINE_CODE;
	TerminalStyle style;
	if (resolve_terminal_faces (faces, n, &style))
		term_apply_style (&style);
	else
		term_attrset (FONT_NORMAL);
}

/* The lines of `wp' shown as source: point's, and those of the region
   if it is highlighted (`r'). */
static void
source_lines (Window *wp, const Region *r, size_t *lo, size_t *hi)
{
	Buffer *bp = wp->bp;
	*lo = *hi = buffer_start_of_line (bp, window_o (wp));
	if (r != NULL) {
		*lo = MIN (*lo, buffer_start_of_line (bp, r->start));
		*hi = MAX (*hi, buffer_start_of_line (bp, r->end));
	}
}

static void
note_shown (Markdown *md, unsigned long win, size_t lo, size_t hi)
{
	size_t i;
	for (i = 0; i < md->nshown && md->shown[i].win != win; i++)
		;
	if (i == md->nshown) {
		if (md->nshown < MAX_SHOWN)
			md->nshown++;
		else
			i = md->next_shown++ % MAX_SHOWN;
	}
	md->shown[i].win = win;
	md->shown[i].lo = lo;
	md->shown[i].hi = hi;
}

/* Whether the line at `lo' shows rendered in `wp', as last drawn (or as
   it would be drawn, if not yet). */
static bool
shown_rendered (Window *wp, size_t lo)
{
	Buffer *bp = wp->bp;
	if (!markdown_active (bp) || bp->markdown->source)
		return false;
	Markdown *md = bp->markdown;
	size_t slo = NOPOS, shi = NOPOS;
	for (size_t i = 0; i < md->nshown; i++)
		if (md->shown[i].win == wp->id) {
			slo = md->shown[i].lo;
			shi = md->shown[i].hi;
		}
	if (slo == NOPOS) {
		/* Not drawn yet: as it would be. */
		Region r;
		bool highlight = (wp == cur_wp || get_variable_bool ("highlight-nonselected-windows")) &&
			bp->mark != NULL && bp->mark_active;
		if (highlight)
			r = region_new (window_o (wp), bp->mark->o);
		source_lines (wp, highlight ? &r : NULL, &slo, &shi);
	}
	return lo < slo || lo > shi;
}

size_t
markdown_line_rows (Window *wp, size_t lo, size_t width, DisplayRow **rows)
{
	if (!shown_rendered (wp, lo))
		return 0;
	Glyphs g;
	layout (wp->bp, lo, width, &g);
	width = MAX (width, 1);
	size_t n = 0, cap = 4;
	*rows = xmalloc (cap * sizeof (DisplayRow));
	size_t start = 0, col = 0, brk = 0;
	for (size_t k = 0; k < g.n; k++) {
		const Glyph *gl = &g.v[k];
		bool space = gl->len == 1 && gl->text[0] == ' ';
		if (col + gl->width > start + width && col > start) {
			if (n + 1 >= cap)
				*rows = xrealloc (*rows, (cap *= 2) * sizeof (DisplayRow));
			/* A space that does not fit ends the row, unseen; else
			   the row ends after its last space. */
			size_t end = space ? col + gl->width : brk > start ? brk : col;
			(*rows)[n++] = (DisplayRow) { start, end, start };
			start = end;
			if (!space && end == brk) {
				k--;
				continue;
			}
		}
		col += gl->width;
		if (space)
			brk = col;
	}
	if (n + 1 >= cap)
		*rows = xrealloc (*rows, (cap + 1) * sizeof (DisplayRow));
	(*rows)[n++] = (DisplayRow) { start, MAX (col, start), start };
	free (g.v);
	return n;
}

bool
markdown_draw_line (Window *wp, size_t lo, size_t width, size_t startcol, size_t endcol, const Region *r,
		    bool truncate)
{
	Buffer *bp = wp->bp;
	if (!markdown_active (bp) || bp->markdown->source)
		return false;
	size_t slo, shi;
	source_lines (wp, r, &slo, &shi);
	note_shown (bp->markdown, wp->id, slo, shi);
	if (lo >= slo && lo <= shi)
		return false;

	Glyphs g;
	/* Wrapped, `startcol' is where the row starts in a layout to the
	   width; truncated, it is how far the window scrolled sideways. */
	layout (bp, lo, truncate ? width + startcol : width, &g);
	size_t total = g.cols > startcol ? g.cols - startcol : 0;
	bool truncated = truncate && total > width;
	size_t limit = truncated && width > 0 ? width - 1 : width;
	size_t col = 0, x = 0;
	unsigned cur = UINT_MAX;
	bool cur_region = false;
	for (size_t k = 0; k < g.n; k++) {
		const Glyph *gl = &g.v[k];
		size_t w = gl->width;
		if (col + w <= startcol) {
			col += w;
			continue;
		}
		bool region = r != NULL && !gl->pad && region_contains (*r, lo + gl->src);
		if (gl->style != cur || region != cur_region) {
			apply_style (gl->style, region);
			cur = gl->style;
			cur_region = region;
		}
		if (col < startcol) {
			for (size_t visible = col + w - startcol; visible > 0 && x < limit; visible--, x++)
				term_addch (' ');
			col += w;
			continue;
		}
		if (x + w > limit || (!truncate && col + w > endcol))
			break;
		term_addnstr (gl->text, gl->len);
		x += w;
		col += w;
	}
	free (g.v);
	term_apply_face (FACE_DEFAULT);
	for (; x < limit; x++)
		term_addch (' ');
	if (truncated && width > 0)
		term_addch ('$');
	return true;
}

bool
markdown_cell_offset (Window *wp, size_t lo, size_t col, size_t *o, bool *on_text)
{
	Buffer *bp = wp->bp;
	if (!shown_rendered (wp, lo))
		return false;

	size_t text_w = window_text_width (wp);
	Glyphs g;
	layout (bp, lo, window_wraps (wp) ? text_w : text_w + wp->start_column, &g);
	size_t c = 0;
	*o = buffer_end_of_line (bp, lo);
	*on_text = false;
	for (size_t k = 0; k < g.n; k++) {
		const Glyph *gl = &g.v[k];
		if (gl->width == 0)
			continue;
		if (col < c + gl->width) {
			*o = lo + gl->src;
			*on_text = !gl->pad;
			break;
		}
		c += gl->width;
	}
	free (g.v);
	return true;
}


/*--------------------------------------------------------------------------
 * The mode and its commands.
 *--------------------------------------------------------------------------*/

static void
enter_markdown_mode (Buffer *bp)
{
	if (markdown_map == NULL) {
		markdown_map = keymap_new ();
		keymap_bind (markdown_map, "\\C-c\\C-v", "markdown-toggle-source");
		keymap_bind (markdown_map, "\\C-c\\C-a", "markdown-table-align");
		keymap_bind (markdown_map, "\\C-c\\C-sb", "markdown-insert-bold");
		keymap_bind (markdown_map, "\\C-c\\C-si", "markdown-insert-italic");
		keymap_bind (markdown_map, "\\C-c\\C-sc", "markdown-insert-code");
		keymap_bind (markdown_map, "\\C-c\\C-x\\C-x", "markdown-toggle-task");
	}
	if (bp->markdown == NULL)
		bp->markdown = xzalloc (sizeof (Markdown));
	bp->keymap = markdown_map;
	free (bp->mode_label);
	bp->mode_label = xstrdup ("Markdown");
	buffer_set_highlighter (bp, &markdown_syntax);
}

static bool
has_suffix_nocase (const char *s, const char *suffix)
{
	size_t n = strlen (s), m = strlen (suffix);
	if (n < m)
		return false;
	for (size_t i = 0; i < m; i++)
		if (tolower ((unsigned char) s[n - m + i]) != suffix[i])
			return false;
	return true;
}

void
markdown_after_visit (Buffer *bp)
{
	const char *name = bp->filename != NULL ? bp->filename : bp->remote_path;
	if (name != NULL && bp->keymap == NULL && bp->preview == NULL &&
	    (has_suffix_nocase (name, ".md") || has_suffix_nocase (name, ".markdown") ||
	     has_suffix_nocase (name, ".mdown")))
		enter_markdown_mode (bp);
}

static bool
F_markdown_mode (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (preview_showing (cur_bp) || cur_bp->shell != NULL || cur_bp->dired != NULL ||
	    cur_bp->magit != NULL || cur_bp->compilation != NULL) {
		minibuf_error ("Cannot use markdown-mode in this buffer");
		return false;
	}
	enter_markdown_mode (cur_bp);
	return true;
}

static bool
need_markdown_mode (void)
{
	if (markdown_active (cur_bp))
		return true;
	minibuf_error ("Not in markdown-mode");
	return false;
}

static bool
F_markdown_toggle_source (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!need_markdown_mode ())
		return false;
	Markdown *md = cur_bp->markdown;
	md->source = !md->source;
	minibuf_write (md->source ? "Showing the Markdown source" : "Showing the Markdown rendered, but for point's line");
	return true;
}

/* Replace bytes [from, to) of the current buffer with `len' bytes of
   `text', whose lines end as the buffer's do. */
static void
replace_text (size_t from, size_t to, const char *text, size_t len)
{
	buffer_goto_offset (cur_bp, from);
	buffer_replace_estr (cur_bp, to - from, ev_of (text, len, buffer_eol (cur_bp)));
}

static size_t
count_before (size_t o, char c)
{
	size_t n = 0;
	while (n < o && buffer_get_char (cur_bp, o - n - 1) == c)
		n++;
	return n;
}

static size_t
count_after (size_t o, char c)
{
	size_t n = 0, len = buffer_length (cur_bp);
	while (o + n < len && buffer_get_char (cur_bp, o + n) == c)
		n++;
	return n;
}

/* Leave the region's text, now at [start, end), with point after it. */
static void
finish_wrap (size_t start, size_t end)
{
	buffer_goto_offset (cur_bp, end);
	if (cur_bp->mark != NULL)
		marker_move (cur_bp->mark, cur_bp, start);
	cur_bp->mark_active = false;
}

/* Insert a pair of delimiters `n' times `c' around the region, or
   around nothing at point; or take them away if they are around the
   region already. */
static bool
toggle_emphasis (char c, size_t n)
{
	if (!need_markdown_mode () || buffer_warn_if_readonly (cur_bp))
		return false;
	StrBuf sb;
	sb_init (&sb);
	if (cur_bp->mark == NULL || !cur_bp->mark_active) {
		for (size_t i = 0; i < 2 * n; i++)
			sb_addc (&sb, c);
		buffer_insert_estr (cur_bp, ev_of (sb.s, sb.len, eol_lf));
		buffer_goto_offset (cur_bp, buffer_pt (cur_bp) - n);
		sb_free (&sb);
		return true;
	}
	Region r = region_calculate ();
	Estr *t = buffer_get_region (cur_bp, r);
	size_t len = t->len;
	size_t outside = MIN (count_before (r.start, c), count_after (r.end, c));
	size_t in_start = run_len (t->text, len, 0, c), in_end = 0;
	while (in_end < len && t->text[len - 1 - in_end] == c)
		in_end++;
	size_t inside = MIN (in_start, in_end);
	/* *** is both strong and emphasis. */
	if (outside == n || outside == 3) {
		replace_text (r.start - n, r.end + n, t->text, len);
		finish_wrap (r.start - n, r.start - n + len);
	} else if ((inside == n || inside == 3) && len > 2 * n) {
		replace_text (r.start, r.end, t->text + n, len - 2 * n);
		finish_wrap (r.start, r.start + len - 2 * n);
	} else {
		for (size_t i = 0; i < n; i++)
			sb_addc (&sb, c);
		sb_addn (&sb, t->text, len);
		for (size_t i = 0; i < n; i++)
			sb_addc (&sb, c);
		replace_text (r.start, r.end, sb.s, sb.len);
		finish_wrap (r.start, r.start + sb.len);
	}
	estr_free (t);
	sb_free (&sb);
	return true;
}

static bool
F_markdown_insert_bold (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return toggle_emphasis ('*', 2);
}

static bool
F_markdown_insert_italic (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	return toggle_emphasis ('*', 1);
}

static bool
F_markdown_insert_code (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!need_markdown_mode () || buffer_warn_if_readonly (cur_bp))
		return false;
	if (cur_bp->mark == NULL || !cur_bp->mark_active) {
		buffer_insert_estr (cur_bp, ev_cstr ("``"));
		buffer_goto_offset (cur_bp, buffer_pt (cur_bp) - 1);
		return true;
	}
	Region r = region_calculate ();
	Estr *t = buffer_get_region (cur_bp, r);
	const char *s = t->text;
	size_t len = t->len, before = count_before (r.start, '`'), after = count_after (r.end, '`');
	size_t in_start = run_len (s, len, 0, '`'), in_end = 0, longest = 0;
	while (in_end < len && s[len - 1 - in_end] == '`')
		in_end++;
	for (size_t i = 0; i < len;) {
		size_t m = run_len (s, len, i, '`');
		longest = MAX (longest, m);
		i += m > 0 ? m : 1;
	}
	StrBuf sb;
	sb_init (&sb);
	if (before > 0 && before == after) {
		replace_text (r.start - before, r.end + after, s, len);
		finish_wrap (r.start - before, r.start - before + len);
	} else if (in_start > 0 && in_start == in_end && len > 2 * in_start) {
		size_t a = in_start, b = len - in_end;
		if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') {
			a++;
			b--;
		}
		replace_text (r.start, r.end, s + a, b - a);
		finish_wrap (r.start, r.start + b - a);
	} else {
		/* Backticks longer than any in the text, with spaces between
		   them and any at its ends. */
		bool pad = len > 0 && (s[0] == '`' || s[len - 1] == '`');
		for (size_t i = 0; i <= longest; i++)
			sb_addc (&sb, '`');
		size_t fence = sb.len;
		if (pad)
			sb_addc (&sb, ' ');
		sb_addn (&sb, s, len);
		if (pad)
			sb_addc (&sb, ' ');
		for (size_t i = 0; i < fence; i++)
			sb_addc (&sb, '`');
		replace_text (r.start, r.end, sb.s, sb.len);
		finish_wrap (r.start, r.start + sb.len);
	}
	sb_free (&sb);
	estr_free (t);
	return true;
}

static bool
F_markdown_toggle_task (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!need_markdown_mode ())
		return false;
	size_t pt = buffer_pt (cur_bp), lo = buffer_start_of_line (cur_bp, pt);
	Analysis an;
	analyze_line (cur_bp, lo, &an);
	bool task = an.li.list && an.li.task != 0 && an.li.kind != B_CODE;
	bool done = an.li.task == 2;
	size_t at = an.li.tpos + 1;
	analysis_free (&an);
	if (!task) {
		minibuf_error ("Not on a task list item");
		return false;
	}
	if (buffer_warn_if_readonly (cur_bp))
		return false;
	replace_text (lo + at, lo + at + 1, done ? " " : "x", 1);
	buffer_goto_offset (cur_bp, pt);
	return true;
}


/*--------------------------------------------------------------------------
 * Aligning tables.
 *--------------------------------------------------------------------------*/

/* Columns for alignment: East Asian wide characters take two whatever
   the locale, as they do in the terminals and browsers the file may be
   shown in. */
static size_t
align_width (const char *s, size_t len)
{
	static const uint32_t wide[][2] = {
		{ 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x2E80, 0x303E },
		{ 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xA000, 0xA4CF },
		{ 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 },
		{ 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 }, { 0x1F300, 0x1F64F },
		{ 0x1F900, 0x1F9FF }, { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
	};
	size_t cols = 0;
	for (size_t i = 0; i < len;) {
		size_t n;
		uint32_t ch = utf8_decode (s, len, i, &n);
		i += n;
		int w = ch < 0x80 ? 1 : wcwidth ((wchar_t) ch);
		for (size_t k = 0; k < sizeof wide / sizeof wide[0]; k++)
			if (ch >= wide[k][0] && ch <= wide[k][1])
				w = 2;
		if (w < 0)
			w = (ch >= 0x300 && ch < 0x370) || (ch >= 0x200B && ch <= 0x200F) ||
				(ch >= 0xFE00 && ch <= 0xFE0F) ? 0 : 1;
		cols += (size_t) w;
	}
	return cols;
}

typedef struct {
	char *s;
	size_t len;
	Row row;
} TableLine;

enum Align { ALIGN_NONE, ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

static bool
is_table_kind (enum BlockKind k)
{
	return k == B_THEAD || k == B_TRULE || k == B_TROW;
}

static enum BlockKind
line_kind (size_t lo)
{
	Analysis an;
	analyze_line (cur_bp, lo, &an);
	enum BlockKind k = an.li.kind;
	analysis_free (&an);
	return k;
}

static void
add_padded (StrBuf *sb, const char *s, size_t len, size_t width, enum Align align, size_t *at)
{
	size_t w = align_width (s, len), room = width > w ? width - w : 0;
	size_t left = align == ALIGN_RIGHT ? room : align == ALIGN_CENTER ? room / 2 : 0;
	for (size_t i = 0; i < left; i++)
		sb_addc (sb, ' ');
	*at = sb->len;
	sb_addn (sb, s, len);
	for (size_t i = left; i < room; i++)
		sb_addc (sb, ' ');
}

static bool
F_markdown_table_align (long uniarg, Args *args)
{
	(void) uniarg;
	(void) args;
	if (!need_markdown_mode ())
		return false;
	Buffer *bp = cur_bp;
	size_t pt = buffer_pt (bp), lo = buffer_start_of_line (bp, pt);
	if (!is_table_kind (line_kind (lo))) {
		minibuf_error ("Not in a table");
		return false;
	}
	if (buffer_warn_if_readonly (bp))
		return false;

	size_t start = lo;
	while (line_kind (start) != B_THEAD) {
		size_t prev = buffer_prev_line (bp, start);
		if (prev == NOPOS || !is_table_kind (line_kind (prev))) {
			minibuf_error ("Not in a table");
			return false;
		}
		start = prev;
	}
	TableLine *lines = NULL;
	size_t n = 0, cap = 0, prow = 0, end = start;
	for (size_t l = start; l != NOPOS; l = buffer_next_line (bp, l)) {
		enum BlockKind k = line_kind (l);
		if (n == 0 ? k != B_THEAD : n == 1 ? k != B_TRULE : k != B_TROW)
			break;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			lines = xrealloc (lines, cap * sizeof (TableLine));
		}
		TableLine *tl = &lines[n];
		tl->s = fetch_line (bp, l, &tl->len);
		row_split (tl->s, tl->len, &tl->row);
		if (l == lo)
			prow = n;
		end = l + tl->len;
		n++;
	}

	/* The cell point is in, and how far into its text. */
	const TableLine *pl = &lines[prow];
	size_t pcol = pt - lo, pcell = 0, poff = 0;
	for (size_t k = 0; k < pl->row.n; k++) {
		size_t ts, te;
		cell_text (pl->s, &pl->row, k, &ts, &te);
		pcell = k;
		poff = pcol > ts ? MIN (pcol, te) - ts : 0;
		if (pcol <= pl->row.end[k])
			break;
	}

	size_t ncols = lines[0].row.n, hcols = ncols;
	for (size_t i = 0; i < n; i++)
		ncols = MAX (ncols, lines[i].row.n);
	size_t *width = xzalloc (ncols * sizeof (size_t));
	enum Align *align = xzalloc (ncols * sizeof (enum Align));
	for (size_t c = 0; c < ncols; c++)
		width[c] = 3;
	for (size_t i = 0; i < n; i++)
		for (size_t c = 0; c < lines[i].row.n && i != 1; c++) {
			size_t ts, te;
			cell_text (lines[i].s, &lines[i].row, c, &ts, &te);
			width[c] = MAX (width[c], align_width (lines[i].s + ts, te - ts));
		}
	for (size_t c = 0; c < hcols; c++) {
		size_t ts, te;
		cell_text (lines[1].s, &lines[1].row, c, &ts, &te);
		bool l = lines[1].s[ts] == ':', r = lines[1].s[te - 1] == ':';
		align[c] = l && r ? ALIGN_CENTER : l ? ALIGN_LEFT : r ? ALIGN_RIGHT : ALIGN_NONE;
	}

	const char *eol = buffer_eol (bp);
	bool outer = lines[0].row.lead || lines[0].row.trail;
	size_t indent = lines[0].row.first;
	StrBuf old, sb;
	sb_init (&old);
	sb_init (&sb);
	size_t new_pt = NOPOS;
	for (size_t i = 0; i < n; i++) {
		const TableLine *tl = &lines[i];
		if (i > 0) {
			sb_add (&old, eol);
			sb_add (&sb, eol);
		}
		sb_addn (&old, tl->s, tl->len);
		sb_addn (&sb, lines[0].s, indent);
		if (outer)
			sb_add (&sb, "| ");
		size_t cells = i < 2 ? hcols : MAX (hcols, tl->row.n);
		for (size_t c = 0; c < cells; c++) {
			if (c > 0)
				sb_add (&sb, " | ");
			size_t at, ts = 0, te = 0;
			if (c < tl->row.n)
				cell_text (tl->s, &tl->row, c, &ts, &te);
			if (i == 1) {
				at = sb.len;
				size_t w = width[c];
				enum Align a = align[c];
				sb_addc (&sb, a == ALIGN_LEFT || a == ALIGN_CENTER ? ':' : '-');
				for (size_t k = 2; k < w; k++)
					sb_addc (&sb, '-');
				sb_addc (&sb, a == ALIGN_RIGHT || a == ALIGN_CENTER ? ':' : '-');
				te = ts + w;
			} else
				add_padded (&sb, tl->s + ts, te - ts, width[c], c < hcols ? align[c] : ALIGN_NONE, &at);
			if (i == prow && c == pcell)
				new_pt = at + MIN (poff, te - ts);
		}
		if (outer)
			sb_add (&sb, " |");
		else
			while (sb.len > 0 && sb.s[sb.len - 1] == ' ')
				sb.len--;
	}
	if (new_pt == NOPOS || new_pt > sb.len)
		new_pt = MIN (pt - start, sb.len);

	bool same = old.len == sb.len && memcmp (old.s, sb.s, sb.len) == 0;
	if (same)
		minibuf_write ("The table is aligned already");
	else {
		replace_text (start, end, sb.s, sb.len);
		buffer_goto_offset (bp, start + new_pt);
	}
	for (size_t i = 0; i < n; i++)
		free (lines[i].s);
	free (lines);
	free (width);
	free (align);
	sb_free (&old);
	sb_free (&sb);
	return true;
}

void
markdown_mode_init (void)
{
	lisp_func_new ("markdown-mode", F_markdown_mode, true,
		       "Major mode for editing Markdown. Visiting a .md, .markdown or .mdown file\n"
		       "selects it.\n"
		       "The text stays Markdown source. Lines other than point's (and those of an\n"
		       "active region) are shown rendered: headings, emphasis, code, links,\n"
		       "quotes, lists, task boxes, rules and tables, with their markup hidden.\n"
		       "\n"
		       "C-c C-v (markdown-toggle-source) shows all of the source instead.\n"
		       "C-c C-a (markdown-table-align) aligns the table at point.\n"
		       "C-c C-s b, C-c C-s i and C-c C-s c make the region bold, italic or code.\n"
		       "C-c C-x C-x (markdown-toggle-task) checks or unchecks a task box.");
	lisp_func_new ("markdown-toggle-source", F_markdown_toggle_source, true,
		       "Show the Markdown buffer all as source, or again rendered but for\n"
		       "the lines point and the region are on.");
	lisp_func_new ("markdown-table-align", F_markdown_table_align, true,
		       "Align the pipe table at point: pad its cells to the width of their\n"
		       "columns, as their delimiter row says (:--, :-:, --:), and give every\n"
		       "row the header's number of cells. Wide characters count two columns.\n"
		       "One undo restores the table as it was.");
	lisp_func_new ("markdown-insert-bold", F_markdown_insert_bold, true,
		       "Make the region strong (**text**), or make it plain again if it is.\n"
		       "With no active region, insert **** with point in the middle.");
	lisp_func_new ("markdown-insert-italic", F_markdown_insert_italic, true,
		       "Make the region emphasized (*text*), or plain again if it is.\n"
		       "With no active region, insert ** with point in the middle.");
	lisp_func_new ("markdown-insert-code", F_markdown_insert_code, true,
		       "Make the region a code span (`text`), with more backticks than it\n"
		       "has inside; or plain again if it is one. With no active region,\n"
		       "insert `` with point in the middle.");
	lisp_func_new ("markdown-toggle-task", F_markdown_toggle_task, true,
		       "Check the task box of the list item at point ([ ] to [x]), or\n"
		       "uncheck it.");
}
