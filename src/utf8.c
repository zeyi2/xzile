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

#include "xzile.h"

#include <wchar.h>

/* Return the display column width of a single Unicode code point, as
   drawn by the redisplay: C0 controls and DEL as "^X", C1 controls as
   an octal escape. */
int
utf8_char_display_width (uint32_t ch)
{
	if (ch < 0x20 || ch == 0x7F)
		return 2;
	if (ch >= 0x80 && ch < 0xA0)
		return 4;
	int w = wcwidth ((wchar_t) ch);
	return w < 0 ? 1 : w;
}

/*
 * Return the expected total byte length of a UTF-8 sequence from its
 * lead byte, or 0 when the byte is a continuation byte or is invalid.
 * ASCII bytes return 1.
 */
int
utf8_seq_len (unsigned char b)
{
	if (b < 0x80)
		return 1;
	if ((b & 0xE0) == 0xC0)
		return 2;
	if ((b & 0xF0) == 0xE0)
		return 3;
	if ((b & 0xF8) == 0xF0)
		return 4;
	return 0;
}

/*
 * Decode the UTF-8 sequence starting at byte offset `i` inside the
 * byte slice [s, s+len).  Invalid, overlong and surrogate encodings
 * decode as U+FFFD with a length of one byte.
 */
uint32_t
utf8_decode (const char *s, size_t len, size_t i, size_t *char_len)
{
	if (i >= len) {
		*char_len = 0;
		return 0;
	}

	unsigned char b = (unsigned char) s[i];
	if (b < 0x80) {
		*char_len = 1;
		return b;
	}

	int seq_len = utf8_seq_len (b);
	*char_len = 1;
	if (seq_len == 0 || i + (size_t) seq_len > len)
		return UTF8_REPLACEMENT;

	uint32_t cp = b & (0xFF >> (seq_len + 1));
	for (int k = 1; k < seq_len; k++) {
		unsigned char c = (unsigned char) s[i + k];
		if ((c & 0xC0) != 0x80)
			return UTF8_REPLACEMENT;
		cp = (cp << 6) | (c & 0x3F);
	}

	static const uint32_t min_cp[] = { 0, 0, 0x80, 0x800, 0x10000 };
	if (cp < min_cp[seq_len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		return UTF8_REPLACEMENT;

	*char_len = (size_t) seq_len;
	return cp;
}

/*
 * Return the byte offset of the start of the next UTF-8 character
 * after position `i` in [s, s+len).  If `i` is already at or past
 * `len`, returns `len`.
 */
size_t
utf8_next_char (const char *s, size_t len, size_t i)
{
	size_t char_len;
	utf8_decode (s, len, i, &char_len);
	size_t next = i + char_len;
	return next <= len ? next : len;
}

/*
 * Return the byte offset of the start of the UTF-8 character that
 * immediately precedes position `i` in [s, s+len).
 */
size_t
utf8_prev_char (const char *s, size_t len, size_t i)
{
	if (i == 0)
		return 0;

	size_t p = i - 1;
	while (p > 0 && ((unsigned char) s[p] & 0xC0) == 0x80)
		p--;

	if (i - p > 4)
		return i - 1;

	size_t char_len;
	utf8_decode (s, len, p, &char_len);
	if (p + char_len == i)
		return p;

	return i - 1;
}

/* Encode `cp' into `out' (at least 4 bytes); return the length. */
size_t
utf8_encode (uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char) cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = (char) (0xC0 | (cp >> 6));
		out[1] = (char) (0x80 | (cp & 0x3F));
		return 2;
	} else if (cp < 0x10000) {
		out[0] = (char) (0xE0 | (cp >> 12));
		out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char) (0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char) (0xF0 | (cp >> 18));
	out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char) (0x80 | (cp & 0x3F));
	return 4;
}
