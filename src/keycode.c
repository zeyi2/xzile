/* Key encoding and decoding functions

   Copyright (c) 1997-2021 Free Software Foundation, Inc.

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

void
kv_push (KeyVec *kv, Keystroke k)
{
	if (kv->n == kv->cap) {
		kv->cap = kv->cap ? kv->cap * 2 : 8;
		kv->v = xrealloc (kv->v, kv->cap * sizeof (Keystroke));
	}
	kv->v[kv->n++] = k;
}

void
kv_free (KeyVec *kv)
{
	free (kv->v);
	kv->v = NULL;
	kv->n = kv->cap = 0;
}

void
kv_copy (KeyVec *dst, const KeyVec *src)
{
	dst->n = dst->cap = 0;
	dst->v = NULL;
	for (size_t i = 0; i < src->n; i++)
		kv_push (dst, src->v[i]);
}

typedef struct {
	Keystroke code;
	const char *name;
	const char *desc;
} KeyInfo;

static const KeyInfo keyinfo[] = {
	{ KBD_BS, "\\BACKSPACE", "<backspace>" },
	{ KBD_CTRL, "\\C-", "C-" },
	{ KBD_DEL, "\\DELETE", "<delete>" },
	{ KBD_DOWN, "\\DOWN", "<down>" },
	/* Kludge to make keystrings work in both Emacs and Zile. */
	{ 033, "\\e", "ESC" },
	{ KBD_ESCAPE, "\\ESC", "<escape>" },
	{ KBD_END, "\\END", "<end>" },
	{ KBD_F1, "\\F1", "<f1>" },
	{ KBD_F10, "\\F10", "<f10>" },
	{ KBD_F11, "\\F11", "<f11>" },
	{ KBD_F12, "\\F12", "<f12>" },
	{ KBD_F2, "\\F2", "<f2>" },
	{ KBD_F3, "\\F3", "<f3>" },
	{ KBD_F4, "\\F4", "<f4>" },
	{ KBD_F5, "\\F5", "<f5>" },
	{ KBD_F6, "\\F6", "<f6>" },
	{ KBD_F7, "\\F7", "<f7>" },
	{ KBD_F8, "\\F8", "<f8>" },
	{ KBD_F9, "\\F9", "<f9>" },
	{ KBD_HOME, "\\HOME", "<home>" },
	{ KBD_INS, "\\INSERT", "<insert>" },
	{ KBD_LEFT, "\\LEFT", "<left>" },
	{ KBD_META, "\\M-", "M-" },
	{ KBD_PGDN, "\\NEXT", "<next>" },
	{ KBD_PGDN, "\\PAGEDOWN", "<pagedown>" },
	{ KBD_PGUP, "\\PAGEUP", "<pageup>" },
	{ KBD_PGUP, "\\PRIOR", "<prior>" },
	{ KBD_RET, "\\RET", "<RET>" },
	/* Kludge to make keystrings work in both Emacs and Zile. */
	{ KBD_RET, "\\r", NULL },
	{ KBD_RIGHT, "\\RIGHT", "<right>" },
	{ ' ', "\\SPC", "SPC" },
	{ KBD_TAB, "\\TAB", "<TAB>" },
	{ KBD_TAB, "\\t", NULL },
	{ KBD_UP, "\\UP", "<up>" },
	{ '\\', "\\\\", "\\" },
};

#define NKEYINFO (sizeof keyinfo / sizeof keyinfo[0])

/*
 * Convert a key code to its description.
 */
static char *
keytodesc (Keystroke key)
{
	for (size_t i = 0; i < NKEYINFO; i++)
		if (keyinfo[i].code == key && keyinfo[i].desc != NULL)
			return xstrdup (keyinfo[i].desc);
	if (key <= 0xff && isgraph ((int) key))
		return xasprintf ("%c", (int) key);
	return xasprintf ("<%x>", key);
}

/*
 * Convert a key chord into its text description
 */
char *
chordtodesc (Keystroke key)
{
	StrBuf sb;
	sb_init (&sb);
	if (key & KBD_CTRL)
		sb_add (&sb, "C-");
	if (key & KBD_META)
		sb_add (&sb, "M-");
	char *s = keytodesc (key & ~(KBD_CTRL | KBD_META));
	sb_add (&sb, s);
	free (s);
	return sb_take (&sb);
}

/*
 * Convert a key string to its key code.
 */
static Keystroke
strtokey (const char *buf, size_t *len)
{
	if (buf[0] == '\\') {
		const KeyInfo *best = NULL;
		size_t best_len = 0;
		for (size_t i = 0; i < NKEYINFO; i++) {
			size_t l = strlen (keyinfo[i].name);
			if (l > best_len && strncmp (buf, keyinfo[i].name, l) == 0) {
				best = &keyinfo[i];
				best_len = l;
			}
		}
		*len = best_len;
		return best != NULL ? best->code : KBD_NOKEY;
	}
	*len = 1;
	return (unsigned char) buf[0];
}

/*
 * Convert a key chord string to its key code.
 */
static Keystroke
strtochord (const char *buf, size_t *len)
{
	Keystroke key = 0, k;

	*len = 0;
	do {
		size_t l;
		k = strtokey (buf + *len, &l);
		if (k == KBD_NOKEY) {
			*len = 0;
			return KBD_NOKEY;
		}
		*len += l;
		key |= k;
	} while ((k == KBD_CTRL || k == KBD_META) && buf[*len] != '\0');

	return key;
}

/*
 * Convert a key sequence string into a key code sequence; return false
 * if it can't be converted.
 */
bool
keystrtovec (const char *key, KeyVec *out)
{
	out->v = NULL;
	out->n = out->cap = 0;
	if (key == NULL)
		return false;
	for (size_t i = 0, len = 0, n = strlen (key); i < n; i += len) {
		Keystroke code = strtochord (key + i, &len);
		if (code == KBD_NOKEY || len == 0) {
			kv_free (out);
			return false;
		}
		kv_push (out, code);
	}
	return true;
}

/*
 * Convert a key code sequence into a descriptive string.
 */
char *
keyvectodesc (const KeyVec *keys)
{
	StrBuf sb;
	sb_init (&sb);
	for (size_t i = 0; i < keys->n; i++) {
		if (i > 0)
			sb_addc (&sb, ' ');
		char *s = chordtodesc (keys->v[i]);
		sb_add (&sb, s);
		free (s);
	}
	return sb_take (&sb);
}
