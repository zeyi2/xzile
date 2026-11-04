/* The HTML preview window of XZile, using WebKitGTK

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

/*
 * xzile-html-preview [--software] [--] FILE
 *
 * Shows FILE in a window, rendered by WebKit, and reloads it when it
 * changes. The editor (html.c) runs it with its output on a pipe:
 * it prints "ready" once it handles signals, and "error: ..." when a
 * page cannot be shown. SIGUSR1 reloads the page and brings the window
 * to the front; SIGHUP and SIGTERM close it, as does the editor going
 * away.
 *
 * WebKit composites on the GPU when it can. --software makes it draw
 * everything on the CPU instead.
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

/* Changes come in bursts while a file is written. */
#define RELOAD_DELAY_MS 150

static GtkWidget *window;
static WebKitWebView *view;
static char *file_uri;
static char *file_name;
static guint reload_source;
static pid_t parent;

static void
say (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	vprintf (fmt, ap);
	va_end (ap);
	putchar ('\n');
	fflush (stdout);
}

/* Reload, unless the user has followed a link elsewhere. */
static void
reload (void)
{
	const char *uri = webkit_web_view_get_uri (view);
	if (uri == NULL || strcmp (uri, file_uri) == 0)
		webkit_web_view_reload (view);
}

static gboolean
reload_later (gpointer data)
{
	(void) data;
	reload_source = 0;
	reload ();
	return G_SOURCE_REMOVE;
}

static void
file_changed (GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event, gpointer data)
{
	(void) monitor;
	(void) file;
	(void) other;
	(void) data;
	if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && event != G_FILE_MONITOR_EVENT_CREATED &&
	    event != G_FILE_MONITOR_EVENT_CHANGED && event != G_FILE_MONITOR_EVENT_MOVED_IN &&
	    event != G_FILE_MONITOR_EVENT_RENAMED)
		return;
	if (reload_source != 0)
		g_source_remove (reload_source);
	reload_source = g_timeout_add (RELOAD_DELAY_MS, reload_later, NULL);
}

static gboolean
on_usr1 (gpointer data)
{
	(void) data;
	/* Back to the file, if a link was followed. */
	const char *uri = webkit_web_view_get_uri (view);
	if (uri != NULL && strcmp (uri, file_uri) != 0)
		webkit_web_view_load_uri (view, file_uri);
	else
		webkit_web_view_reload (view);
	gtk_window_present (GTK_WINDOW (window));
	return G_SOURCE_CONTINUE;
}

static gboolean
on_quit_signal (gpointer data)
{
	(void) data;
	gtk_main_quit ();
	return G_SOURCE_CONTINUE;
}

/* Close with the editor, even if it could not say so. */
static gboolean
check_parent (gpointer data)
{
	(void) data;
	if (getppid () != parent)
		gtk_main_quit ();
	return G_SOURCE_CONTINUE;
}

static gboolean
load_failed (WebKitWebView *v, WebKitLoadEvent event, char *uri, GError *error, gpointer data)
{
	(void) v;
	(void) event;
	(void) data;
	if (!g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
		say ("error: cannot load %s: %s", uri, error->message);
	return FALSE;
}

static void
web_process_terminated (WebKitWebView *v, WebKitWebProcessTerminationReason reason, gpointer data)
{
	(void) data;
	say ("error: the web process %s; reloading",
	     reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "ran out of memory" : "crashed");
	webkit_web_view_reload (v);
}

static void
title_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
	(void) object;
	(void) pspec;
	(void) data;
	const char *t = webkit_web_view_get_title (view);
	char *title = g_strdup_printf ("%s%s%s - XZile HTML preview", t != NULL && *t != '\0' ? t : "",
				       t != NULL && *t != '\0' ? " - " : "", file_name);
	gtk_window_set_title (GTK_WINDOW (window), title);
	g_free (title);
}

/* Reload, zoom, go back and forward, and close from the keyboard. */
static gboolean
key_press (GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	(void) w;
	(void) data;
	GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask ();
	gboolean ctrl = mods == GDK_CONTROL_MASK, alt = mods == GDK_MOD1_MASK;
	double zoom = webkit_web_view_get_zoom_level (view);
	switch (ev->keyval) {
	case GDK_KEY_F5:
		webkit_web_view_reload (view);
		return TRUE;
	case GDK_KEY_r:
		if (!ctrl)
			break;
		webkit_web_view_reload (view);
		return TRUE;
	case GDK_KEY_plus:
	case GDK_KEY_equal:
	case GDK_KEY_KP_Add:
		if (!ctrl && mods != (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
			break;
		webkit_web_view_set_zoom_level (view, MIN (zoom * 1.1, 5.0));
		return TRUE;
	case GDK_KEY_minus:
	case GDK_KEY_KP_Subtract:
		if (!ctrl)
			break;
		webkit_web_view_set_zoom_level (view, MAX (zoom / 1.1, 0.25));
		return TRUE;
	case GDK_KEY_0:
	case GDK_KEY_KP_0:
		if (!ctrl)
			break;
		webkit_web_view_set_zoom_level (view, 1.0);
		return TRUE;
	case GDK_KEY_Left:
		if (!alt)
			break;
		webkit_web_view_go_back (view);
		return TRUE;
	case GDK_KEY_Right:
		if (!alt)
			break;
		webkit_web_view_go_forward (view);
		return TRUE;
	case GDK_KEY_Home:
		if (!alt)
			break;
		webkit_web_view_load_uri (view, file_uri);
		return TRUE;
	case GDK_KEY_q:
	case GDK_KEY_w:
		if (!ctrl)
			break;
		gtk_main_quit ();
		return TRUE;
	default:
		break;
	}
	return FALSE;
}

static void
usage (FILE *f)
{
	fprintf (f, "Usage: xzile-html-preview [--software] [--] FILE\n"
		 "Show an HTML file rendered by WebKit, reloading it when it changes.\n"
		 "\n"
		 "  --software   draw on the CPU rather than compositing on the GPU\n"
		 "\n"
		 "Keys: F5 or Ctrl+R reload, Ctrl+plus, Ctrl+minus and Ctrl+0 zoom,\n"
		 "Alt+Left and Alt+Right go back and forward, Alt+Home returns to the\n"
		 "file, Ctrl+W or Ctrl+Q closes.\n"
		 "SIGUSR1 reloads and raises the window.\n");
}

int
main (int argc, char **argv)
{
	gboolean software = FALSE;
	const char *path = NULL;
	for (int i = 1; i < argc; i++) {
		if (path == NULL && strcmp (argv[i], "--software") == 0)
			software = TRUE;
		else if (path == NULL && strcmp (argv[i], "--help") == 0) {
			usage (stdout);
			return 0;
		} else if (path == NULL && strcmp (argv[i], "--") == 0 && i + 1 < argc)
			path = argv[++i];
		else if (path == NULL && argv[i][0] != '-')
			path = argv[i];
		else {
			usage (stderr);
			return 2;
		}
	}
	if (path == NULL) {
		usage (stderr);
		return 2;
	}

	/* The editor's end of the pipe may close first. */
	signal (SIGPIPE, SIG_IGN);
	parent = getppid ();

	if (!gtk_init_check (&argc, &argv)) {
		say ("error: cannot open a window (is there a display?)");
		return 1;
	}

	GFile *file = g_file_new_for_commandline_arg (path);
	file_uri = g_file_get_uri (file);
	file_name = g_file_get_basename (file);
	GError *err = NULL;
	GFileMonitor *monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &err);
	if (monitor != NULL)
		g_signal_connect (monitor, "changed", G_CALLBACK (file_changed), NULL);
	else {
		say ("error: cannot watch %s for changes: %s", path, err->message);
		g_clear_error (&err);
	}

	WebKitSettings *settings = webkit_settings_new ();
	webkit_settings_set_hardware_acceleration_policy (settings, software ? WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER
							  : WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
	webkit_settings_set_enable_developer_extras (settings, TRUE);
	view = WEBKIT_WEB_VIEW (webkit_web_view_new_with_settings (settings));
	g_object_unref (settings);

	window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size (GTK_WINDOW (window), 1000, 760);
	gtk_window_set_role (GTK_WINDOW (window), "xzile-html-preview");
	gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (view));
	g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), NULL);
	g_signal_connect (window, "key-press-event", G_CALLBACK (key_press), NULL);
	g_signal_connect (view, "load-failed", G_CALLBACK (load_failed), NULL);
	g_signal_connect (view, "web-process-terminated", G_CALLBACK (web_process_terminated), NULL);
	g_signal_connect (view, "notify::title", G_CALLBACK (title_changed), NULL);
	title_changed (NULL, NULL, NULL);

	g_unix_signal_add (SIGUSR1, on_usr1, NULL);
	g_unix_signal_add (SIGHUP, on_quit_signal, NULL);
	g_unix_signal_add (SIGTERM, on_quit_signal, NULL);
	g_unix_signal_add (SIGINT, on_quit_signal, NULL);
	g_timeout_add_seconds (1, check_parent, NULL);

	webkit_web_view_load_uri (view, file_uri);
	gtk_widget_show_all (window);
	gtk_widget_grab_focus (GTK_WIDGET (view));
	say ("ready");
	gtk_main ();

	if (monitor != NULL)
		g_object_unref (monitor);
	g_object_unref (file);
	g_free (file_uri);
	g_free (file_name);
	return 0;
}
