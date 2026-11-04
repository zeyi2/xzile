/* "Lisp" "interpreter".

   Copyright (c) 2001-2020 Free Software Foundation, Inc.

   This file is part of GNU Zile.

   GNU Zile is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GNU Zile is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.  */

#include "xzile.h"

#include <ctype.h>

/* Argument queues. */

Args *
args_new (void)
{
	return xzalloc (sizeof (Args));
}

void
args_add (Args *a, const char *s)
{
	a->v = xrealloc (a->v, (a->n + 1) * sizeof (char *));
	a->v[a->n++] = s != NULL ? xstrdup (s) : NULL;
}

void
args_free (Args *a)
{
	if (a == NULL)
		return;
	for (size_t i = 0; i < a->n; i++)
		free (a->v[i]);
	free (a->v);
	free (a);
}

/* Remove and return the next argument; NULL if none (or if it is NULL).
   The string remains owned by `a'. */
char *
args_poll (Args *a)
{
	if (a == NULL || a->pos >= a->n)
		return NULL;
	return a->v[a->pos++];
}

size_t
args_size (const Args *a)
{
	return a != NULL ? a->n - a->pos : 0;
}

bool
args_empty (const Args *a)
{
	return args_size (a) == 0;
}


/* Lisp functions. */

Map *lisp_funcs;

void
lisp_func_new (const char *name, Function func, bool interactive, const char *doc)
{
	if (lisp_funcs == NULL)
		lisp_funcs = map_new ();
	LispFunc *f = xmalloc (sizeof (LispFunc));
	f->name = name;
	f->func = func;
	f->interactive = interactive;
	f->doc = doc;
	free (map_get (lisp_funcs, name));
	map_set (lisp_funcs, name, f);
}

LispFunc *
lisp_func_find (const char *name)
{
	return name != NULL ? map_get (lisp_funcs, name) : NULL;
}


/* S-expressions. A Lexp is either an atom or a list, and may be quoted.
   N.B. This is not the same as in Lisp (car and cdr). */

typedef struct Lexp Lexp;

typedef struct {
	Lexp **v;
	size_t n;
} LexpList;

struct Lexp {
	bool quoted;
	char *atom;
	LexpList *list;
};

static void lexp_list_free (LexpList *l);

static void
lexp_free (Lexp *l)
{
	free (l->atom);
	lexp_list_free (l->list);
	free (l);
}

static void
lexp_list_free (LexpList *l)
{
	if (l == NULL)
		return;
	for (size_t i = 0; i < l->n; i++)
		lexp_free (l->v[i]);
	free (l->v);
	free (l);
}

static void
lexp_list_add (LexpList *l, Lexp *e)
{
	l->v = xrealloc (l->v, (l->n + 1) * sizeof (Lexp *));
	l->v[l->n++] = e;
}

static bool
lexp_eval (Lexp *l)
{
	if (l->atom != NULL || l->list == NULL) /* Don't evaluate atoms. */
		return false;

	Args *args = args_new ();
	for (size_t i = 0; i < l->list->n; i++)
		args_add (args, l->list->v[i]->atom);

	bool ok = false;
	LispFunc *func = lisp_func_find (args_poll (args));
	if (func != NULL)
		ok = call_command (func, 1, args);
	args_free (args);
	return ok;
}


/* Sexp parser. */

enum TokenType {
	TOK_CLOSEPAREN,
	TOK_OPENPAREN,
	TOK_QUOTE,
	TOK_WORD,
};

enum ParseStatus {
	PARSE_OK,
	PARSE_EOF,
	PARSE_UNCLOSED_QUOTE,
};

typedef struct {
	const char *a;
	size_t len, pos;
} Reader;

static bool
read_char (Reader *r, char *c)
{
	if (r->pos >= r->len)
		return false;
	*c = r->a[r->pos++];
	return true;
}

static enum ParseStatus
read_token (Reader *r, enum TokenType *type, char **tok)
{
	char c = 0;
	*tok = NULL;

	/* Chew space to next token */
	do {
		if (!read_char (r, &c))
			return PARSE_EOF;

		/* Munch comments */
		if (c == ';')
			do
				if (!read_char (r, &c))
					return PARSE_EOF;
			while (c != '\n');
	} while (isspace ((unsigned char) c));

	/* Snag token */
	if (c == '(') {
		*type = TOK_OPENPAREN;
		return PARSE_OK;
	} else if (c == ')') {
		*type = TOK_CLOSEPAREN;
		return PARSE_OK;
	} else if (c == '\'') {
		*type = TOK_QUOTE;
		return PARSE_OK;
	}

	/* It looks like a symbol or string. */
	bool doublequotes = false;
	if (c == '\"') {
		doublequotes = true;
		if (!read_char (r, &c))
			return PARSE_EOF;
	}

	StrBuf sb;
	sb_init (&sb);
	for (;;) {
		if (!doublequotes) {
			if (c == ')' || c == '(' || c == ';' || isspace ((unsigned char) c)) {
				r->pos--;
				break;
			}
		} else {
			if (c == '\n' || c == '\r') {
				sb_free (&sb);
				return PARSE_UNCLOSED_QUOTE;
			}
			if (c == '\"')
				break;
		}
		sb_addc (&sb, c);
		if (!read_char (r, &c)) {
			sb_free (&sb);
			return PARSE_EOF;
		}
	}

	*type = TOK_WORD;
	*tok = sb_take (&sb);
	return PARSE_OK;
}

static enum ParseStatus read_list (Reader *r, LexpList **out);

static enum ParseStatus
read_sexp (Reader *r, Lexp **out, enum TokenType *type)
{
	char *tok;
	enum ParseStatus st = read_token (r, type, &tok);
	if (st != PARSE_OK)
		return st;

	Lexp *sexp = xzalloc (sizeof (Lexp));
	if (*type == TOK_QUOTE) {
		sexp->quoted = true;
		st = read_token (r, type, &tok);
		if (st != PARSE_OK) {
			lexp_free (sexp);
			return st;
		}
	}

	switch (*type) {
	case TOK_OPENPAREN:
		read_list (r, &sexp->list);
		break;
	case TOK_WORD:
		sexp->atom = tok;
		break;
	default:
		break;
	}

	*out = sexp;
	return PARSE_OK;
}

/* Read a list up to a closing parenthesis or EOF. Returns the list,
   or NULL on a parse error other than EOF. */
static enum ParseStatus
read_list (Reader *r, LexpList **out)
{
	LexpList *list = xzalloc (sizeof (LexpList));
	enum ParseStatus st;
	for (;;) {
		enum TokenType type;
		Lexp *sexp;
		st = read_sexp (r, &sexp, &type);
		if (st != PARSE_OK)
			break;
		if (type == TOK_CLOSEPAREN) {
			lexp_free (sexp);
			break;
		}
		lexp_list_add (list, sexp);
	}
	if (st == PARSE_UNCLOSED_QUOTE) {
		lexp_list_free (list);
		list = NULL;
	}
	*out = list;
	return st;
}


/* Calling Lisp functions. */

bool
funcall_args (const char *name, long uniarg, Args *args)
{
	LispFunc *f = lisp_func_find (name);
	return f != NULL ? f->func (uniarg, args) : false;
}

/* Call `name' with no arguments. */
bool
funcall (const char *name)
{
	Args args = { NULL, 0, 0 };
	return funcall_args (name, 1, &args);
}

/* Call `name' as if interactively with universal argument `uniarg'. */
bool
funcall_n (const char *name, long uniarg)
{
	return funcall_args (name, uniarg, NULL);
}

bool
noarg (Args *args)
{
	return !(lastflag & FLAG_SET_UNIARG) && (args != NULL && args_size (args) == 0);
}

bool
bool_arg (Args *args, bool *val)
{
	*val = false;
	if (args_size (args) > 0) {
		const char *s = args_poll (args);
		*val = !(s != NULL && strcmp (s, "nil") == 0);
		return true;
	}
	return false;
}

bool
parse_number (const char *s, long *out)
{
	return parse_long (s, out);
}

bool
int_arg (Args *args, long *n)
{
	*n = 0;
	if (args_size (args) == 0)
		return false;
	return parse_long (args_poll (args), n);
}

bool
int_or_uniarg (Args *args, long *n, long uniarg)
{
	if (args_size (args) > 0) {
		long num;
		bool ok = parse_number (args_poll (args), &num);
		if (ok)
			*n = num;
		return ok;
	}
	*n = uniarg;
	return true;
}


/* Loading Lisp. */

void
lisp_loadstring (const char *a, size_t len)
{
	Reader r = { a, len, 0 };
	LexpList *list;
	read_list (&r, &list);
	if (list != NULL)
		for (size_t i = 0; i < list->n; i++)
			lexp_eval (list->v[i]);
	lexp_list_free (list);
}

bool
lisp_loadfile (const char *file)
{
	char *s;
	size_t len;
	if (file == NULL || !read_file (file, &s, &len))
		return false;
	lisp_loadstring (s, len);
	free (s);
	return true;
}

static bool
F_load (long uniarg, Args *args)
{
	(void) uniarg;
	return !args_empty (args) && lisp_loadfile (args_poll (args));
}

void
lisp_init (void)
{
	lisp_func_new ("load", F_load, true,
		       "Execute a file of Lisp code named FILE.");
}
