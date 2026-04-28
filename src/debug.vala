/* Debug logging infrastructure */

int debug_fd = -1;
HashTable<string, string>? debug_categories = null;
bool debug_all_categories = false;

void debug_set_categories (string? spec) {
	debug_all_categories = false;
	debug_categories = new HashTable<string, string> (str_hash, str_equal);

	if (spec == null || spec.length == 0)
		return;

	foreach (string raw_part in spec.split (",")) {
		string part = raw_part.strip ();
		if (part.length == 0)
			continue;
		if (part == "*") {
			debug_all_categories = true;
			continue;
		}
		debug_categories.insert (part, part);
	}
}

void debug_set_log_path (string? path) {
	if (debug_fd >= 0) {
		Posix.close (debug_fd);
		debug_fd = -1;
	}

	if (path == null || path.length == 0)
		return;

	debug_fd = Posix.open (path, Posix.O_WRONLY | Posix.O_CREAT | Posix.O_APPEND, 0644);
	if (debug_fd < 0)
		Posix.stderr.printf ("%s: failed to open debug log `%s'\n", program_name, path);
}

void debug_init_from_env () {
	debug_set_categories (Environment.get_variable ("XZILE_DEBUG"));
	debug_set_log_path (Environment.get_variable ("XZILE_DEBUG_LOG"));
}

bool debug_enabled (string category) {
	if (debug_fd < 0)
		return false;
	if (debug_all_categories)
		return true;
	return debug_categories != null && debug_categories.lookup (category) != null;
}

void debug_log (string category, string fmt, ...) {
	if (!debug_enabled (category))
		return;

	int64 now_ms = get_monotonic_time () / 1000;
	string message = fmt.vprintf (va_list ());
	string line = "[%lld] %s: %s\n".printf (now_ms, category, message);
	Posix.write (debug_fd, line, line.length);
}

void debug_shutdown () {
	if (debug_fd >= 0) {
		Posix.close (debug_fd);
		debug_fd = -1;
	}
}
