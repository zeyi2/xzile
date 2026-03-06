/* UTF-8 utilities

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

[CCode (cheader_filename = "wchar.h", cname = "wcwidth")]
extern int wcwidth (unichar c);

/* Return the display column width of a single Unicode code point */
public int utf8_char_display_width (unichar ch) {
	int w = wcwidth (ch);
	return w < 0 ? 1 : w;
}

/*
 * Return the expected total byte length of a UTF-8 sequence from its
 * lead byte, or 0 when the byte is a continuation byte or is invalid.
 * ASCII bytes return 1.
 */
public int utf8_seq_len (uchar b) {
	if (b < 0x80)  return 1;
	if ((b & 0xE0) == 0xC0) return 2;
	if ((b & 0xF0) == 0xE0) return 3;
	if ((b & 0xF8) == 0xF0) return 4;
	return 0;
}

/*
 * Validate and decode a pre-assembled byte buffer `buf[0..seq_len)`
 * that starts with a non-ASCII lead byte.  Returns the Unicode code
 * point on success, U+FFFD on any encoding error.
 *
 * `seq_len` must equal utf8_seq_len(buf[0]) and all bytes must already
 * be present in `buf`.  The caller is responsible for those invariants.
 */
public uint32 utf8_decode_buf (uint8[] buf, int seq_len) {
	for (int k = 1; k < seq_len; k++) {
		if ((buf[k] & 0xC0) != 0x80)
			return 0xFFFD;
	}

	uint8[] tmp = new uint8[seq_len + 1];
	for (int k = 0; k < seq_len; k++)
		tmp[k] = buf[k];
	tmp[seq_len] = 0;

	unichar vc = ((string) tmp).get_char_validated (seq_len);
	return (long) vc > 0 ? (uint32) vc : 0xFFFD;
}

/*
 * Decode the UTF-8 sequence starting at byte offset `i` inside the
 * byte slice [s, s+len).
 */
public uint32 utf8_decode (char* s, size_t len, size_t i, out size_t char_len) {
	if (i >= len) {
		char_len = 0;
		return 0;
	}

	uchar b = (uchar) s[i];

	if (b < 0x80) {
		char_len = 1;
		return b;
	}

	int seq_len = utf8_seq_len (b);
	if (seq_len == 0) {
		char_len = 1;
		return 0xFFFD;
	}

	if (i + seq_len > len) {
		char_len = 1;
		return 0xFFFD;
	}

	uint8[] buf = new uint8[seq_len];
	buf[0] = b;
	for (int k = 1; k < seq_len; k++)
		buf[k] = (uint8) s[i + k];

	uint32 cp = utf8_decode_buf (buf, seq_len);
	if (cp != 0xFFFD) {
		char_len = seq_len;
		return cp;
	}

	char_len = 1;
	return 0xFFFD;
}

/*
 * Return the byte offset of the start of the next UTF-8 character
 * after position `i` in [s, s+len).  If `i` is already at or past
 * `len`, returns `len`.
 */
public size_t utf8_next_char (char* s, size_t len, size_t i) {
	size_t char_len;
	utf8_decode (s, len, i, out char_len);
	size_t next = i + char_len;
	return next <= len ? next : len;
}

/*
 * Return the byte offset of the start of the UTF-8 character that
 * immediately precedes position `i` in [s, s+len).
 */
public size_t utf8_prev_char (char* s, size_t len, size_t i) {
	if (i == 0)
		return 0;

	size_t p = i - 1;
	while (p > 0 && ((uchar) s[p] & 0xC0) == 0x80)
		p--;

	if (i - p > 4)
		return i - 1;

	size_t char_len;
	utf8_decode (s, len, p, out char_len);
	if (p + char_len == i)
		return p;

	return i - 1;
}
