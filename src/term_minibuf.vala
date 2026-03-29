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

namespace TermMinibuf {
	public void write (string s) {
		term_move (term_height () - 1, 0);
		term_clrtoeol ();
		term_apply_face (Minibuf.showing_error () ? FACE_ERROR : FACE_DEFAULT);
		term_addstr (s);
		term_apply_face (FACE_DEFAULT);
	}

	/*
	 * Return the display-column width of the UTF-8 string `s[0..byte_len)`.
	 * Uses utf8_char_display_width() (wcwidth) for each code point.
	 */
	size_t display_width_of (string s, long byte_len) {
		size_t col = 0;
		long i = 0;
		while (i < byte_len) {
			unichar ch = s.get_char (i);
			if (ch == 0) break;
			int w = utf8_char_display_width (ch);
			col += (size_t) (w > 0 ? w : 1);
			long next = utf8_next_pos (s, i);
			if (next <= i) break; /* safety against infinite loop */
			i = next;
		}
		return col;
	}

	void draw_read (string prompt, string val,
					size_t prompt_len, string match, size_t pointo) {
		term_move (term_height () - 1, 0);
		term_clrtoeol ();
		term_apply_face (FACE_MINIBUFFER_PROMPT);
		term_addstr (prompt);
		term_apply_face (FACE_DEFAULT);

		/* cursor_col: display columns from start of val to the point. */
		size_t cursor_col = display_width_of (val, (long) pointo);
		size_t avail = term_width () - prompt_len;

		int margin = 1;
		size_t n = 0;    /* byte offset of the first visible character */
		size_t n_col = 0; /* display column corresponding to n */

		if (prompt_len + cursor_col + 1 >= term_width ()) {
			margin++;
			term_addstr ("$");
			avail -= 1;
			size_t target = avail * 2 / 3;
			long bi = 0;
			size_t dcol = 0;
			while (bi < (long) val.length && cursor_col - dcol > target) {
				unichar ch = val.get_char (bi);
				if (ch == 0) break;
				size_t w = (size_t) utf8_char_display_width (ch);
				dcol += w;
				long next = utf8_next_pos (val, bi);
				if (next <= bi) break;
				bi = next;
			}
			n     = (size_t) bi;
			n_col = dcol;
		}

		term_addstr (val.substring ((long) n));
		term_addstr (match);

		/* Determine whether a right-overflow marker is needed.
		 * Reuse: total_width - n_col gives the display width from n to end. */
		size_t total_width = display_width_of (val, (long) val.length);
		if (total_width - n_col >= avail - margin + 1) {
			term_move (term_height () - 1, term_width () - 1);
			term_addstr ("$");
		}

		/* Place the cursor: prompt + margin + display columns from n to point.
		 * cursor_col and n_col were both computed above — no extra scan. */
		term_move (term_height () - 1,
				   prompt_len + margin - 1 + (cursor_col - n_col));

		term_refresh ();
	}

	void maybe_close_popup (Completion? cp) {
		Window wp = null;
		Window old_wp = cur_wp;
		if (cp != null && (cp.flags & Completion.Flags.POPPEDUP) != 0 &&
			(wp = Window.find ("*Completions*")) != null) {
			wp.set_current ();
			if ((cp.flags & Completion.Flags.CLOSE) != 0)
				funcall ("delete-window");
			else if (cp.old_bp != null)
				cp.old_bp.switch_to ();
			old_wp.set_current ();
			term_redisplay ();
		}
	}

	string? utf8_feed (ref uint8[] accum, uchar b) {
		if (accum.length == 0) {
			int need = utf8_seq_len (b);
			if (need <= 1)
				return ((string) new uint8[] { b, 0 });
			accum = new uint8[] { b };
			return null;
		}

		if ((b & 0xC0) == 0x80) {
			int old_len = accum.length;
			accum.resize (old_len + 1);
			accum[old_len] = b;
			int need = utf8_seq_len (accum[0]);
			if (accum.length == need) {
				uint8[] tmp = accum;
				accum = new uint8[0];
				tmp.resize (need + 1);
				tmp[need] = 0;
				string s = (string) tmp;
				if (s.get_char_validated (-1) > 0)
					return s[0 : need];
				return null; /* invalid sequence: discard */
			}
			return null;
		}

		/* Unexpected non-continuation byte: reset and retry. */
		accum = new uint8[0];
		return utf8_feed (ref accum, b);
	}

	/* Thin wrappers so callers can use (string, long) signatures. */
	long utf8_prev_pos (string s, long pos) {
		return (long) utf8_prev_char ((char*) s, (size_t) s.length, (size_t) pos);
	}

	long utf8_next_pos (string s, long pos) {
		return (long) utf8_next_char ((char*) s, (size_t) s.length, (size_t) pos);
	}

	delegate void Closure ();
	public string? read (string prompt, string val, long pos, Completion? cp, History? hp) {
		if (hp != null)
			hp.prepare ();

		/* Per-session UTF-8 accumulator (see utf8_feed). */
		uint8[] accum = new uint8[0];

		uint c = 0;
		int thistab = 0, lasttab = -1;
		string? a = val, saved = null;

		size_t prompt_len = prompt.length;
		if (pos == long.MAX)
			pos = (long) a.length;

		Closure do_got_tab = () => {
			if (cp == null) {
				ding ();
				return;
			}

			if (lasttab != -1 && lasttab != Completion.Code.notmatched
				&& (Completion.Flags.POPPEDUP in cp.flags)) {
				Completion.scroll_up ();
				thistab = lasttab;
			} else {
				thistab = cp.try (a, true);

				Closure some_match = () => {
					string bs = "";
					if (Completion.Flags.FILENAME in cp.flags)
						bs = cp.path;
					bs += cp.match.substring (0, cp.matchsize);
					if (!a.has_prefix (bs))
						thistab = -1;
					a = bs;
					pos = (long) a.length;
				};

				switch (thistab) {
				case Completion.Code.matched:
					maybe_close_popup (cp);
					cp.flags &= ~Completion.Flags.POPPEDUP;
					some_match ();
					break;
				case Completion.Code.matchednonunique:
					some_match ();
					break;
				case Completion.Code.nonunique:
					some_match ();
					break;
				case Completion.Code.notmatched:
					ding ();
					break;
				default:
					break;
				}
			}
		};

		/* Insert a raw byte `c` (0x20–0xFF) into `a` at `pos`, accumulating
		 * multi-byte UTF-8 sequences before splicing them in. */
		Closure other_key = () => {
			if (c > 0xFF) {
				ding ();
				return;
			}

			string? ch = utf8_feed (ref accum, (uchar) c);
			if (ch == null)
				return; /* still accumulating continuation bytes */

			a = a.slice (0, pos) + ch + a.substring (pos);
			pos += (long) ch.length;
		};

		do {
			string s;
			switch (lasttab) {
			case Completion.Code.matchednonunique:
				s = " [Complete, but not unique]";
				break;
			case Completion.Code.notmatched:
				s = " [No match]";
				break;
			case Completion.Code.matched:
				s = " [Sole completion]";
				break;
			default:
				s = "";
				break;
			}
			draw_read (prompt, a, prompt_len, s, pos);

			thistab = -1;

			switch (c = getkeystroke (GETKEY_DEFAULT)) {
			case KBD_NOKEY:
			case KBD_RET:
				break;
			case KBD_CTRL | 'z':
				funcall ("suspend-emacs");
				break;
			case KBD_CANCEL:
				a = null;
				break;
			case KBD_CTRL | 'a':
			case KBD_HOME:
				pos = 0;
				break;
			case KBD_CTRL | 'e':
			case KBD_END:
				pos = (long) a.length;
				break;
			case KBD_CTRL | 'b':
			case KBD_LEFT:
				if (pos > 0)
					pos = utf8_prev_pos (a, pos);
				else
					ding ();
				break;
			case KBD_CTRL | 'f':
			case KBD_RIGHT:
				if (pos < (long) a.length)
					pos = utf8_next_pos (a, pos);
				else
					ding ();
				break;
			case KBD_CTRL | 'k':
				maybe_destroy_kill_ring ();
				if (pos < (long) a.length) {
					string rest = a.substring (pos);
					kill_ring_push (ImmutableEstr.of (rest, rest.length));
					a = a.substring (0, pos);
				} else
					ding ();
				break;
			case KBD_CTRL | 'y':
				a += (string) kill_ring_text.text;
				break;
			case KBD_BS:
				if (pos > 0) {
					long new_pos = utf8_prev_pos (a, pos);
					a = a.slice (0, new_pos) + a.substring (pos);
					pos = new_pos;
				} else
					ding ();
				break;
			case KBD_CTRL | 'd':
			case KBD_DEL:
				if (pos < (long) a.length) {
					long next = utf8_next_pos (a, pos);
					a = a.slice (0, pos) + a.substring (next);
				} else
					ding ();
				break;
			case KBD_META | 'v':
			case KBD_PGUP:
				if (cp == null) {
					ding ();
					break;
				}

				if ((cp.flags & Completion.Flags.POPPEDUP) != 0) {
					Completion.scroll_down ();
					thistab = lasttab;
				}
				break;
			case KBD_CTRL | 'v':
			case KBD_PGDN:
				if (cp == null) {
					ding ();
					break;
				}

				if ((cp.flags & Completion.Flags.POPPEDUP) != 0) {
					Completion.scroll_up ();
					thistab = lasttab;
				}
				break;
			case KBD_UP:
			case KBD_META | 'p':
				if (hp != null) {
					string? elem = hp.previous_element ();
					if (elem != null) {
						if (saved == null)
							saved = a;
						a = elem;
					}
				}
				break;
			case KBD_DOWN:
			case KBD_META | 'n':
				if (hp != null) {
					string? elem = hp.next_element ();
					if (elem != null)
						a = elem;
					else if (saved != null) {
						a = saved;
						saved = null;
					}
				}
				break;
			case KBD_TAB:
				do_got_tab ();
				break;
			case ' ':
				if (cp != null) {
					do_got_tab ();
					break;
				}
				other_key ();
				break;
			default:
				other_key ();
				break;
			}

			lasttab = thistab;
		} while (c != KBD_RET && c != KBD_CANCEL);

		Minibuf.clear ();
		maybe_close_popup (cp);
		return a;
	}
}
