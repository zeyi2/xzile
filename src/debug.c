/* Debug logging infrastructure

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

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

static int debug_fd = -1;
static StrVec debug_categories;
static bool debug_all_categories = false;

void
debug_set_categories (const char *spec)
{
	debug_all_categories = false;
	sv_clear (&debug_categories);

	if (spec == NULL)
		return;

	for (const char *p = spec; *p != '\0';) {
		const char *end = strchr (p, ',');
		size_t len = end != NULL ? (size_t) (end - p) : strlen (p);
		while (len > 0 && isspace ((unsigned char) *p)) {
			p++;
			len--;
		}
		while (len > 0 && isspace ((unsigned char) p[len - 1]))
			len--;
		if (len == 1 && *p == '*')
			debug_all_categories = true;
		else if (len > 0) {
			char *part = xstrndup (p, len);
			sv_add_sorted (&debug_categories, part);
			free (part);
		}
		if (end == NULL)
			break;
		p = end + 1;
	}
}

void
debug_set_log_path (const char *path)
{
	if (debug_fd >= 0) {
		close (debug_fd);
		debug_fd = -1;
	}

	if (path == NULL || path[0] == '\0')
		return;

	debug_fd = open (path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	if (debug_fd < 0)
		fprintf (stderr, "%s: failed to open debug log `%s'\n", program_name, path);
}

void
debug_init_from_env (void)
{
	debug_set_categories (getenv ("XZILE_DEBUG"));
	debug_set_log_path (getenv ("XZILE_DEBUG_LOG"));
}

bool
debug_enabled (const char *category)
{
	if (debug_fd < 0)
		return false;
	if (debug_all_categories)
		return true;
	return sv_contains (&debug_categories, category);
}

void
debug_log (const char *category, const char *fmt, ...)
{
	if (!debug_enabled (category))
		return;

	va_list ap;
	va_start (ap, fmt);
	char *message = xvasprintf (fmt, ap);
	va_end (ap);
	char *line = xasprintf ("[%lld] %s: %s\n", (long long) monotonic_ms (), category, message);
	if (write (debug_fd, line, strlen (line)) < 0) { /* Ignore error. */ }
	free (line);
	free (message);
}

void
debug_shutdown (void)
{
	if (debug_fd >= 0) {
		close (debug_fd);
		debug_fd = -1;
	}
}
