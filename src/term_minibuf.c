/* Minibuffer handling

   Copyright (c) 1997-2020 Free Software Foundation, Inc.

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

static void
fill_minibuffer_lines (size_t first_row, const char *face_name)
{
	term_apply_face (face_name);
	for (size_t row = first_row; row < term_height (); row++) {
		term_move (row, 0);
		for (size_t i = 0; i < term_width (); i++)
			term_addch (' ');
	}
	term_move (first_row, 0);
}

void
term_minibuf_write (const char *s)
{
	if (!term_initialized ())
		return;

	size_t lines = 1, len = strlen (s);
	for (size_t i = 0; i < len; i++)
		if (s[i] == '\n' && i + 1 < len)
			lines++;
	size_t first_row = lines < term_height () ? term_height () - lines : 0;

	const char *base_face = minibuf_showing_error () ? FACE_ERROR : FACE_DEFAULT;
	fill_minibuffer_lines (first_row, base_face);
	term_apply_face (base_face);
	term_addstr (s);
	term_apply_face (FACE_DEFAULT);
}

/*
 * Return the display-column width of the UTF-8 string `s[0..byte_len)`.
 */
static size_t
display_width_of (const char *s, size_t byte_len)
{
	size_t col = 0, i = 0;
	while (i < byte_len) {
		size_t char_len;
		uint32_t ch = utf8_decode (s, byte_len, i, &char_len);
		if (char_len == 0)
			break;
		int w = utf8_char_display_width (ch);
		col += (size_t) (w > 0 ? w : 1);
		i += char_len;
	}
	return col;
}

static void
draw_read (const char *prompt, const char *val, size_t prompt_len, const char *match, size_t pointo)
{
	size_t width = term_width ();
	size_t val_len = strlen (val);

	fill_minibuffer_lines (term_height () - 1, FACE_DEFAULT);
	term_apply_face (FACE_MINIBUFFER_PROMPT);
	term_addstr (prompt);
	term_apply_face (FACE_DEFAULT);

	/* cursor_col: display columns from start of val to the point. */
	size_t cursor_col = display_width_of (val, pointo);
	size_t avail = width > prompt_len ? width - prompt_len : 1;

	size_t margin = 1;
	size_t n = 0;		/* byte offset of the first visible character */
	size_t n_col = 0;	/* display column corresponding to n */

	if (prompt_len + cursor_col + 1 >= width) {
		margin++;
		term_addstr ("$");
		if (avail > 1)
			avail -= 1;
		size_t target = avail * 2 / 3;
		size_t bi = 0, dcol = 0;
		while (bi < val_len && cursor_col - dcol > target) {
			size_t char_len;
			uint32_t ch = utf8_decode (val, val_len, bi, &char_len);
			if (char_len == 0)
				break;
			dcol += (size_t) utf8_char_display_width (ch);
			bi += char_len;
		}
		n = bi;
		n_col = dcol;
	}

	term_addstr (val + n);
	term_addstr (match);

	/* Determine whether a right-overflow marker is needed. */
	size_t total_width = display_width_of (val, val_len);
	if (total_width - n_col + margin >= avail + 1) {
		term_move (term_height () - 1, width - 1);
		term_addstr ("$");
	}

	/* Place the cursor: prompt + margin + display columns from n to point. */
	term_set_cursor_visible (true);
	term_move (term_height () - 1, prompt_len + margin - 1 + (cursor_col - n_col));

	term_refresh ();
}

static void
maybe_close_popup (Completion *cp)
{
	Window *wp;
	Window *old_wp = cur_wp;
	if (cp != NULL && (cp->flags & COMPLETION_POPPEDUP) &&
	    (wp = window_find ("*Completions*")) != NULL) {
		window_set_current (wp);
		if (cp->flags & COMPLETION_CLOSE)
			funcall ("delete-window");
		else if (cp->old_bp != NULL && buffer_is_live (cp->old_bp)) {
			buffer_switch_to (cp->old_bp, true);
			if (cp->old_pt != NULL && cp->old_pt->bp == cp->old_bp)
				buffer_goto_offset (cp->old_bp, cp->old_pt->o);
		}
		window_set_current (old_wp);
		term_redisplay ();
	}
}

/* Per-session UTF-8 accumulator: feed one byte, return a complete
   character once one is available. */
typedef struct {
	unsigned char bytes[4];
	size_t n;
} Utf8Accum;

static bool
utf8_feed (Utf8Accum *acc, unsigned char b, char *out, size_t *out_len)
{
	if (acc->n == 0) {
		int need = utf8_seq_len (b);
		if (need <= 1) {
			out[0] = (char) b;
			*out_len = 1;
			return true;
		}
		acc->bytes[0] = b;
		acc->n = 1;
		return false;
	}

	if ((b & 0xC0) == 0x80) {
		acc->bytes[acc->n++] = b;
		size_t need = (size_t) utf8_seq_len (acc->bytes[0]);
		if (acc->n == need) {
			size_t char_len;
			uint32_t cp = utf8_decode ((const char *) acc->bytes, need, 0, &char_len);
			acc->n = 0;
			if (cp == UTF8_REPLACEMENT && char_len != need)
				return false; /* invalid sequence: discard */
			memcpy (out, acc->bytes, need);
			*out_len = need;
			return true;
		}
		return false;
	}

	/* Unexpected non-continuation byte: reset and retry. */
	acc->n = 0;
	return utf8_feed (acc, b, out, out_len);
}

typedef struct {
	char *a;		/* The text being edited; NULL when cancelled. */
	size_t pos;		/* Byte offset of the point in `a'. */
	Completion *cp;
	int thistab, lasttab;
	Utf8Accum accum;
} ReadState;

static void
set_text (ReadState *st, char *s)
{
	free (st->a);
	st->a = s;
}

static void
splice (ReadState *st, size_t from, size_t to, const char *ins, size_t ins_len)
{
	size_t len = strlen (st->a);
	char *s = xmalloc (len - (to - from) + ins_len + 1);
	memcpy (s, st->a, from);
	memcpy (s + from, ins, ins_len);
	memcpy (s + from + ins_len, st->a + to, len - to + 1);
	set_text (st, s);
}

static void
some_match (ReadState *st)
{
	Completion *cp = st->cp;
	StrBuf bs;
	sb_init (&bs);
	if (cp->flags & COMPLETION_FILENAME)
		sb_add (&bs, cp->path);
	sb_addn (&bs, cp->match, cp->matchsize);
	if (!str_has_prefix (st->a, bs.s))
		st->thistab = -1;
	set_text (st, sb_take (&bs));
	st->pos = strlen (st->a);
}

static void
do_got_tab (ReadState *st)
{
	Completion *cp = st->cp;
	if (cp == NULL) {
		ding ();
		return;
	}

	if (st->lasttab != -1 && st->lasttab != COMPLETION_NOTMATCHED
	    && (cp->flags & COMPLETION_POPPEDUP)) {
		completion_scroll_up ();
		st->thistab = st->lasttab;
	} else {
		st->thistab = (int) completion_try (cp, st->a, true);

		switch (st->thistab) {
		case COMPLETION_MATCHED:
			maybe_close_popup (cp);
			cp->flags &= ~COMPLETION_POPPEDUP;
			some_match (st);
			break;
		case COMPLETION_MATCHEDNONUNIQUE:
		case COMPLETION_NONUNIQUE:
			some_match (st);
			break;
		case COMPLETION_NOTMATCHED:
			ding ();
			break;
		default:
			break;
		}
	}
}

/* Insert a raw byte `c` (0x20–0xFF) at the point, accumulating
   multi-byte UTF-8 sequences before splicing them in. */
static void
other_key (ReadState *st, Keystroke c)
{
	if (c > 0xFF) {
		ding ();
		return;
	}

	char ch[4];
	size_t ch_len;
	if (!utf8_feed (&st->accum, (unsigned char) c, ch, &ch_len))
		return; /* still accumulating continuation bytes */

	splice (st, st->pos, st->pos, ch, ch_len);
	st->pos += ch_len;
}

static int reading = 0;

/* Is the minibuffer being read? */
bool
term_minibuf_active (void)
{
	return reading > 0;
}

char *
term_minibuf_read (const char *prompt, const char *val, long pos, Completion *cp, History *hp)
{
	reading++;
	if (hp != NULL)
		history_prepare (hp);

	ReadState st;
	st.a = xstrdup (val);
	st.cp = cp;
	st.thistab = 0;
	st.lasttab = -1;
	st.accum.n = 0;
	char *saved = NULL;
	Keystroke c = 0;

	size_t prompt_len = display_width_of (prompt, strlen (prompt));
	st.pos = pos == LONG_MAX || pos < 0 ? strlen (st.a) : MIN ((size_t) pos, strlen (st.a));

	do {
		const char *s;
		switch (st.lasttab) {
		case COMPLETION_MATCHEDNONUNIQUE:
			s = " [Complete, but not unique]";
			break;
		case COMPLETION_NOTMATCHED:
			s = " [No match]";
			break;
		case COMPLETION_MATCHED:
			s = " [Sole completion]";
			break;
		default:
			s = "";
			break;
		}
		draw_read (prompt, st.a, prompt_len, s, st.pos);

		st.thistab = -1;
		size_t len = strlen (st.a);

		switch (c = getkeystroke (GETKEY_DEFAULT)) {
		case KBD_NOKEY:
		case KBD_RET:
			break;
		case KBD_CTRL | 'z':
			funcall ("suspend-emacs");
			break;
		case KBD_CANCEL:
			break;
		case KBD_CTRL | 'a':
		case KBD_HOME:
			st.pos = 0;
			break;
		case KBD_CTRL | 'e':
		case KBD_END:
			st.pos = len;
			break;
		case KBD_CTRL | 'b':
		case KBD_LEFT:
			if (st.pos > 0)
				st.pos = utf8_prev_char (st.a, len, st.pos);
			else
				ding ();
			break;
		case KBD_CTRL | 'f':
		case KBD_RIGHT:
			if (st.pos < len)
				st.pos = utf8_next_char (st.a, len, st.pos);
			else
				ding ();
			break;
		case KBD_CTRL | 'k':
			maybe_destroy_kill_ring ();
			if (st.pos < len) {
				kill_ring_push (ev_of (st.a + st.pos, len - st.pos, eol_lf));
				st.a[st.pos] = '\0';
			} else
				ding ();
			break;
		case KBD_CTRL | 'y':
			if (kill_ring_text != NULL) {
				splice (&st, st.pos, st.pos, kill_ring_text->text, strlen (kill_ring_text->text));
				st.pos += strlen (kill_ring_text->text);
			} else
				ding ();
			break;
		case KBD_BS:
			if (st.pos > 0) {
				size_t new_pos = utf8_prev_char (st.a, len, st.pos);
				splice (&st, new_pos, st.pos, "", 0);
				st.pos = new_pos;
			} else
				ding ();
			break;
		case KBD_CTRL | 'd':
		case KBD_DEL:
			if (st.pos < len)
				splice (&st, st.pos, utf8_next_char (st.a, len, st.pos), "", 0);
			else
				ding ();
			break;
		case KBD_META | 'v':
		case KBD_PGUP:
			if (cp == NULL) {
				ding ();
				break;
			}
			if (cp->flags & COMPLETION_POPPEDUP) {
				completion_scroll_down ();
				st.thistab = st.lasttab;
			}
			break;
		case KBD_CTRL | 'v':
		case KBD_PGDN:
			if (cp == NULL) {
				ding ();
				break;
			}
			if (cp->flags & COMPLETION_POPPEDUP) {
				completion_scroll_up ();
				st.thistab = st.lasttab;
			}
			break;
		case KBD_UP:
		case KBD_META | 'p':
			if (hp != NULL) {
				const char *elem = history_previous_element (hp);
				if (elem != NULL) {
					if (saved == NULL)
						saved = xstrdup (st.a);
					set_text (&st, xstrdup (elem));
					st.pos = strlen (st.a);
				}
			}
			break;
		case KBD_DOWN:
		case KBD_META | 'n':
			if (hp != NULL) {
				const char *elem = history_next_element (hp);
				if (elem != NULL)
					set_text (&st, xstrdup (elem));
				else if (saved != NULL) {
					set_text (&st, saved);
					saved = NULL;
				}
				st.pos = strlen (st.a);
			}
			break;
		case KBD_TAB:
			do_got_tab (&st);
			break;
		case ' ':
			if (cp != NULL) {
				do_got_tab (&st);
				break;
			}
			other_key (&st, c);
			break;
		default:
			other_key (&st, c);
			break;
		}

		st.lasttab = st.thistab;
	} while (c != KBD_RET && c != KBD_CANCEL);

	free (saved);
	if (c == KBD_CANCEL) {
		free (st.a);
		st.a = NULL;
	}

	reading--;
	minibuf_clear ();
	maybe_close_popup (cp);
	return st.a;
}
