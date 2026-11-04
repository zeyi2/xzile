/* Utility functions

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

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static NORETURN void
out_of_memory (void)
{
	fputs ("xzile: out of memory\n", stderr);
	abort ();
}

void *
xmalloc (size_t n)
{
	void *p = malloc (n ? n : 1);
	if (p == NULL)
		out_of_memory ();
	return p;
}

void *
xzalloc (size_t n)
{
	void *p = calloc (1, n ? n : 1);
	if (p == NULL)
		out_of_memory ();
	return p;
}

void *
xrealloc (void *p, size_t n)
{
	p = realloc (p, n ? n : 1);
	if (p == NULL)
		out_of_memory ();
	return p;
}

char *
xstrdup (const char *s)
{
	return xstrndup (s, strlen (s));
}

char *
xstrndup (const char *s, size_t n)
{
	char *p = xmalloc (n + 1);
	memcpy (p, s, n);
	p[n] = '\0';
	return p;
}

char *
xstrdup_or_null (const char *s)
{
	return s != NULL ? xstrdup (s) : NULL;
}

char *
xvasprintf (const char *fmt, va_list ap)
{
	va_list ap2;
	va_copy (ap2, ap);
	int n = vsnprintf (NULL, 0, fmt, ap2);
	va_end (ap2);
	if (n < 0)
		return xstrdup ("");
	char *s = xmalloc ((size_t) n + 1);
	vsnprintf (s, (size_t) n + 1, fmt, ap);
	return s;
}

char *
xasprintf (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	char *s = xvasprintf (fmt, ap);
	va_end (ap);
	return s;
}

char *
xstrcat3 (const char *a, const char *b, const char *c)
{
	size_t la = strlen (a), lb = strlen (b), lc = strlen (c);
	char *s = xmalloc (la + lb + lc + 1);
	memcpy (s, a, la);
	memcpy (s + la, b, lb);
	memcpy (s + la + lb, c, lc + 1);
	return s;
}

const char *
xmemmem (const char *hay, size_t hlen, const char *needle, size_t nlen)
{
	if (nlen == 0)
		return hay;
	if (hlen < nlen)
		return NULL;
	const char *end = hay + hlen - nlen;
	for (const char *p = hay; p <= end; p++) {
		p = memchr (p, needle[0], (size_t) (end - p) + 1);
		if (p == NULL)
			return NULL;
		if (memcmp (p, needle, nlen) == 0)
			return p;
	}
	return NULL;
}

bool
str_has_prefix (const char *s, const char *prefix)
{
	return strncmp (s, prefix, strlen (prefix)) == 0;
}

bool
str_has_suffix (const char *s, const char *suffix)
{
	size_t ls = strlen (s), lx = strlen (suffix);
	return ls >= lx && memcmp (s + ls - lx, suffix, lx) == 0;
}

bool
str_eq (const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp (a, b) == 0;
}

/* Parse a whole string as a base-10 long. */
bool
parse_long (const char *s, long *out)
{
	if (s == NULL || *s == '\0')
		return false;
	char *end;
	errno = 0;
	long n = strtol (s, &end, 10);
	if (errno != 0 || *end != '\0')
		return false;
	*out = n;
	return true;
}

char *
path_basename (const char *path)
{
	size_t len = strlen (path);
	while (len > 1 && path[len - 1] == '/')
		len--;
	if (len == 0)
		return xstrdup (".");
	if (len == 1 && path[0] == '/')
		return xstrdup ("/");
	size_t start = len;
	while (start > 0 && path[start - 1] != '/')
		start--;
	return xstrndup (path + start, len - start);
}

char *
path_dirname (const char *path)
{
	size_t len = strlen (path);
	while (len > 1 && path[len - 1] == '/')
		len--;
	while (len > 0 && path[len - 1] != '/')
		len--;
	if (len == 0)
		return xstrdup (".");
	while (len > 1 && path[len - 1] == '/')
		len--;
	return xstrndup (path, len);
}

char *
path_join (const char *dir, const char *name)
{
	size_t ld = strlen (dir);
	if (ld > 0 && dir[ld - 1] == '/')
		return xstrcat3 (dir, "", name);
	return xstrcat3 (dir, "/", name);
}

char *
get_current_dir (void)
{
	size_t size = 256;
	for (;;) {
		char *buf = xmalloc (size);
		if (getcwd (buf, size) != NULL)
			return buf;
		free (buf);
		if (errno != ERANGE) {
			const char *pwd = getenv ("PWD");
			return xstrdup (pwd != NULL ? pwd : "/");
		}
		size *= 2;
	}
}

char *
get_home_dir (void)
{
	const char *home = getenv ("HOME");
	if (home != NULL && *home != '\0')
		return xstrdup (home);
	struct passwd *pw = getpwuid (getuid ());
	if (pw != NULL && pw->pw_dir != NULL)
		return xstrdup (pw->pw_dir);
	return xstrdup ("/");
}

bool
file_exists (const char *path)
{
	struct stat st;
	return stat (path, &st) == 0;
}

bool
file_is_regular (const char *path)
{
	struct stat st;
	return stat (path, &st) == 0 && S_ISREG (st.st_mode);
}

bool
file_is_dir (const char *path)
{
	struct stat st;
	return stat (path, &st) == 0 && S_ISDIR (st.st_mode);
}

/* Read a whole file. The result is NUL-terminated. Sets errno on failure. */
bool
read_file (const char *path, char **out, size_t *len)
{
	int fd = open (path, O_RDONLY);
	if (fd < 0)
		return false;

	struct stat st;
	if (fstat (fd, &st) == 0 && S_ISDIR (st.st_mode)) {
		close (fd);
		errno = EISDIR;
		return false;
	}

	size_t cap = 4096, n = 0;
	char *buf = xmalloc (cap + 1);
	for (;;) {
		if (n == cap) {
			cap *= 2;
			buf = xrealloc (buf, cap + 1);
		}
		ssize_t r = read (fd, buf + n, cap - n);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			int saved = errno;
			free (buf);
			close (fd);
			errno = saved;
			return false;
		}
		if (r == 0)
			break;
		n += (size_t) r;
	}
	close (fd);
	buf[n] = '\0';
	*out = buf;
	*len = n;
	return true;
}

int64_t
monotonic_ms (void)
{
	struct timespec ts;
	clock_gettime (CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


/* String buffers. */

void
sb_init (StrBuf *sb)
{
	sb->cap = 32;
	sb->len = 0;
	sb->s = xmalloc (sb->cap);
	sb->s[0] = '\0';
}

void
sb_addn (StrBuf *sb, const char *s, size_t n)
{
	if (sb->len + n + 1 > sb->cap) {
		while (sb->len + n + 1 > sb->cap)
			sb->cap *= 2;
		sb->s = xrealloc (sb->s, sb->cap);
	}
	memcpy (sb->s + sb->len, s, n);
	sb->len += n;
	sb->s[sb->len] = '\0';
}

void
sb_add (StrBuf *sb, const char *s)
{
	sb_addn (sb, s, strlen (s));
}

void
sb_addc (StrBuf *sb, char c)
{
	sb_addn (sb, &c, 1);
}

void
sb_addf (StrBuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	char *s = xvasprintf (fmt, ap);
	va_end (ap);
	sb_add (sb, s);
	free (s);
}

char *
sb_take (StrBuf *sb)
{
	char *s = sb->s;
	sb->s = NULL;
	sb->len = sb->cap = 0;
	return s;
}

void
sb_free (StrBuf *sb)
{
	free (sb->s);
	sb->s = NULL;
	sb->len = sb->cap = 0;
}


/* String vectors. */

static void
sv_grow (StrVec *sv)
{
	if (sv->n == sv->cap) {
		sv->cap = sv->cap ? sv->cap * 2 : 8;
		sv->v = xrealloc (sv->v, sv->cap * sizeof (char *));
	}
}

void
sv_push (StrVec *sv, const char *s)
{
	sv_grow (sv);
	sv->v[sv->n++] = xstrdup (s);
}

static size_t
sv_lower_bound (const StrVec *sv, const char *s)
{
	size_t lo = 0, hi = sv->n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (strcmp (sv->v[mid], s) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Insert into a sorted vector, keeping it unique. */
bool
sv_add_sorted (StrVec *sv, const char *s)
{
	size_t i = sv_lower_bound (sv, s);
	if (i < sv->n && strcmp (sv->v[i], s) == 0)
		return false;
	sv_grow (sv);
	memmove (sv->v + i + 1, sv->v + i, (sv->n - i) * sizeof (char *));
	sv->v[i] = xstrdup (s);
	sv->n++;
	return true;
}

/* Membership test for a sorted vector. */
bool
sv_contains (const StrVec *sv, const char *s)
{
	size_t i = sv_lower_bound (sv, s);
	return i < sv->n && strcmp (sv->v[i], s) == 0;
}

void
sv_clear (StrVec *sv)
{
	for (size_t i = 0; i < sv->n; i++)
		free (sv->v[i]);
	free (sv->v);
	sv->v = NULL;
	sv->n = sv->cap = 0;
}


/* Hash maps. */

struct MapEntry {
	char *key;
	void *val;
	MapEntry *next;
};

static size_t
str_hash (const char *s)
{
	size_t h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char) *s;
	return h;
}

Map *
map_new (void)
{
	Map *m = xzalloc (sizeof (Map));
	m->nbuckets = 64;
	m->buckets = xzalloc (m->nbuckets * sizeof (MapEntry *));
	return m;
}

void
map_free (Map *m, void (*free_val) (void *))
{
	if (m == NULL)
		return;
	for (size_t i = 0; i < m->nbuckets; i++)
		for (MapEntry *e = m->buckets[i], *next; e != NULL; e = next) {
			next = e->next;
			if (free_val != NULL)
				free_val (e->val);
			free (e->key);
			free (e);
		}
	free (m->buckets);
	free (m);
}

static MapEntry **
map_slot (const Map *m, const char *key)
{
	MapEntry **pe = &m->buckets[str_hash (key) % m->nbuckets];
	while (*pe != NULL && strcmp ((*pe)->key, key) != 0)
		pe = &(*pe)->next;
	return pe;
}

void *
map_get (const Map *m, const char *key)
{
	if (m == NULL || key == NULL)
		return NULL;
	MapEntry *e = *map_slot (m, key);
	return e != NULL ? e->val : NULL;
}

static void
map_rehash (Map *m)
{
	size_t nb = m->nbuckets * 2;
	MapEntry **buckets = xzalloc (nb * sizeof (MapEntry *));
	for (size_t i = 0; i < m->nbuckets; i++)
		for (MapEntry *e = m->buckets[i], *next; e != NULL; e = next) {
			next = e->next;
			size_t h = str_hash (e->key) % nb;
			e->next = buckets[h];
			buckets[h] = e;
		}
	free (m->buckets);
	m->buckets = buckets;
	m->nbuckets = nb;
}

void
map_set (Map *m, const char *key, void *val)
{
	MapEntry **pe = map_slot (m, key);
	if (*pe != NULL) {
		(*pe)->val = val;
		return;
	}
	MapEntry *e = xmalloc (sizeof (MapEntry));
	e->key = xstrdup (key);
	e->val = val;
	e->next = NULL;
	*pe = e;
	if (++m->size > m->nbuckets * 2)
		map_rehash (m);
}

void *
map_remove (Map *m, const char *key)
{
	MapEntry **pe = map_slot (m, key);
	MapEntry *e = *pe;
	if (e == NULL)
		return NULL;
	void *val = e->val;
	*pe = e->next;
	free (e->key);
	free (e);
	m->size--;
	return val;
}

void
map_iter_init (MapIter *it)
{
	it->bucket = 0;
	it->entry = NULL;
}

bool
map_iter_next (const Map *m, MapIter *it, const char **key, void **val)
{
	if (m == NULL)
		return false;
	if (it->entry != NULL)
		it->entry = it->entry->next;
	while (it->entry == NULL) {
		if (it->bucket >= m->nbuckets)
			return false;
		it->entry = m->buckets[it->bucket++];
	}
	if (key != NULL)
		*key = it->entry->key;
	if (val != NULL)
		*val = it->entry->val;
	return true;
}
