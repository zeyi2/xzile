/* The HTML preview window of XZile on macOS, using WKWebView

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
 * The macOS counterpart of html_preview.c, with the same protocol: it
 * shows FILE in a window, rendered by WebKit, and reloads it when it
 * changes. The editor (html.c) runs it with its output on a pipe: it
 * prints "ready" once it handles signals, and "error: ..." when a page
 * cannot be shown. SIGUSR1 reloads the page and brings the window to
 * the front; SIGHUP and SIGTERM close it, as does the editor going
 * away.
 *
 * Links and new windows open in this window: nothing is handed to the
 * browser or to another application. --software is accepted, as the
 * Linux helper takes it, and ignored: WebKit decides how to draw.
 *
 * Built with ARC (-fobjc-arc), against Cocoa and WebKit.
 */

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* How often the file and the editor are checked, in seconds. A change
   is acted on once the file has stayed the same for a whole interval,
   as changes come in bursts while a file is written. Polling, unlike a
   watch on the file, follows a save that renames a new file over it. */
#define POLL_INTERVAL 0.2

#define MIN_ZOOM 0.25
#define MAX_ZOOM 5.0

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

typedef struct {
	bool exists;
	dev_t dev;
	ino_t ino;
	off_t size;
	struct timespec mtime, ctime;
} Stamp;

static Stamp
file_stamp (const char *path)
{
	Stamp s;
	memset (&s, 0, sizeof s);
	struct stat st;
	if (stat (path, &st) == 0) {
		s.exists = true;
		s.dev = st.st_dev;
		s.ino = st.st_ino;
		s.size = st.st_size;
		s.mtime = st.st_mtimespec;
		s.ctime = st.st_ctimespec;
	}
	return s;
}

static bool
same_stamp (const Stamp *a, const Stamp *b)
{
	return a->exists == b->exists && a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
	       a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec &&
	       a->ctime.tv_sec == b->ctime.tv_sec && a->ctime.tv_nsec == b->ctime.tv_nsec;
}

/* Schemes shown in the window; others (mailto:, ...) would need
   another application, so are refused. */
static BOOL
is_web_scheme (NSString *scheme)
{
	NSString *s = scheme.lowercaseString;
	return [s isEqualToString:@"file"] || [s isEqualToString:@"http"] || [s isEqualToString:@"https"] ||
	       [s isEqualToString:@"about"] || [s isEqualToString:@"data"] || [s isEqualToString:@"blob"];
}

static BOOL
is_inside (NSURL *url, NSURL *dir)
{
	NSString *d = dir.path;
	if (![d hasSuffix:@"/"])
		d = [d stringByAppendingString:@"/"];
	return [url.URLByStandardizingPath.path hasPrefix:d];
}

static NSMenu *
add_menu (NSMenu *bar, NSString *title)
{
	NSMenuItem *item = [bar addItemWithTitle:title action:NULL keyEquivalent:@""];
	NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
	item.submenu = menu;
	return menu;
}

static NSMenuItem *
add_item (NSMenu *menu, NSString *title, SEL action, NSString *key, NSEventModifierFlags mods, id target)
{
	NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key];
	item.keyEquivalentModifierMask = mods;
	item.target = target;
	return item;
}

static void *title_context = &title_context;

@interface Preview : NSObject <NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate>
- (instancetype)initWithPath:(const char *)arg;
@end

@implementation Preview {
	NSWindow *window;
	WKWebView *view;
	NSURL *fileURL;
	NSURL *dirURL;		/* What the page may read: its directory. */
	NSString *fileName;
	char *path;
	Stamp stamp;
	BOOL changed;		/* The file changed, and may still be changing. */
	pid_t parent;
	NSMutableArray *sources;
	NSTimer *timer;
}

- (instancetype)initWithPath:(const char *)arg
{
	self = [super init];
	if (self == nil)
		return nil;
	NSString *p = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:arg length:strlen (arg)];
	fileURL = [NSURL fileURLWithPath:p isDirectory:NO].URLByStandardizingPath;
	dirURL = fileURL.URLByDeletingLastPathComponent;
	fileName = fileURL.lastPathComponent;
	path = strdup (fileURL.fileSystemRepresentation);
	stamp = file_stamp (path);
	parent = getppid ();
	sources = [NSMutableArray array];
	return self;
}

/* The main menu, with the usual keys of a browser. */
- (void)buildMenu
{
	const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
	NSString *name = @"XZile HTML Preview";
	NSMenu *bar = [[NSMenu alloc] init];

	NSMenu *m = add_menu (bar, name);
	add_item (m, [@"Hide " stringByAppendingString:name], @selector (hide:), @"h", cmd, NSApp);
	add_item (m, @"Hide Others", @selector (hideOtherApplications:), @"h", cmd | NSEventModifierFlagOption, NSApp);
	add_item (m, @"Show All", @selector (unhideAllApplications:), @"", 0, NSApp);
	[m addItem:[NSMenuItem separatorItem]];
	add_item (m, [@"Quit " stringByAppendingString:name], @selector (terminate:), @"q", cmd, NSApp);

	m = add_menu (bar, @"File");
	add_item (m, @"Close Window", @selector (performClose:), @"w", cmd, nil);

	m = add_menu (bar, @"Edit");
	add_item (m, @"Undo", @selector (undo:), @"z", cmd, nil);
	add_item (m, @"Redo", @selector (redo:), @"Z", cmd, nil);
	[m addItem:[NSMenuItem separatorItem]];
	add_item (m, @"Cut", @selector (cut:), @"x", cmd, nil);
	add_item (m, @"Copy", @selector (copy:), @"c", cmd, nil);
	add_item (m, @"Paste", @selector (paste:), @"v", cmd, nil);
	add_item (m, @"Select All", @selector (selectAll:), @"a", cmd, nil);

	m = add_menu (bar, @"View");
	add_item (m, @"Reload Page", @selector (reloadPage:), @"r", cmd, self);
	[m addItem:[NSMenuItem separatorItem]];
	add_item (m, @"Zoom In", @selector (zoomIn:), @"+", cmd, self);
	/* Command-= too, without Shift, as in Safari. */
	NSMenuItem *item = add_item (m, @"Zoom In", @selector (zoomIn:), @"=", cmd, self);
	item.hidden = YES;
	item.allowsKeyEquivalentWhenHidden = YES;
	add_item (m, @"Zoom Out", @selector (zoomOut:), @"-", cmd, self);
	add_item (m, @"Actual Size", @selector (actualSize:), @"0", cmd, self);
	[m addItem:[NSMenuItem separatorItem]];
	add_item (m, @"Enter Full Screen", @selector (toggleFullScreen:), @"f", cmd | NSEventModifierFlagControl, nil);

	m = add_menu (bar, @"History");
	add_item (m, @"Back", @selector (back:), @"[", cmd, self);
	add_item (m, @"Forward", @selector (forward:), @"]", cmd, self);
	add_item (m, @"Back to the File", @selector (home:), @"H", cmd, self);

	m = add_menu (bar, @"Window");
	add_item (m, @"Minimize", @selector (performMiniaturize:), @"m", cmd, nil);
	add_item (m, @"Zoom", @selector (performZoom:), @"", 0, nil);
	[m addItem:[NSMenuItem separatorItem]];
	add_item (m, @"Bring All to Front", @selector (arrangeInFront:), @"", 0, NSApp);
	NSApp.windowsMenu = m;

	NSApp.mainMenu = bar;
}

- (void)loadFile
{
	[view loadFileURL:fileURL allowingReadAccessToURL:dirURL];
}

/* Whether the window shows the file, rather than a page a link led to. */
- (BOOL)showingFile
{
	NSURL *u = view.URL;
	return u == nil || (u.isFileURL && [u.URLByStandardizingPath.path isEqualToString:fileURL.path]);
}

- (void)reloadFile
{
	/* Nothing was shown if the first load failed: there is nothing to
	   reload. */
	if (view.URL == nil)
		[self loadFile];
	else
		[view reload];
}

/* Show a page in this window. A file is given read access to its own
   directory, or to the file's if it is in there. */
- (void)openURL:(NSURL *)url
{
	if (url.isFileURL) {
		NSURL *dir = is_inside (url, dirURL) ? dirURL : url.URLByStandardizingPath.URLByDeletingLastPathComponent;
		[view loadFileURL:url allowingReadAccessToURL:dir];
	} else
		[view loadRequest:[NSURLRequest requestWithURL:url]];
}

- (void)raise
{
	if (window.miniaturized)
		[window deminiaturize:nil];
	[NSApp unhide:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	/* -activate, its replacement, only asks; the editor in front asked
	   for this. */
	[NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
	[window makeKeyAndOrderFront:nil];
	/* Shown even if the system declines to activate us. */
	[window orderFrontRegardless];
}

- (void)updateTitle
{
	NSString *t = view.title;
	window.title = t.length > 0 ? [NSString stringWithFormat:@"%@ - %@ - XZile HTML preview", t, fileName]
				    : [NSString stringWithFormat:@"%@ - XZile HTML preview", fileName];
}

- (void)observeValueForKeyPath:(NSString *)key ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
	if (context == title_context)
		[self updateTitle];
	else
		[super observeValueForKeyPath:key ofObject:object change:change context:context];
}

- (double)zoom
{
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
	if (@available (macOS 11.0, *))
		return view.pageZoom;
#endif
	return view.magnification;
}

- (void)setZoom:(double)zoom
{
	zoom = fmin (fmax (zoom, MIN_ZOOM), MAX_ZOOM);
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 110000
	if (@available (macOS 11.0, *)) {
		view.pageZoom = zoom;
		return;
	}
#endif
	view.magnification = zoom;
}

- (void)reloadPage:(id)sender { [view reload]; }
- (void)zoomIn:(id)sender { [self setZoom:[self zoom] * 1.1]; }
- (void)zoomOut:(id)sender { [self setZoom:[self zoom] / 1.1]; }
- (void)actualSize:(id)sender { [self setZoom:1.0]; }
- (void)back:(id)sender { [view goBack]; }
- (void)forward:(id)sender { [view goForward]; }
- (void)home:(id)sender { [self loadFile]; }

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
	if (item.action == @selector (back:))
		return view.canGoBack;
	if (item.action == @selector (forward:))
		return view.canGoForward;
	return YES;
}

/* Signals arrive through kqueue, on the main queue, so their handlers
   run in the event loop like any other event. */
- (void)onSignal:(int)sig run:(dispatch_block_t)handler
{
	dispatch_source_t s = dispatch_source_create (DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t) sig, 0,
						      dispatch_get_main_queue ());
	dispatch_source_set_event_handler (s, handler);
	dispatch_resume (s);
	[sources addObject:s];
	/* The source sees the signal even when it is ignored. Ignored only
	   now, so that none is lost before the source is there. */
	signal (sig, SIG_IGN);
}

/* Close with the editor, even if it could not say so. */
- (void)watchParent
{
	if (parent > 1) {
		dispatch_source_t s = dispatch_source_create (DISPATCH_SOURCE_TYPE_PROC, (uintptr_t) parent,
							      DISPATCH_PROC_EXIT, dispatch_get_main_queue ());
		if (s != NULL) {
			dispatch_source_set_event_handler (s, ^{
				[NSApp terminate:nil];
			});
			dispatch_resume (s);
			[sources addObject:s];
		}
	}
	/* It may have gone before the source was made; -poll: checks too. */
	if (getppid () != parent)
		[NSApp terminate:nil];
}

- (void)poll:(NSTimer *)t
{
	if (getppid () != parent) {
		[NSApp terminate:nil];
		return;
	}
	Stamp now = file_stamp (path);
	if (!same_stamp (&now, &stamp)) {
		stamp = now;
		changed = YES;
		return;
	}
	/* Missing in the middle of a save, or deleted: wait for it. */
	if (changed && now.exists) {
		changed = NO;
		if ([self showingFile])
			[self reloadFile];
	}
}

- (void)applicationWillFinishLaunching:(NSNotification *)note
{
	[self buildMenu];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	WKWebViewConfiguration *config = [[WKWebViewConfiguration alloc] init];
	view = [[WKWebView alloc] initWithFrame:NSMakeRect (0, 0, 1000, 760) configuration:config];
	view.navigationDelegate = self;
	view.UIDelegate = self;
	view.allowsBackForwardNavigationGestures = YES;
	view.allowsMagnification = YES;
#if defined(__MAC_13_3) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_13_3
	/* Web Inspector, from the context menu or Safari's Develop menu. */
	if (@available (macOS 13.3, *))
		view.inspectable = YES;
#endif
	[view addObserver:self forKeyPath:@"title" options:0 context:title_context];

	window = [[NSWindow alloc] initWithContentRect:NSMakeRect (0, 0, 1000, 760)
					     styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
						       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
					       backing:NSBackingStoreBuffered
						 defer:NO];
	window.releasedWhenClosed = NO;
	window.restorable = NO;
	window.tabbingMode = NSWindowTabbingModeDisallowed;
	window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
	window.delegate = self;
	window.contentView = view;
	[window center];
	[self updateTitle];

	__weak Preview *weak = self;
	[self onSignal:SIGUSR1 run:^{
		Preview *me = weak;
		/* Back to the file, if a link was followed. */
		if ([me showingFile])
			[me reloadFile];
		else
			[me loadFile];
		[me raise];
	}];
	dispatch_block_t quit = ^{
		[NSApp terminate:nil];
	};
	[self onSignal:SIGHUP run:quit];
	[self onSignal:SIGTERM run:quit];
	[self onSignal:SIGINT run:quit];
	[self watchParent];

	timer = [NSTimer timerWithTimeInterval:POLL_INTERVAL target:self selector:@selector (poll:)
				      userInfo:nil repeats:YES];
	/* Common modes: also while a menu is open or the window resized. */
	[[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];

	[self loadFile];
	[self raise];
	[window makeFirstResponder:view];
	say ("ready");
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
	return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
	[timer invalidate];
	[view removeObserver:self forKeyPath:@"title" context:title_context];
}

- (void)webView:(WKWebView *)v
	decidePolicyForNavigationAction:(WKNavigationAction *)action
			decisionHandler:(void (^) (WKNavigationActionPolicy))decide
{
	NSURL *url = action.request.URL;
	if (url == nil) {
		decide (WKNavigationActionPolicyAllow);
		return;
	}
	if (!is_web_scheme (url.scheme)) {
		if (action.targetFrame == nil || action.targetFrame.isMainFrame)
			say ("error: not opening %s: only web pages and files are shown",
			     url.absoluteString.UTF8String);
		decide (WKNavigationActionPolicyCancel);
		return;
	}
	/* target="_blank": here, not in a new window. */
	if (action.targetFrame == nil) {
		decide (WKNavigationActionPolicyCancel);
		[self openURL:url];
		return;
	}
	/* A link to another file, which may be outside the directory the
	   page can read. */
	if (url.isFileURL && action.targetFrame.isMainFrame &&
	    action.navigationType == WKNavigationTypeLinkActivated &&
	    ![url.URLByStandardizingPath.path isEqualToString:v.URL.URLByStandardizingPath.path]) {
		decide (WKNavigationActionPolicyCancel);
		[self openURL:url];
		return;
	}
	decide (WKNavigationActionPolicyAllow);
}

/* window.open: shown here too, and the page gets no window object. */
- (WKWebView *)webView:(WKWebView *)v
	createWebViewWithConfiguration:(WKWebViewConfiguration *)config
		   forNavigationAction:(WKNavigationAction *)action
			windowFeatures:(WKWindowFeatures *)features
{
	NSURL *url = action.request.URL;
	if (url != nil && is_web_scheme (url.scheme) && ![url.absoluteString isEqualToString:@"about:blank"])
		[self openURL:url];
	return nil;
}

- (void)loadFailed:(NSError *)error
{
	if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled)
		return;
	/* WebKitErrorFrameLoadInterruptedByPolicyChange: a navigation we
	   cancelled or redirected. */
	if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102)
		return;
	NSURL *url = error.userInfo[NSURLErrorFailingURLErrorKey];
	NSString *where = url != nil ? url.absoluteString : fileURL.absoluteString;
	say ("error: cannot load %s: %s", where.UTF8String, error.localizedDescription.UTF8String);
}

- (void)webView:(WKWebView *)v didFailProvisionalNavigation:(WKNavigation *)nav withError:(NSError *)error
{
	[self loadFailed:error];
}

- (void)webView:(WKWebView *)v didFailNavigation:(WKNavigation *)nav withError:(NSError *)error
{
	[self loadFailed:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)v
{
	say ("error: the web process crashed; reloading");
	[self reloadFile];
}

@end

static void
usage (FILE *f)
{
	fprintf (f, "Usage: xzile-html-preview [--software] [--] FILE\n"
		 "Show an HTML file rendered by WebKit, reloading it when it changes.\n"
		 "\n"
		 "  --software   accepted for the Linux helper's sake, and ignored\n"
		 "\n"
		 "Keys: Command-R reloads, Command-+, Command-- and Command-0 zoom,\n"
		 "Command-[ and Command-] go back and forward, Command-Shift-H returns\n"
		 "to the file, Command-W closes and Command-Q quits.\n"
		 "SIGUSR1 reloads and raises the window.\n");
}

/* The application's delegate, kept alive for NSApp, which does not. */
static Preview *preview;

int
main (int argc, char **argv)
{
	const char *path = NULL;
	for (int i = 1; i < argc; i++) {
		if (path == NULL && strcmp (argv[i], "--software") == 0)
			continue;
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

	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		preview = [[Preview alloc] initWithPath:path];
		app.delegate = preview;
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[app run];
	}
	return 0;
}
