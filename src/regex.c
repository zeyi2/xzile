/* Regular expressions in Emacs syntax

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

/* A backtracking matcher. Patterns are compiled to a small program
   whose jumps are relative, so fragments can be concatenated freely.

   Supported syntax:
     .  [...]  [^...]  [:class:]  *  +  ?  *?  +?  ??  \{m,n\}
     \( \)  \(?: \)  \|  \1-\9  ^  $  \`  \'  \w  \W  \b  \B  \<  \>
     \s- \S- (whitespace)  \sw \Sw (word constituents)
   `^' and `$' are anchors only at the start and end of an alternative;
   elsewhere, like a leading `*', they stand for themselves. */

#include "xzile.h"

#include <ctype.h>

#define DUP_MAX 255

enum Op {
	OP_CHAR,		/* a: byte */
	OP_ANY,			/* any character but newline */
	OP_SET,			/* a: set index */
	OP_BOL,
	OP_EOL,
	OP_BOB,
	OP_EOB,
	OP_WORDB,
	OP_NWORDB,
	OP_WSTART,
	OP_WEND,
	OP_WORDCHAR,
	OP_NWORDCHAR,
	OP_SPACE,
	OP_NSPACE,
	OP_SPLIT,		/* a: preferred offset, b: alternative offset */
	OP_JMP,			/* a: offset */
	OP_SAVE,		/* a: register */
	OP_MARK,		/* a: register; record loop entry position */
	OP_CHECK,		/* a: register; fail if no progress since MARK */
	OP_BACKREF,		/* a: group */
	OP_MATCH,
};

typedef struct {
	enum Op op;
	int a, b;
} Inst;

typedef struct {
	uint32_t lo, hi;
} Range;

typedef struct {
	unsigned char bits[32];	/* Bytes below 0x80. */
	Range *ranges;		/* Non-ASCII code point ranges. */
	size_t nranges;
	bool negate;
} CharSet;

typedef struct {
	Inst *v;
	size_t n, cap;
	bool nullable;
} Frag;

struct Regex {
	bool plain, icase;
	char *literal;		/* Plain patterns. */
	size_t literal_len;
	Inst *prog;
	size_t nprog;
	CharSet *sets;
	size_t nsets;
	int ngroups;
	int nregs;
	bool has_backref;	/* Back-references and loops over patterns that */
	bool has_empty_loop;	/* can match empty need the backtracking matcher. */
};

typedef struct {
	const char *p;
	size_t len, pos;
	Regex *re;
	int ngroups_open;
	int nloops;
	const char *err;
} Parser;

/* Backtracking gives up after this many steps, as Emacs does. */
#define MAX_BACKTRACK_STEPS 20000000


/* Fragments. */

static void
frag_push (Frag *f, enum Op op, int a, int b)
{
	if (f->n == f->cap) {
		f->cap = f->cap ? f->cap * 2 : 8;
		f->v = xrealloc (f->v, f->cap * sizeof (Inst));
	}
	f->v[f->n].op = op;
	f->v[f->n].a = a;
	f->v[f->n].b = b;
	f->n++;
}

static void
frag_append (Frag *f, const Frag *g)
{
	for (size_t i = 0; i < g->n; i++)
		frag_push (f, g->v[i].op, g->v[i].a, g->v[i].b);
}

static Frag
frag_empty (void)
{
	Frag f = { NULL, 0, 0, true };
	return f;
}

static void
frag_free (Frag *f)
{
	free (f->v);
	f->v = NULL;
	f->n = f->cap = 0;
}

/* f1 \| f2 */
static Frag
frag_alt (Frag *f1, Frag *f2)
{
	Frag f = frag_empty ();
	frag_push (&f, OP_SPLIT, 1, (int) f1->n + 2);
	frag_append (&f, f1);
	frag_push (&f, OP_JMP, (int) f2->n + 1, 0);
	frag_append (&f, f2);
	f.nullable = f1->nullable || f2->nullable;
	frag_free (f1);
	frag_free (f2);
	return f;
}

static Frag
frag_star (Parser *ps, const Frag *body, bool greedy)
{
	Frag f = frag_empty ();
	int reg = -1;
	if (body->nullable)
		reg = ps->nloops++;
	int blen = (int) body->n + 1 + (reg >= 0 ? 2 : 0);
	if (greedy)
		frag_push (&f, OP_SPLIT, 1, blen + 1);
	else
		frag_push (&f, OP_SPLIT, blen + 1, 1);
	if (reg >= 0)
		frag_push (&f, OP_MARK, reg, 0);
	frag_append (&f, body);
	if (reg >= 0)
		frag_push (&f, OP_CHECK, reg, 0);
	frag_push (&f, OP_JMP, -blen, 0);
	f.nullable = true;
	return f;
}

static Frag
frag_opt (const Frag *body, bool greedy)
{
	Frag f = frag_empty ();
	if (greedy)
		frag_push (&f, OP_SPLIT, 1, (int) body->n + 1);
	else
		frag_push (&f, OP_SPLIT, (int) body->n + 1, 1);
	frag_append (&f, body);
	f.nullable = true;
	return f;
}

/* body{min,max}; max < 0 means unbounded. */
static Frag
frag_repeat (Parser *ps, Frag *body, int min, int max, bool greedy)
{
	Frag f = frag_empty ();
	for (int i = 0; i < min; i++)
		frag_append (&f, body);
	f.nullable = min == 0 || body->nullable;

	if (max < 0) {
		Frag s = frag_star (ps, body, greedy);
		frag_append (&f, &s);
		frag_free (&s);
	} else if (max > min) {
		/* Nest the optional copies: (b(b(b)?)?)? */
		Frag tail = frag_empty ();
		for (int i = min; i < max; i++) {
			Frag inner = frag_empty ();
			frag_append (&inner, body);
			frag_append (&inner, &tail);
			inner.nullable = body->nullable;
			frag_free (&tail);
			tail = frag_opt (&inner, greedy);
			frag_free (&inner);
		}
		frag_append (&f, &tail);
		frag_free (&tail);
	}
	frag_free (body);
	return f;
}


/* Character sets. */

static CharSet *
new_set (Parser *ps, int *index)
{
	Regex *re = ps->re;
	re->sets = xrealloc (re->sets, (re->nsets + 1) * sizeof (CharSet));
	CharSet *cs = &re->sets[re->nsets];
	memset (cs, 0, sizeof *cs);
	*index = (int) re->nsets++;
	return cs;
}

static void
set_add_byte (CharSet *cs, unsigned c)
{
	cs->bits[c >> 3] |= (unsigned char) (1u << (c & 7));
}

static bool
set_has_byte (const CharSet *cs, unsigned c)
{
	return (cs->bits[c >> 3] >> (c & 7)) & 1;
}

static void
set_add_range (CharSet *cs, uint32_t lo, uint32_t hi)
{
	for (uint32_t c = lo; c <= hi && c < 0x80; c++)
		set_add_byte (cs, c);
	if (hi >= 0x80) {
		cs->ranges = xrealloc (cs->ranges, (cs->nranges + 1) * sizeof (Range));
		cs->ranges[cs->nranges].lo = MAX (lo, 0x80);
		cs->ranges[cs->nranges].hi = hi;
		cs->nranges++;
	}
}

static bool
set_has_cp (const CharSet *cs, uint32_t cp)
{
	for (size_t i = 0; i < cs->nranges; i++)
		if (cp >= cs->ranges[i].lo && cp <= cs->ranges[i].hi)
			return true;
	return false;
}

static const struct {
	const char *name;
	int (*fn) (int);
} char_classes[] = {
	{ "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank },
	{ "cntrl", iscntrl }, { "digit", isdigit }, { "graph", isgraph },
	{ "lower", islower }, { "print", isprint }, { "punct", ispunct },
	{ "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
};

/* Read one (possibly multibyte) character of the pattern. */
static uint32_t
read_pattern_char (Parser *ps)
{
	size_t char_len;
	uint32_t cp = utf8_decode (ps->p, ps->len, ps->pos, &char_len);
	if (cp == UTF8_REPLACEMENT && char_len == 1)
		cp = (unsigned char) ps->p[ps->pos];
	ps->pos += char_len;
	return cp;
}

static Frag
parse_bracket (Parser *ps)
{
	Frag f = frag_empty ();
	int index;
	CharSet *cs = new_set (ps, &index);

	if (ps->pos < ps->len && ps->p[ps->pos] == '^') {
		cs->negate = true;
		ps->pos++;
	}

	bool first = true;
	for (;;) {
		if (ps->pos >= ps->len) {
			ps->err = "Unmatched [ or [^";
			return f;
		}
		char c = ps->p[ps->pos];
		if (c == ']' && !first) {
			ps->pos++;
			break;
		}
		first = false;

		if (c == '[' && ps->pos + 1 < ps->len && ps->p[ps->pos + 1] == ':') {
			const char *end = xmemmem (ps->p + ps->pos + 2, ps->len - ps->pos - 2, ":]", 2);
			if (end != NULL) {
				size_t nlen = (size_t) (end - (ps->p + ps->pos + 2));
				size_t k;
				for (k = 0; k < sizeof char_classes / sizeof char_classes[0]; k++)
					if (strlen (char_classes[k].name) == nlen &&
					    strncmp (char_classes[k].name, ps->p + ps->pos + 2, nlen) == 0)
						break;
				if (k == sizeof char_classes / sizeof char_classes[0]) {
					ps->err = "Invalid character class name";
					return f;
				}
				for (unsigned b = 0; b < 0x80; b++)
					if (char_classes[k].fn ((int) b))
						set_add_byte (cs, b);
				ps->pos = (size_t) (end - ps->p) + 2;
				continue;
			}
		}

		uint32_t lo = read_pattern_char (ps), hi = lo;
		if (ps->pos + 1 < ps->len && ps->p[ps->pos] == '-' && ps->p[ps->pos + 1] != ']') {
			ps->pos++;
			hi = read_pattern_char (ps);
			if (hi < lo) {
				ps->err = "Invalid range end";
				return f;
			}
		}
		set_add_range (cs, lo, hi);
	}

	if (ps->re->icase)
		for (unsigned b = 0; b < 0x80; b++)
			if (set_has_byte (cs, b)) {
				set_add_byte (cs, (unsigned) tolower ((int) b));
				set_add_byte (cs, (unsigned) toupper ((int) b));
			}

	frag_push (&f, OP_SET, index, 0);
	f.nullable = false;
	return f;
}


/* The parser. */

static Frag parse_alt (Parser *ps, bool top);

static bool
at (Parser *ps, const char *s)
{
	size_t n = strlen (s);
	return ps->pos + n <= ps->len && memcmp (ps->p + ps->pos, s, n) == 0;
}

/* Is the parser at the end of an alternative? */
static bool
at_branch_end (Parser *ps)
{
	return ps->pos >= ps->len || at (ps, "\\|") || at (ps, "\\)");
}

static Frag
single (enum Op op, int a, bool nullable)
{
	Frag f = frag_empty ();
	frag_push (&f, op, a, 0);
	f.nullable = nullable;
	return f;
}

static int
parse_int (Parser *ps)
{
	int n = -1;
	while (ps->pos < ps->len && isdigit ((unsigned char) ps->p[ps->pos])) {
		n = (n < 0 ? 0 : n) * 10 + (ps->p[ps->pos++] - '0');
		if (n > DUP_MAX * 10)
			n = DUP_MAX * 10;
	}
	return n;
}

/* Parse an atom. Returns false if there is none at this point. */
static bool
parse_atom (Parser *ps, bool branch_start, Frag *out)
{
	char c = ps->p[ps->pos];

	if (c == '^' && branch_start) {
		ps->pos++;
		*out = single (OP_BOL, 0, true);
		return true;
	}
	if (c == '$') {
		ps->pos++;
		*out = at_branch_end (ps) ? single (OP_EOL, 0, true) : single (OP_CHAR, '$', false);
		return true;
	}
	if (c == '.') {
		ps->pos++;
		*out = single (OP_ANY, 0, false);
		return true;
	}
	if (c == '[') {
		ps->pos++;
		*out = parse_bracket (ps);
		return true;
	}

	if (c == '\\') {
		if (ps->pos + 1 >= ps->len) {
			ps->err = "Trailing backslash";
			return false;
		}
		char e = ps->p[ps->pos + 1];
		ps->pos += 2;
		switch (e) {
		case '(': {
			bool shy = at (ps, "?:");
			int group = 0;
			if (shy)
				ps->pos += 2;
			else
				group = ++ps->ngroups_open;
			Frag inner = parse_alt (ps, false);
			if (ps->err != NULL) {
				frag_free (&inner);
				return false;
			}
			if (!at (ps, "\\)")) {
				frag_free (&inner);
				ps->err = "Unmatched ( or \\(";
				return false;
			}
			ps->pos += 2;
			if (shy) {
				*out = inner;
				return true;
			}
			Frag f = frag_empty ();
			frag_push (&f, OP_SAVE, 2 * group, 0);
			frag_append (&f, &inner);
			frag_push (&f, OP_SAVE, 2 * group + 1, 0);
			f.nullable = inner.nullable;
			frag_free (&inner);
			*out = f;
			return true;
		}
		case ')':
			ps->err = "Unmatched ) or \\)";
			return false;
		case '`':
			*out = single (OP_BOB, 0, true);
			return true;
		case '\'':
			*out = single (OP_EOB, 0, true);
			return true;
		case 'w':
			*out = single (OP_WORDCHAR, 0, false);
			return true;
		case 'W':
			*out = single (OP_NWORDCHAR, 0, false);
			return true;
		case 'b':
			*out = single (OP_WORDB, 0, true);
			return true;
		case 'B':
			*out = single (OP_NWORDB, 0, true);
			return true;
		case '<':
			*out = single (OP_WSTART, 0, true);
			return true;
		case '>':
			*out = single (OP_WEND, 0, true);
			return true;
		case 's':
		case 'S':
			if (ps->pos < ps->len && (ps->p[ps->pos] == '-' || ps->p[ps->pos] == ' ')) {
				ps->pos++;
				*out = single (e == 's' ? OP_SPACE : OP_NSPACE, 0, false);
				return true;
			}
			if (ps->pos < ps->len && ps->p[ps->pos] == 'w') {
				ps->pos++;
				*out = single (e == 's' ? OP_WORDCHAR : OP_NWORDCHAR, 0, false);
				return true;
			}
			ps->err = "Invalid syntax designator";
			return false;
		default:
			if (e >= '1' && e <= '9') {
				int n = e - '0';
				if (n > ps->ngroups_open) {
					ps->err = "Invalid back reference";
					return false;
				}
				ps->re->has_backref = true;
				*out = single (OP_BACKREF, n, true);
				return true;
			}
			*out = single (OP_CHAR, ps->re->icase ? tolower ((unsigned char) e) : (unsigned char) e, false);
			return true;
		}
	}

	/* A literal character, which may be multibyte. */
	size_t char_len;
	utf8_decode (ps->p, ps->len, ps->pos, &char_len);
	Frag f = frag_empty ();
	for (size_t k = 0; k < char_len; k++) {
		unsigned char b = (unsigned char) ps->p[ps->pos + k];
		frag_push (&f, OP_CHAR, ps->re->icase ? tolower (b) : b, 0);
	}
	f.nullable = false;
	ps->pos += char_len;
	*out = f;
	return true;
}

static Frag
parse_branch (Parser *ps)
{
	Frag f = frag_empty ();
	bool branch_start = true;

	while (!at_branch_end (ps) && ps->err == NULL) {
		char c = ps->p[ps->pos];

		/* A repetition operator with nothing to repeat is literal. */
		Frag atom;
		bool is_anchor = false;
		if (branch_start && (c == '*' || c == '+' || c == '?')) {
			ps->pos++;
			atom = single (OP_CHAR, (unsigned char) c, false);
		} else {
			is_anchor = c == '^' && branch_start;
			if (!parse_atom (ps, branch_start, &atom))
				break;
		}

		/* Quantifiers. */
		for (;;) {
			if (ps->err != NULL || ps->pos >= ps->len)
				break;
			char q = ps->p[ps->pos];
			int min, max;
			if (is_anchor)
				break;
			if (q == '*' || q == '+' || q == '?') {
				ps->pos++;
				min = q == '+' ? 1 : 0;
				max = q == '?' ? 1 : -1;
			} else if (at (ps, "\\{")) {
				ps->pos += 2;
				min = parse_int (ps);
				max = min;
				if (ps->pos < ps->len && ps->p[ps->pos] == ',') {
					ps->pos++;
					max = parse_int (ps);
				}
				if (min < 0)
					min = 0;
				if (!at (ps, "\\}") || min > DUP_MAX || max > DUP_MAX || (max >= 0 && max < min)) {
					ps->err = "Invalid content of \\{\\}";
					break;
				}
				ps->pos += 2;
			} else
				break;

			bool greedy = true;
			if (ps->pos < ps->len && ps->p[ps->pos] == '?' && q != '\\') {
				greedy = false;
				ps->pos++;
			}
			atom = frag_repeat (ps, &atom, min, max, greedy);
		}

		bool nullable = f.nullable && atom.nullable;
		frag_append (&f, &atom);
		frag_free (&atom);
		f.nullable = nullable;
		/* `*' after a leading `^' is literal, as in Emacs. */
		branch_start = is_anchor;
	}
	return f;
}

static Frag
parse_alt (Parser *ps, bool top)
{
	Frag f = parse_branch (ps);
	while (ps->err == NULL && at (ps, "\\|")) {
		ps->pos += 2;
		Frag g = parse_branch (ps);
		f = frag_alt (&f, &g);
	}
	if (top && ps->err == NULL && at (ps, "\\)"))
		ps->err = "Unmatched ) or \\)";
	return f;
}

void
regex_free (Regex *re)
{
	if (re == NULL)
		return;
	free (re->literal);
	free (re->prog);
	for (size_t i = 0; i < re->nsets; i++)
		free (re->sets[i].ranges);
	free (re->sets);
	free (re);
}

Regex *
regex_compile (const char *pattern, size_t len, bool plain, bool icase, const char **err)
{
	Regex *re = xzalloc (sizeof (Regex));
	re->plain = plain;
	re->icase = icase;
	*err = NULL;

	if (plain) {
		re->literal = xstrndup (pattern, len);
		re->literal_len = len;
		if (icase)
			for (size_t i = 0; i < len; i++)
				re->literal[i] = (char) tolower ((unsigned char) re->literal[i]);
		return re;
	}

	Parser ps = { pattern, len, 0, re, 0, 0, NULL };
	Frag f = parse_alt (&ps, true);
	if (ps.err != NULL) {
		frag_free (&f);
		regex_free (re);
		*err = ps.err;
		return NULL;
	}
	frag_push (&f, OP_MATCH, 0, 0);
	re->prog = f.v;
	re->nprog = f.n;
	re->ngroups = ps.ngroups_open;
	re->nregs = 2 * (ps.ngroups_open + 1) + ps.nloops;
	re->has_empty_loop = ps.nloops > 0;
	return re;
}


/* Matching. */

typedef struct {
	const char *text;
	size_t len;
	int before, after;	/* Bytes adjoining the text, or -1. */
} Subject;

static int
byte_at (const Subject *s, long i)
{
	if (i < 0)
		return s->before;
	if ((size_t) i >= s->len)
		return s->after;
	return (unsigned char) s->text[i];
}

static bool
is_word_byte (int c)
{
	return c >= 0 && (isalnum (c) || c == '_' || c >= 0x80);
}

typedef struct {
	int pc;
	size_t sp;
	int reg;		/* >= 0 for a register restore entry */
	size_t val;
} BtEntry;

typedef struct {
	BtEntry *v;
	size_t n, cap;
} BtStack;

static void
bt_push (BtStack *st, int pc, size_t sp, int reg, size_t val)
{
	if (st->n == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 64;
		st->v = xrealloc (st->v, st->cap * sizeof (BtEntry));
	}
	BtEntry *e = &st->v[st->n++];
	e->pc = pc;
	e->sp = sp;
	e->reg = reg;
	e->val = val;
}

/* Length of the character at `sp' (at least 1). */
static size_t
char_len_at (const Subject *s, size_t sp, uint32_t *cp)
{
	size_t char_len;
	*cp = utf8_decode (s->text, s->len, sp, &char_len);
	if (char_len == 0)
		char_len = 1;
	return char_len;
}

static bool
match_here (const Regex *re, const Subject *s, size_t start, size_t *end, size_t *regs, BtStack *st,
	    size_t *steps)
{
	for (int i = 0; i < re->nregs; i++)
		regs[i] = NOPOS;
	st->n = 0;
	bt_push (st, 0, start, -1, 0);

	while (st->n > 0) {
		BtEntry e = st->v[--st->n];
		if (e.reg >= 0) {
			regs[e.reg] = e.val;
			continue;
		}
		int pc = e.pc;
		size_t sp = e.sp;

		for (;;) {
			if (++*steps > MAX_BACKTRACK_STEPS)
				return false;
			const Inst *in = &re->prog[pc];
			int c = sp < s->len ? (unsigned char) s->text[sp] : -1;
			bool ok = true;

			switch (in->op) {
			case OP_CHAR:
				ok = c >= 0 && (re->icase ? tolower (c) : c) == in->a;
				if (ok)
					sp++;
				break;
			case OP_ANY:
				ok = c >= 0 && c != '\n';
				if (ok) {
					uint32_t cp;
					sp += char_len_at (s, sp, &cp);
				}
				break;
			case OP_SET: {
				const CharSet *cs = &re->sets[in->a];
				if (c < 0) {
					ok = false;
					break;
				}
				uint32_t cp;
				size_t n = 1;
				bool in_set;
				if (c < 0x80)
					in_set = set_has_byte (cs, (unsigned) c);
				else {
					n = char_len_at (s, sp, &cp);
					in_set = set_has_cp (cs, cp == UTF8_REPLACEMENT && n == 1 ? (uint32_t) c : cp);
				}
				ok = in_set != cs->negate;
				if (ok)
					sp += n;
				break;
			}
			case OP_BOL: {
				int p = byte_at (s, (long) sp - 1);
				ok = p < 0 || p == '\n';
				break;
			}
			case OP_EOL: {
				int n = byte_at (s, (long) sp);
				ok = n < 0 || n == '\n';
				break;
			}
			case OP_BOB:
				ok = sp == 0 && s->before < 0;
				break;
			case OP_EOB:
				ok = sp == s->len && s->after < 0;
				break;
			case OP_WORDB:
			case OP_NWORDB:
			case OP_WSTART:
			case OP_WEND: {
				bool pw = is_word_byte (byte_at (s, (long) sp - 1));
				bool nw = is_word_byte (byte_at (s, (long) sp));
				if (in->op == OP_WORDB)
					ok = pw != nw;
				else if (in->op == OP_NWORDB)
					ok = pw == nw;
				else if (in->op == OP_WSTART)
					ok = !pw && nw;
				else
					ok = pw && !nw;
				break;
			}
			case OP_WORDCHAR:
			case OP_NWORDCHAR:
				ok = c >= 0 && is_word_byte (c) == (in->op == OP_WORDCHAR);
				if (ok) {
					uint32_t cp;
					sp += c >= 0x80 ? char_len_at (s, sp, &cp) : 1;
				}
				break;
			case OP_SPACE:
			case OP_NSPACE:
				ok = c >= 0 && (isspace (c) != 0) == (in->op == OP_SPACE);
				if (ok)
					sp++;
				break;
			case OP_SPLIT:
				bt_push (st, pc + in->b, sp, -1, 0);
				pc += in->a;
				continue;
			case OP_JMP:
				pc += in->a;
				continue;
			case OP_SAVE:
			case OP_MARK:
				bt_push (st, 0, 0, in->op == OP_SAVE ? in->a : 2 * (re->ngroups + 1) + in->a,
					 regs[in->op == OP_SAVE ? in->a : 2 * (re->ngroups + 1) + in->a]);
				regs[in->op == OP_SAVE ? in->a : 2 * (re->ngroups + 1) + in->a] = sp;
				break;
			case OP_CHECK:
				ok = regs[2 * (re->ngroups + 1) + in->a] != sp;
				break;
			case OP_BACKREF: {
				size_t gs = regs[2 * in->a], ge = regs[2 * in->a + 1];
				if (gs == NOPOS || ge == NOPOS || ge < gs) {
					ok = false;
					break;
				}
				size_t n = ge - gs;
				ok = sp + n <= s->len;
				for (size_t k = 0; ok && k < n; k++) {
					int a = (unsigned char) s->text[gs + k], b = (unsigned char) s->text[sp + k];
					ok = re->icase ? tolower (a) == tolower (b) : a == b;
				}
				if (ok)
					sp += n;
				break;
			}
			case OP_MATCH:
				*end = sp;
				return true;
			}

			if (!ok)
				break;
			pc++;
		}
	}
	return false;
}

/* Pike VM: simulate all threads in lockstep, in priority order, which
   gives the same (leftmost-first) results as backtracking in linear time
   per start position. It cannot handle back-references. Threads that
   consume a multibyte character wait out its remaining bytes. */

typedef struct {
	int pc;
	size_t start;
	size_t wait;		/* Bytes still to skip before reaching `pc'. */
} Thread;

typedef struct {
	Thread *t;
	size_t n;
} ThreadList;

/* A character is at most 4 bytes, so a thread waits at most 3. */
#define MAX_WAIT 3

typedef struct {
	const Regex *re;
	const Subject *s;
	unsigned *marks;	/* marks[pc] == stamp if pc is in the list being built. */
	unsigned *wait_marks;	/* Likewise for waiting threads, by pc and wait. */
	unsigned stamp;
} PikeState;

static void
push_waiting (PikeState *ps, ThreadList *l, int pc, size_t start, size_t wait)
{
	unsigned *m = &ps->wait_marks[(size_t) pc * MAX_WAIT + wait - 1];
	if (*m == ps->stamp)
		return;
	*m = ps->stamp;
	l->t[l->n].pc = pc;
	l->t[l->n].start = start;
	l->t[l->n].wait = wait;
	l->n++;
}

static bool
check_assertion (const Subject *s, enum Op op, size_t sp)
{
	int p = byte_at (s, (long) sp - 1), n = byte_at (s, (long) sp);
	switch (op) {
	case OP_BOL:
		return p < 0 || p == '\n';
	case OP_EOL:
		return n < 0 || n == '\n';
	case OP_BOB:
		return sp == 0 && s->before < 0;
	case OP_EOB:
		return sp == s->len && s->after < 0;
	case OP_WORDB:
		return is_word_byte (p) != is_word_byte (n);
	case OP_NWORDB:
		return is_word_byte (p) == is_word_byte (n);
	case OP_WSTART:
		return !is_word_byte (p) && is_word_byte (n);
	case OP_WEND:
		return is_word_byte (p) && !is_word_byte (n);
	default:
		return true;
	}
}

static void
add_thread (PikeState *ps, ThreadList *l, int pc, size_t start, size_t sp)
{
	if (ps->marks[pc] == ps->stamp)
		return;
	ps->marks[pc] = ps->stamp;

	const Inst *in = &ps->re->prog[pc];
	switch (in->op) {
	case OP_JMP:
		add_thread (ps, l, pc + in->a, start, sp);
		return;
	case OP_SPLIT:
		add_thread (ps, l, pc + in->a, start, sp);
		add_thread (ps, l, pc + in->b, start, sp);
		return;
	case OP_SAVE:
	case OP_MARK:
	case OP_CHECK:
		add_thread (ps, l, pc + 1, start, sp);
		return;
	case OP_BOL:
	case OP_EOL:
	case OP_BOB:
	case OP_EOB:
	case OP_WORDB:
	case OP_NWORDB:
	case OP_WSTART:
	case OP_WEND:
		if (check_assertion (ps->s, in->op, sp))
			add_thread (ps, l, pc + 1, start, sp);
		return;
	default:
		l->t[l->n].pc = pc;
		l->t[l->n].start = start;
		l->t[l->n].wait = 0;
		l->n++;
		return;
	}
}

/* Return the number of bytes instruction `in' consumes at `sp', or 0
   if it does not match. */
static size_t
step_inst (const Regex *re, const Subject *s, const Inst *in, size_t sp)
{
	if (sp >= s->len)
		return 0;
	int c = (unsigned char) s->text[sp];
	uint32_t cp;
	switch (in->op) {
	case OP_CHAR:
		return (re->icase ? tolower (c) : c) == in->a ? 1 : 0;
	case OP_ANY:
		return c != '\n' ? char_len_at (s, sp, &cp) : 0;
	case OP_SET: {
		const CharSet *cs = &re->sets[in->a];
		size_t n = 1;
		bool in_set;
		if (c < 0x80)
			in_set = set_has_byte (cs, (unsigned) c);
		else {
			n = char_len_at (s, sp, &cp);
			in_set = set_has_cp (cs, cp == UTF8_REPLACEMENT && n == 1 ? (uint32_t) c : cp);
		}
		return in_set != cs->negate ? n : 0;
	}
	case OP_WORDCHAR:
	case OP_NWORDCHAR:
		if (is_word_byte (c) != (in->op == OP_WORDCHAR))
			return 0;
		return c >= 0x80 ? char_len_at (s, sp, &cp) : 1;
	case OP_SPACE:
	case OP_NSPACE:
		return (isspace (c) != 0) == (in->op == OP_SPACE) ? 1 : 0;
	default:
		return 0;
	}
}

/* Run from `from'; if `anchored', only matches starting there count.
   Returns the match start or -1. */
static long
pike_run (const Regex *re, const Subject *s, size_t from, bool anchored, size_t *end)
{
	/* Each list holds each pc at most once active and once per wait. */
	size_t cap = re->nprog * (MAX_WAIT + 1) + 1;
	ThreadList clist = { xmalloc (cap * sizeof (Thread)), 0 };
	ThreadList nlist = { xmalloc (cap * sizeof (Thread)), 0 };
	PikeState ps = { re, s, xzalloc (re->nprog * sizeof (unsigned)),
			 xzalloc (re->nprog * MAX_WAIT * sizeof (unsigned)), 1 };
	bool matched = false;
	long best_start = -1;

	add_thread (&ps, &clist, 0, from, from);
	for (size_t sp = from;; sp++) {
		if (clist.n == 0) {
			if (matched || anchored || sp > s->len)
				break;
		}

		ps.stamp++;
		nlist.n = 0;
		for (size_t i = 0; i < clist.n; i++) {
			Thread *t = &clist.t[i];
			if (t->wait > 0) {
				/* Still inside a multibyte character. */
				if (t->wait == 1)
					add_thread (&ps, &nlist, t->pc, t->start, sp + 1);
				else
					push_waiting (&ps, &nlist, t->pc, t->start, t->wait - 1);
				continue;
			}
			const Inst *in = &re->prog[t->pc];
			if (in->op == OP_MATCH) {
				matched = true;
				best_start = (long) t->start;
				*end = sp;
				break;	/* Lower-priority threads are cut off. */
			}
			size_t n = step_inst (re, s, in, sp);
			if (n == 1)
				add_thread (&ps, &nlist, t->pc + 1, t->start, sp + 1);
			else if (n > 1)
				push_waiting (&ps, &nlist, t->pc + 1, t->start, n - 1);
		}

		if (sp >= s->len)
			break;
		/* Start a new, lowest-priority, thread at the next position. */
		if (!matched && !anchored)
			add_thread (&ps, &nlist, 0, sp + 1, sp + 1);

		ThreadList tmp = clist;
		clist = nlist;
		nlist = tmp;
	}

	free (clist.t);
	free (nlist.t);
	free (ps.marks);
	free (ps.wait_marks);
	return best_start;
}

static bool
literal_at (const Regex *re, const char *text, size_t pos)
{
	if (!re->icase)
		return memcmp (text + pos, re->literal, re->literal_len) == 0;
	for (size_t k = 0; k < re->literal_len; k++)
		if (tolower ((unsigned char) text[pos + k]) != (unsigned char) re->literal[k])
			return false;
	return true;
}

long
regex_search (const Regex *re, const char *text, size_t len, bool forward,
	      int before, int after, size_t *end)
{
	if (re->plain) {
		size_t n = re->literal_len;
		if (n > len)
			return -1;
		if (forward) {
			for (size_t pos = 0; pos + n <= len; pos++)
				if (literal_at (re, text, pos)) {
					*end = pos + n;
					return (long) pos;
				}
		} else {
			for (size_t pos = len - n + 1; pos-- > 0;)
				if (literal_at (re, text, pos)) {
					*end = pos + n;
					return (long) pos;
				}
		}
		return -1;
	}

	Subject s = { text, len, before, after };
	size_t *regs = NULL;
	BtStack st = { NULL, 0, 0 };
	long ret = -1;

	/* A quick filter for patterns that start with a literal byte. */
	int first = re->prog[0].op == OP_CHAR && !re->icase ? re->prog[0].a : -1;

	if (!re->has_backref && !re->has_empty_loop) {
		if (forward)
			ret = pike_run (re, &s, 0, false, end);
		else
			for (size_t pos = len + 1; pos-- > 0;)
				if ((first < 0 || (pos < len && (unsigned char) text[pos] == first)) &&
				    (ret = pike_run (re, &s, pos, true, end)) >= 0)
					break;
		return ret;
	}

	regs = xmalloc ((size_t) MAX (re->nregs, 1) * sizeof (size_t));
	size_t steps = 0;
	if (forward) {
		for (size_t pos = 0; pos <= len; pos++) {
			if (first >= 0) {
				const char *p = memchr (text + pos, first, len - pos);
				if (p == NULL)
					break;
				pos = (size_t) (p - text);
			}
			if (match_here (re, &s, pos, end, regs, &st, &steps)) {
				ret = (long) pos;
				break;
			}
		}
	} else {
		for (size_t pos = len + 1; pos-- > 0;)
			if ((first < 0 || (pos < len && (unsigned char) text[pos] == first)) &&
			    match_here (re, &s, pos, end, regs, &st, &steps)) {
				ret = (long) pos;
				break;
			}
	}

	free (st.v);
	free (regs);
	if (steps > MAX_BACKTRACK_STEPS) {
		*end = 0;
		return REGEX_TOO_COMPLEX;
	}
	return ret;
}
