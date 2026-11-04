/* Main types and definitions

   Copyright (c) 1997-2020 Free Software Foundation, Inc.
   Copyright (c) 2025-2026 Zeyi2 <zeyi2@nekoarch.cc>

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

#ifndef XZILE_H
#define XZILE_H

#if !defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <time.h>

#ifndef PACKAGE
#define PACKAGE "xzile"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "XZile"
#endif
#ifndef PACKAGE_BUGREPORT
#define PACKAGE_BUGREPORT "bug-zile@gnu.org"
#endif
#ifndef VERSION
#define VERSION "2.7.0"
#endif
#ifndef PATH_DOCDIR
#define PATH_DOCDIR "/usr/local/share/doc/xzile"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(f, a) __attribute__ ((format (printf, f, a)))
#define NORETURN __attribute__ ((noreturn))
#else
#define PRINTF_LIKE(f, a)
#define NORETURN
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NOPOS SIZE_MAX

/*--------------------------------------------------------------------------
 * Utilities (util.c).
 *--------------------------------------------------------------------------*/

void *xmalloc (size_t n);
void *xzalloc (size_t n);
void *xrealloc (void *p, size_t n);
char *xstrdup (const char *s);
char *xstrndup (const char *s, size_t n);
char *xstrdup_or_null (const char *s);
char *xvasprintf (const char *fmt, va_list ap);
char *xasprintf (const char *fmt, ...) PRINTF_LIKE (1, 2);
char *xstrcat3 (const char *a, const char *b, const char *c);
const char *xmemmem (const char *hay, size_t hlen, const char *needle, size_t nlen);
bool str_has_prefix (const char *s, const char *prefix);
bool str_has_suffix (const char *s, const char *suffix);
bool str_eq (const char *a, const char *b);
bool parse_long (const char *s, long *out);
char *path_basename (const char *path);
char *path_dirname (const char *path);
char *path_join (const char *dir, const char *name);
char *get_current_dir (void);
char *get_home_dir (void);
bool file_exists (const char *path);
bool file_is_regular (const char *path);
bool file_is_dir (const char *path);
bool read_file (const char *path, char **out, size_t *len);
int64_t monotonic_ms (void);

typedef struct {
	char *s;
	size_t len, cap;
} StrBuf;

void sb_init (StrBuf *sb);
void sb_addn (StrBuf *sb, const char *s, size_t n);
void sb_add (StrBuf *sb, const char *s);
void sb_addc (StrBuf *sb, char c);
void sb_addf (StrBuf *sb, const char *fmt, ...) PRINTF_LIKE (2, 3);
char *sb_take (StrBuf *sb);
void sb_free (StrBuf *sb);

/* A vector of owned strings; optionally kept sorted and unique. */
typedef struct {
	char **v;
	size_t n, cap;
} StrVec;

void sv_push (StrVec *sv, const char *s);
bool sv_add_sorted (StrVec *sv, const char *s);
bool sv_contains (const StrVec *sv, const char *s);
void sv_clear (StrVec *sv);

/* A string-keyed hash table of pointers. */
typedef struct MapEntry MapEntry;
typedef struct {
	MapEntry **buckets;
	size_t nbuckets, size;
} Map;

typedef struct {
	size_t bucket;
	MapEntry *entry;
} MapIter;

Map *map_new (void);
void map_free (Map *m, void (*free_val) (void *));
void *map_get (const Map *m, const char *key);
void map_set (Map *m, const char *key, void *val);
void *map_remove (Map *m, const char *key);
void map_iter_init (MapIter *it);
bool map_iter_next (const Map *m, MapIter *it, const char **key, void **val);

/*--------------------------------------------------------------------------
 * Strings with line ending encoding (estr.c).
 *--------------------------------------------------------------------------*/

extern const char eol_lf[];
extern const char eol_crlf[];
extern const char eol_cr[];

/* A non-owning view of counted text with an EOL type. */
typedef struct {
	const char *text;
	size_t len;
	const char *eol;
} EstrView;

/* An owned, growable string with an EOL type. Always NUL-terminated. */
typedef struct {
	char *text;
	size_t len;
	const char *eol;
	size_t buf_size;
} Estr;

EstrView ev_of (const char *text, size_t len, const char *eol);
EstrView ev_cstr (const char *s);
EstrView ev_empty (void);
EstrView ev_substring (EstrView ev, size_t pos, size_t len);
size_t ev_prev_line (EstrView ev, size_t o);
size_t ev_next_line (EstrView ev, size_t o);
size_t ev_start_of_line (EstrView ev, size_t o);
size_t ev_end_of_line (EstrView ev, size_t o);
size_t ev_lines (EstrView ev);
size_t ev_len_with_eol (EstrView ev, const char *eol_type);

Estr *estr_new (const char *eol);
Estr *estr_copy (EstrView ev);
void estr_free (Estr *es);
EstrView estr_view (const Estr *es);
void estr_set_eol_from_text (Estr *es);
void estr_replace (Estr *es, size_t pos, EstrView src);
void estr_cat (Estr *es, EstrView src);
void estr_remove (Estr *es, size_t pos, size_t size);
void estr_insert (Estr *es, size_t pos, size_t size);
void estr_move (Estr *es, size_t to, size_t from, size_t n);
void estr_set (Estr *es, size_t pos, int c, size_t n);
Estr *estr_from_file (const char *filename);

/*--------------------------------------------------------------------------
 * UTF-8 (utf8.c).
 *--------------------------------------------------------------------------*/

#define UTF8_REPLACEMENT 0xFFFD

int utf8_char_display_width (uint32_t ch);
int utf8_seq_len (unsigned char b);
uint32_t utf8_decode (const char *s, size_t len, size_t i, size_t *char_len);
size_t utf8_next_char (const char *s, size_t len, size_t i);
size_t utf8_prev_char (const char *s, size_t len, size_t i);
size_t utf8_encode (uint32_t cp, char *out);

/* Accumulates typed bytes into whole UTF-8 characters. */
typedef struct {
	unsigned char bytes[4];
	size_t n;
	size_t need;
} Utf8Pending;

bool utf8_pending_feed (Utf8Pending *acc, unsigned char b, char *out, size_t *out_len);

/*--------------------------------------------------------------------------
 * Keyboard handling (keycode.c, getkey.c).
 *--------------------------------------------------------------------------*/

typedef unsigned Keystroke;

/* Special value returned for invalid key codes, or when no key is pressed. */
#define KBD_NOKEY UINT_MAX

/* Key modifiers. */
#define KBD_CTRL 01000u
#define KBD_META 02000u

/* Common non-alphanumeric keys. */
#define KBD_CANCEL (KBD_CTRL | 'g')
#define KBD_TAB 00402u
#define KBD_RET 00403u
#define KBD_PGUP 00404u
#define KBD_PGDN 00405u
#define KBD_HOME 00406u
#define KBD_END 00407u
#define KBD_DEL 00410u
#define KBD_BS 00411u
#define KBD_INS 00412u
#define KBD_LEFT 00413u
#define KBD_RIGHT 00414u
#define KBD_UP 00415u
#define KBD_DOWN 00416u
#define KBD_F1 00420u
#define KBD_F2 00421u
#define KBD_F3 00422u
#define KBD_F4 00423u
#define KBD_F5 00424u
#define KBD_F6 00425u
#define KBD_F7 00426u
#define KBD_F8 00427u
#define KBD_F9 00430u
#define KBD_F10 00431u
#define KBD_F11 00432u
#define KBD_F12 00433u
#define KBD_BRACKETED_PASTE 00434u
#define KBD_ESCAPE 00435u	/* A bare ESC, in terminal buffers. */
/* Mouse events, which only the GUI produces; term_mouse_position
   gives the cell they happened at. */
#define KBD_MOUSE_1 00436u
#define KBD_DRAG_MOUSE_1 00437u
#define KBD_WHEEL_UP 00440u
#define KBD_WHEEL_DOWN 00441u

typedef struct {
	Keystroke *v;
	size_t n, cap;
} KeyVec;

void kv_push (KeyVec *kv, Keystroke k);
void kv_free (KeyVec *kv);
void kv_copy (KeyVec *dst, const KeyVec *src);

char *chordtodesc (Keystroke key);
bool keystrtovec (const char *key, KeyVec *out);
char *keyvectodesc (const KeyVec *keys);

/* Standard pauses in ms. */
#define GETKEY_DEFAULT -1
#define GETKEY_DELAYED 2000

Keystroke lastkey (void);
Keystroke getkeystroke (int delay);
Keystroke getkey (int delay);
unsigned getkey_unfiltered (int delay);
void waitkey (void);
void pushkey (Keystroke key);
void ungetkey (Keystroke key);

/*--------------------------------------------------------------------------
 * Terminal (term.c, or gui.c for the graphical frontend).
 *--------------------------------------------------------------------------*/

#define FONT_NORMAL 0000
#define FONT_REVERSE 0001
#define FONT_UNDERLINE 0002

typedef struct {
	bool has_colors;
	bool supports_default_colors;
	bool supports_reverse;
	bool supports_underline;
	int color_count;
	bool truecolor;		/* Colours may be VCOLOR_RGB values. */
	bool gui;		/* A graphical window rather than a terminal. */
} TerminalCapabilities;

#define TERM_COLOR_UNSPECIFIED -2
#define TERM_COLOR_DEFAULT -1
#define TERM_COLOR_BLACK 0
#define TERM_COLOR_WHITE 7

typedef struct {
	int foreground;
	int background;
	int default_foreground_fallback;
	int default_background_fallback;
	bool bold;
	bool underline;
	bool reverse;
	bool has_foreground;
	bool has_background;
} TerminalStyle;

void term_init (void);
void term_close (void);
bool term_initialized (void);
bool term_input_eof (void);
void term_write_raw (const char *s, size_t len);
size_t term_buf_len (void);
void term_move (size_t y, size_t x);
void term_clrtoeol (void);
void term_refresh (void);
void term_clear (void);
void term_addch (char c);
/* `n' blank cells, which xzile-gui may cover with a preview. */
void term_add_picture (size_t n);
void term_addstr (const char *s);
void term_addnstr (const char *s, size_t n);
void term_attrset (size_t attr);
void term_apply_style (const TerminalStyle *style);
void term_apply_face (const char *face_name);
void term_beep (void);
size_t term_width (void);
size_t term_height (void);
const TerminalCapabilities *term_get_capabilities (void);
Keystroke term_getkey (int delay);
unsigned term_getkey_unfiltered (int delay);
void term_ungetkey (Keystroke key);
char *term_take_pending_paste (void);
void term_suspend (void);
/* The system clipboard, for a GUI: setting returns false and getting
   NULL when the frontend has no clipboard of its own. */
bool term_clipboard_set (const char *text, size_t len);
char *term_clipboard_get (size_t *len);
/* The cell of the last mouse event; false if there is no mouse. */
bool term_mouse_position (size_t *y, size_t *x);

/*--------------------------------------------------------------------------
 * Terminal emulator (vterm.c).
 *--------------------------------------------------------------------------*/

typedef struct VTerm VTerm;

#define VCOLOR_DEFAULT -1
#define VCOLOR_RGB(r, g, b) (0x1000000 | ((r) << 16) | ((g) << 8) | (b))

enum {
	VATTR_BOLD = 1,
	VATTR_DIM = 2,
	VATTR_ITALIC = 4,
	VATTR_UNDERLINE = 8,
	VATTR_REVERSE = 16,
	VATTR_INVISIBLE = 32,
	VATTR_STRIKE = 64,
};

typedef struct {
	int32_t fg, bg;		/* Palette index, VCOLOR_RGB value, or VCOLOR_DEFAULT. */
	uint16_t flags;
} VAttr;

typedef struct {
	uint32_t cp;		/* Code point; 0 for the right half of a wide char. */
	uint8_t width;		/* 1, 2 for a wide char, 0 for its right half. */
	VAttr attr;
} VCell;

/* Called to send replies (e.g. cursor position reports) to the program. */
typedef void (*VTermWriteFn) (const char *s, size_t len, void *data);
/* Called for OSC sequences, e.g. code 7 with the working directory. */
typedef void (*VTermOscFn) (int code, const char *text, void *data);

VTerm *vterm_new (int rows, int cols, VTermWriteFn write, VTermOscFn osc_fn, void *data);
void vterm_free (VTerm *t);
void vterm_write (VTerm *t, const char *s, size_t len);
void vterm_resize (VTerm *t, int rows, int cols);
int vterm_rows (const VTerm *t);
int vterm_cols (const VTerm *t);
const VCell *vterm_cell (const VTerm *t, int y, int x);
void vterm_cursor (const VTerm *t, int *y, int *x);
bool vterm_cursor_visible (const VTerm *t);
bool vterm_app_cursor (const VTerm *t);
bool vterm_bracketed_paste (const VTerm *t);
size_t vterm_scrollback_len (const VTerm *t);
const VCell *vterm_scrollback_line (const VTerm *t, size_t i, size_t *len);

void term_set_vattr (const VAttr *a);
void term_set_cursor_visible (bool visible);
void term_get_cursor (size_t *y, size_t *x);
int color_rgb_to_256 (int red, int green, int blue);
int color_rgb_to_16 (int red, int green, int blue);

/*--------------------------------------------------------------------------
 * Subprocesses (process.c).
 *--------------------------------------------------------------------------*/

typedef struct Process Process;
typedef void (*ProcessOutputFn) (Process *p, const char *data, size_t len, void *owner);
typedef void (*ProcessExitFn) (Process *p, int status, void *owner);

#define MAX_PROCESS_FDS 64

/* Start `argv' in `dir'. `exec_path', if not NULL, is the program to
   run (argv[0] may then differ, e.g. "-zsh"). `env' lists extra
   NAME=VALUE settings. With `use_pty', the process runs on a
   pseudo-terminal of the given size; otherwise its output is a pipe. */
Process *process_start (const char *const *argv, const char *exec_path, const char *dir,
			bool use_pty, int rows, int cols, const char *const *env,
			ProcessOutputFn on_output, ProcessExitFn on_exit, void *owner);
bool process_alive (const Process *p);
pid_t process_pid (const Process *p);
void process_send (Process *p, const char *s, size_t len);
void process_signal (Process *p, int sig);
void process_resize (Process *p, int rows, int cols);
/* A background process, such as a viewer window, is not waited for:
   it does not count for process_any_alive. */
void process_set_background (Process *p);
void process_free (Process *p);
bool process_any_alive (void);
void process_kill_all (void);
struct pollfd;
size_t process_poll_fds (struct pollfd *fds, size_t max);
bool process_handle_events (const struct pollfd *fds, size_t n);
bool process_reap (void);
/* Run `argv' (found on PATH) and wait for it, feeding it `input' if not
   NULL and collecting its standard output and error (either may be
   NULL). `env' lists extra NAME=VALUE settings. Returns the exit status,
   or -1 if it could not be run. Thread-safe. */
int process_run (const char *const *argv, const char *const *env,
		 const char *input, size_t input_len, StrBuf *out, StrBuf *err);

/* Redisplay (term_redisplay.c). */
void term_redisplay (void);
void redisplay_after_output (void);
void term_redraw_cursor (void);
void term_finish (void);

/*--------------------------------------------------------------------------
 * Lisp (lisp.c, eval.c).
 *--------------------------------------------------------------------------*/

/* Arguments passed to a Lisp function. A NULL `Args *` means the
   function was called interactively with a universal argument. Entries
   may themselves be NULL (e.g. nested lists, which are not evaluated). */
typedef struct {
	char **v;
	size_t n, pos;
} Args;

Args *args_new (void);
void args_add (Args *a, const char *s);
void args_free (Args *a);
char *args_poll (Args *a);
size_t args_size (const Args *a);
bool args_empty (const Args *a);

typedef bool (*Function) (long uniarg, Args *args);

typedef struct {
	const char *name;
	Function func;
	bool interactive;
	const char *doc;
} LispFunc;

extern Map *lisp_funcs;

void lisp_func_new (const char *name, Function func, bool interactive, const char *doc);
LispFunc *lisp_func_find (const char *name);

bool funcall (const char *name);
bool funcall_n (const char *name, long uniarg);
bool funcall_args (const char *name, long uniarg, Args *args);
bool noarg (Args *args);
bool bool_arg (Args *args, bool *val);
bool parse_number (const char *s, long *out);
bool int_arg (Args *args, long *n);
bool int_or_uniarg (Args *args, long *n, long uniarg);
void lisp_loadstring (const char *a, size_t len);
bool lisp_loadfile (const char *file);

typedef bool (*MovementFn) (void);
typedef bool (*MovementNFn) (long dir);
bool execute_with_uniarg (long uniarg, MovementFn forward, MovementFn backward);
bool move_with_uniarg (long uniarg, MovementNFn move);
bool execute_function (const char *name, long uniarg, bool is_uniarg);
char *minibuf_read_function_name (const char *fmt, ...) PRINTF_LIKE (1, 2);

/*--------------------------------------------------------------------------
 * Global editor state (main.c).
 *--------------------------------------------------------------------------*/

enum {
	FLAG_NEED_RESYNC = 1 << 0,	/* A resync is required. */
	FLAG_SET_UNIARG = 1 << 1,	/* The last command modified the
					   universal arg variable `uniarg'. */
	FLAG_UNIARG_EMPTY = 1 << 2,	/* Current universal arg is just C-u's with no number. */
	FLAG_DEFINING_MACRO = 1 << 3,	/* We are defining a macro. */
};

extern int thisflag, lastflag;
extern long last_uniarg;
extern char *program_name;

#define EXIT_SIGNAL 2
#define EXIT_COREDUMP 3
NORETURN void zile_exit (int reason);

/*--------------------------------------------------------------------------
 * Buffers, markers, regions, undo (buffer.c, marker.c, undo.c).
 *--------------------------------------------------------------------------*/

typedef struct Buffer Buffer;
typedef struct Window Window;
typedef struct Binding Binding;
typedef struct Dired Dired;
typedef struct Shell Shell;
typedef struct Compilation Compilation;
typedef struct Magit Magit;
typedef struct Preview Preview;
typedef struct Html Html;
typedef struct Mode Mode;
typedef struct CommentStyle CommentStyle;

typedef struct Marker {
	struct Marker *next;	/* Used to chain all markers in the buffer. */
	size_t o;		/* Marker offset within buffer. */
	Buffer *bp;		/* Buffer that marker points into. */
} Marker;

enum UndoType {
	UNDO_START_SEQUENCE,
	UNDO_END_SEQUENCE,
	UNDO_SAVE_BLOCK,
};

typedef struct Undo {
	struct Undo *next;
	enum UndoType type;	/* The type of undo delta. */
	size_t o;		/* Buffer offset of the undo delta. */
	bool unchanged;		/* Flag indicating that reverting this undo leaves
				   the buffer in an unchanged state. */
	Estr *text;		/* Old text. */
	size_t size;		/* Size of replacement text. */
} Undo;

/* What the visited file looked like when last read or written. */
typedef struct {
	bool known;
	struct timespec mtime;
	off_t size;
	dev_t dev;
	ino_t ino;
} FileStamp;

struct Buffer {
	char *name;		/* The name of the buffer. */
	char *filename;		/* The file being edited. */
	FileStamp disk;		/* The visited file when last read or written. */
	bool disk_warned;	/* The user was told the file changed on disk. */
	Buffer *next;		/* Next buffer in buffer list. */
	size_t goalc;		/* Goal column for previous/next-line commands. */
	Marker *mark;		/* The mark. */
	Marker *markers;	/* Markers list (updated whenever text is changed). */
	Undo *last_undop;	/* Most recent undo delta (owns the list). */
	Undo *next_undop;	/* Next undo delta to apply. */
	Map *vars;		/* Buffer-local variables. */
	bool modified;		/* Modified flag. */
	bool nosave;		/* The buffer need not be saved. */
	bool needname;		/* On save, ask for a file name. */
	bool temporary;		/* The buffer is a temporary buffer. */
	bool readonly;		/* The buffer cannot be modified. */
	bool backup;		/* The old file has already been backed up. */
	bool noundo;		/* Do not record undo informations. */
	bool autofill;		/* The buffer is in Auto Fill mode. */
	bool isearch;		/* The buffer is in Isearch loop. */
	bool mark_active;	/* The mark is active. */
	char *dir;		/* The default directory. */

	Binding *keymap;	/* Buffer-local key bindings, or NULL. */
	Dired *dired;		/* Directory editor state, or NULL. */
	Shell *shell;		/* Terminal emulator state, or NULL. */
	Compilation *compilation; /* Compilation state, or NULL. */
	Magit *magit;		/* Git interface state, or NULL. */
	Preview *preview;	/* Image or PDF preview state, or NULL. */
	Html *html;		/* HTML preview window state, or NULL. */
	char *mode_label;	/* Mode line name overriding the highlighter's. */

	const Mode *highlighter;
	int *syntax_states;	/* Parser state at the start of each line. */
	size_t syntax_states_cap;
	size_t syntax_valid_upto;
	size_t syntax_valid_offset;

	size_t *line_checkpoints;
	size_t line_checkpoints_n, line_checkpoints_cap;
	size_t line_indexed_upto_line;
	size_t line_indexed_upto_offset;

	size_t pt_;		/* The point. */
	Estr *text;		/* The text. */
	size_t gap;		/* Size of gap after point. */
};

typedef struct {
	size_t start, end;
} Region;

extern Buffer *cur_bp, *head_bp;

Buffer *buffer_new (Estr *es);
void buffer_init (Buffer *bp);
void buffer_set_highlighter (Buffer *bp, const Mode *mode);
size_t buffer_pt (const Buffer *bp);
EstrView buffer_pre_point (const Buffer *bp);
EstrView buffer_post_point (const Buffer *bp);
size_t buffer_length (const Buffer *bp);
const char *buffer_eol (const Buffer *bp);
size_t buffer_line_len (Buffer *bp, size_t o);
char buffer_get_char (const Buffer *bp, size_t o);
uint32_t buffer_get_utf8_char (const Buffer *bp, size_t o, size_t *char_len);
size_t buffer_prev_line (const Buffer *bp, size_t o);
size_t buffer_next_line (const Buffer *bp, size_t o);
size_t buffer_start_of_line (const Buffer *bp, size_t o);
size_t buffer_end_of_line (const Buffer *bp, size_t o);
size_t buffer_line_o (const Buffer *bp);
bool buffer_replace_estr (Buffer *bp, size_t del, EstrView es);
bool buffer_insert_estr (Buffer *bp, EstrView es);
Estr *buffer_get_region (const Buffer *bp, Region r);
bool buffer_is_empty_line (Buffer *bp);
bool buffer_is_blank_line (Buffer *bp);
char buffer_following_char (Buffer *bp);
char buffer_preceding_char (Buffer *bp);
bool buffer_bobp (const Buffer *bp);
bool buffer_eobp (const Buffer *bp);
bool buffer_bolp (const Buffer *bp);
bool buffer_eolp (Buffer *bp);
bool buffer_insert_char (Buffer *bp, char c);
bool buffer_delete_char (Buffer *bp);
bool buffer_backward_delete_char (Buffer *bp);
void buffer_insert_buffer (Buffer *bp, Buffer *src);
const char *buffer_get_filename_or_name (const Buffer *bp);
const char *buffer_mode_name (const Buffer *bp);
const CommentStyle *buffer_get_comment_style (const Buffer *bp);
void buffer_set_names (Buffer *bp, const char *filename);
const char *buffer_get_variable (Buffer *bp, const char *name);
Buffer *buffer_find (const char *name);
char *buffer_unique_name (const char *base);
bool buffer_is_live (const Buffer *bp);
void buffer_switch_to (Buffer *bp, bool reorder);
bool buffer_warn_if_readonly (Buffer *bp);
bool buffer_warn_if_no_mark (Buffer *bp);
void buffer_set_temporary (Buffer *bp);
size_t buffer_tab_width (Buffer *bp);
void buffer_kill (Buffer *bp);
bool buffer_check_modified (Buffer *bp);
bool buffer_move_char (Buffer *bp, long offset);
size_t buffer_calculate_goalc (Buffer *bp, size_t o);
void buffer_goto_goalc (Buffer *bp);
bool buffer_move_line (Buffer *bp, long n);
bool buffer_move_word (Buffer *bp, long dir);
bool buffer_move_sexp (Buffer *bp, long dir);
size_t buffer_offset_to_line (Buffer *bp, size_t offset);
void buffer_goto_offset (Buffer *bp, size_t o);
int buffer_get_line_start_state (Buffer *bp, size_t line_idx);
Buffer *create_auto_buffer (const char *name);
Buffer *create_scratch_buffer (void);
struct Completion *buffer_make_completion (void);
void buffer_make_recent (Buffer *bp);

/* Movement callbacks bound to `cur_bp'. */
bool cur_move_char (long dir);
bool cur_move_line (long dir);
bool cur_move_word (long dir);
bool cur_move_sexp (long dir);
bool cur_delete_char (void);
bool cur_backward_delete_char (void);

Region region_new (size_t o1, size_t o2);
size_t region_size (Region r);
Region region_calculate (void);
bool region_delete (Region r);
bool region_contains (Region r, size_t o);

void marker_unchain (Marker *m);
void marker_free (Marker *m);
void marker_move (Marker *m, Buffer *bp, size_t o);
Marker *marker_copy (const Marker *m);
Marker *marker_point (void);
void push_mark (void);
void pop_mark (void);
void set_mark (void);

void undo_start_sequence (void);
void undo_end_sequence (void);
void undo_save_block_in (Buffer *bp, size_t o, size_t osize, size_t size);
void undo_set_unchanged (Undo *l);
void undo_free_all (Buffer *bp);
void undo_init (void);

/*--------------------------------------------------------------------------
 * Windows and layout (window.c, layout.c).
 *--------------------------------------------------------------------------*/

/* A place a window has shown: point in a buffer, and how far down the
   window that point was. */
typedef struct {
	Marker *pt;
	size_t topdelta;
} WinPlace;

typedef struct {
	WinPlace *v;		/* Oldest first. */
	size_t n, cap;
} WinPlaces;

struct Window {
	Window *next;		/* The next window in window list. */
	unsigned long id;	/* Distinguishes windows reusing an address. */
	WinPlaces back;		/* Buffers previously shown, for previous-buffer. */
	WinPlaces fwd;		/* Buffers left by previous-buffer, for next-buffer. */
	Buffer *bp;		/* The buffer displayed in window. */
	size_t topdelta;	/* The top line delta from point. */
	size_t start_column;	/* The start column of the window (>0 if scrolled
				   sideways). */
	Marker *saved_pt;	/* The point (used to hold the point in non-current
				   windows). */
	size_t fwidth;		/* The formal width and height of the window. */
	size_t fheight;
	size_t ewidth;		/* The effective width and height of the window. */
	size_t eheight;
	size_t xpos;
	size_t gutter_width;	/* Width of the line-number gutter (0 if disabled). */
	bool all_displayed;	/* The bottom of the buffer is visible */
	size_t lastpointn;	/* The last point line number. */
};

extern Window *cur_wp, *head_wp;

Window *window_find (const char *name);

/* winhist.c */
void winhist_before_command (void);
void winhist_after_command (void);
void winhist_copy (Window *to, const Window *from);
void winhist_free (Window *wp);
Buffer *winhist_replacement (Window *wp, const Buffer *dying, size_t *o, size_t *topdelta);
void winhist_init (void);
size_t window_o (const Window *wp);
bool window_top_visible (Window *wp);
bool window_bottom_visible (const Window *wp);
void window_resync (Window *wp);
void window_set_current (Window *wp);
void wm_delete (Window *wp);
void create_scratch_window (void);
Window *popup_window (void);
Window *window_showing (const Buffer *bp);
void window_show_buffer (Window *wp, Buffer *bp, size_t o);
void window_init (void);

enum SplitKind { SPLIT_ROWS, SPLIT_COLS };

typedef struct WinNode {
	struct WinNode *parent;
	bool is_leaf;
	size_t x, y, width, height;
	/* Split node. */
	enum SplitKind kind;
	struct WinNode *left_or_top, *right_or_bottom;
	double ratio;
	/* Leaf node. */
	Window *wp;
} WinNode;

extern WinNode *root_node;

typedef void (*LeafFn) (WinNode *leaf, void *data);
WinNode *leaf_node_new (Window *wp);
void winnode_each_leaf (WinNode *n, LeafFn fn, void *data);
WinNode *find_leaf_for (Window *wp);
void replace_leaf_with_split (WinNode *leaf, enum SplitKind kind, Window *w1, Window *w2);
void remove_leaf_and_promote_sibling (WinNode *leaf);
Window *update_windows_geometry (size_t x, size_t y, size_t w, size_t h);
bool resize_window_layout (Window *wp, int delta, bool vertical);
unsigned long layout_generation (void);
WinNode *layout_divider_at (size_t y, size_t x);
bool layout_resize_split (WinNode *split, size_t size);

/* redisplay.c */
size_t get_main_window_height (void);
void resize_windows (void);
void recenter (Window *wp);
void redisplay_init (void);

/*--------------------------------------------------------------------------
 * Minibuffer, completion, history (minibuf.c, completion.c, history.c).
 *--------------------------------------------------------------------------*/

typedef struct {
	StrVec elements;
	long sel;		/* Selected element, or -1. */
} History;

History *history_new (void);
void history_add_element (History *hp, const char *s);
void history_prepare (History *hp);
const char *history_previous_element (History *hp);
const char *history_next_element (History *hp);

enum CompletionCode {
	COMPLETION_NOTMATCHED,
	COMPLETION_MATCHED,
	COMPLETION_MATCHEDNONUNIQUE,
	COMPLETION_NONUNIQUE,
};

enum {
	COMPLETION_POPPEDUP = 1 << 0,	/* Completion window has been popped up. */
	COMPLETION_CLOSE = 1 << 1,	/* The completion window should be closed. */
	COMPLETION_FILENAME = 1 << 2,	/* This is a filename completion. */
};

typedef struct Completion {
	char *match;		/* The match buffer. */
	Buffer *old_bp;		/* The buffer the popup window showed before. */
	Marker *old_pt;		/* Its point in that window. */
	StrVec completions;	/* The completions list (sorted). */
	StrVec matches;		/* The matches list (sorted). */
	size_t matchsize;	/* The match buffer size. */
	int flags;		/* Completion flags. */
	char *path;		/* Path for a filename completion. */
} Completion;

Completion *completion_new (bool fileflag);
void completion_free (Completion *cp);
void completion_scroll_up (void);
void completion_scroll_down (void);
enum CompletionCode completion_try (Completion *cp, const char *search, bool popup_when_complete);

void minibuf_init (void);
bool minibuf_no_error (void);
bool minibuf_showing_error (void);
void minibuf_refresh (void);
void minibuf_write (const char *fmt, ...) PRINTF_LIKE (1, 2);
void minibuf_error (const char *fmt, ...) PRINTF_LIKE (1, 2);
char *minibuf_read (const char *fmt, const char *value, ...) PRINTF_LIKE (1, 3);
long minibuf_read_number (const char *fmt, ...) PRINTF_LIKE (1, 2);
char *minibuf_read_filename (const char *fmt, const char *value, const char *file, ...) PRINTF_LIKE (1, 4);
char *minibuf_read_file_to_visit (const char *fmt, const char *value, const char *file, ...) PRINTF_LIKE (1, 4);
int minibuf_read_yesno (const char *fmt, ...) PRINTF_LIKE (1, 2);
int minibuf_read_y_or_n (const char *fmt, ...) PRINTF_LIKE (1, 2);
char *minibuf_read_completion (const char *fmt, const char *val, Completion *cp, History *hp, ...) PRINTF_LIKE (1, 5);
char *minibuf_vread_completion (const char *fmt, const char *val, Completion *cp, History *hp,
				const char *empty_err, const char *invalid_err, va_list ap);
void minibuf_clear (void);
size_t minibuf_get_height (void);

/* term_minibuf.c */
void term_minibuf_write (const char *s);
bool term_minibuf_active (void);
void term_minibuf_enter (void);
void term_minibuf_leave (void);
void minibuf_set_reserved_height (size_t h);

/*--------------------------------------------------------------------------
 * Picker (picker.c, selectors.c).
 *--------------------------------------------------------------------------*/

typedef struct Picker Picker;

typedef struct {
	char *text;		/* Matched, shown and returned. */
	char *annotation;
	int group;
	void *data;
} PickerItem;

typedef struct {
	const char *prompt;
	const char *initial;		/* Initial input. */
	const char *const *groups;	/* Group titles, indexed by item group; or NULL. */
	bool require_match;		/* RET must choose a candidate. */
	bool preselect;			/* Preselect strong matches. */
	unsigned fuzzy_groups;		/* Groups whose fuzzy matches are preselected too. */
	bool file_mode;			/* The input is a path; list its directory. */
	bool other_window;		/* C-o chooses for the other window. */
	int default_item;		/* Selected with empty input, or -1. */
	History *history;
	/* Called when the selection changes. */
	void (*preview) (PickerItem *it, void *data);
	/* C-k on a candidate; returns true if it should be removed. */
	bool (*kill) (PickerItem *it, void *data);
	/* Run in a thread to add candidates with picker_add_async. */
	void (*async) (Picker *p, void *data);
	/* Filters asynchronous candidates, on the main thread. */
	bool (*accept_async) (const char *text, void *data);
	void *data;
} PickerSpec;

enum PickerResult { PICK_CANCEL, PICK_ITEM, PICK_INPUT };

typedef struct {
	enum PickerResult kind;
	int item;		/* For PICK_ITEM. */
	char *input;		/* The input, or for files the chosen path. */
	bool other_window;
} PickerChoice;

Picker *picker_new (const PickerSpec *spec);
void picker_add (Picker *p, const char *text, const char *annotation, int group, void *data);
void picker_add_async (Picker *p, const char *text, const char *annotation, int group);
PickerItem *picker_item (Picker *p, int i);
void picker_run (Picker *p, PickerChoice *choice);
void picker_free (Picker *p);

char *picker_read_name (const char *prompt, StrVec *names, StrVec *annotations,
			History *hp, bool require_match, int default_item);
char *picker_read_file (const char *prompt, const char *initial, History *hp, bool visit);
void recentf_add (const char *path);
void recentf_load (void);
void recentf_save (void);
void selectors_init (void);
char *term_minibuf_read (const char *prompt, const char *val, long pos, Completion *cp, History *hp);

/*--------------------------------------------------------------------------
 * Bindings, commands, macros (bind.c, macro.c).
 *--------------------------------------------------------------------------*/

void init_default_bindings (void);
void bindings_by_function (Map *out);
Binding *keymap_new (void);
void keymap_bind (Binding *map, const char *keystr, const char *func_name);
void keymap_set_default (Binding *map, const char *func_name);
void get_key_sequence (KeyVec *keys);
LispFunc *get_function_by_keys (const KeyVec *keys);
Keystroke binding_completion (const char *a);
bool call_command (LispFunc *f, long uniarg, Args *args);
void get_and_run_command (void);
LispFunc *last_command (void);
void set_this_command (LispFunc *cmd);
void bind_init (void);

void add_cmd_to_macro (void);
void add_key_to_cmd (Keystroke key);
void remove_key_from_cmd (void);
void cancel_kbd_macro (void);
void macro_init (void);

/*--------------------------------------------------------------------------
 * Variables (variables.c).
 *--------------------------------------------------------------------------*/

typedef struct {
	char *name;		/* Variable name. */
	char *defval;		/* Default value. */
	char *val;		/* Current value, if any. */
	bool local;		/* If true, becomes local when set. */
	char *doc;		/* Documentation */
} VarEntry;

void init_variables (void);
void set_variable (const char *name, const char *val);
VarEntry *get_variable_entry (Buffer *bp, const char *name);
const char *get_variable_doc (const char *name, const char **defval);
const char *get_variable (const char *name);
bool get_variable_bool (const char *name);
char *minibuf_read_variable_name (const char *fmt, ...) PRINTF_LIKE (1, 2);
void variables_init (void);

/*--------------------------------------------------------------------------
 * Editing commands (basic.c, funcs.c, line.c, killring.c, ...).
 *--------------------------------------------------------------------------*/

void ding (void);
void basic_init (void);

typedef void (*BufferWriter) (void *data);
void write_temp_buffer (const char *name, bool show, BufferWriter func, void *data);
bool iswordchar (char c);
bool iswordcodepoint (uint32_t c);
bool isopenbracketchar (char c, bool single_quote, bool double_quote);
bool isclosebracketchar (char c, bool single_quote, bool double_quote);

enum Case { CASE_UPPER = 1, CASE_LOWER, CASE_CAPITALIZED };
char *recase (const char *s, size_t len, enum Case rcase);
void funcs_init (void);

void bprintf (const char *fmt, ...) PRINTF_LIKE (1, 2);
bool insert_newline (void);
bool intercalate_newline (void);
int fill_break_line (void);
void line_init (void);

extern Estr *kill_ring_text;
void maybe_destroy_kill_ring (void);
void kill_ring_push (EstrView es);
void kill_ring_take_clipboard (void);

/* paren.c */
bool paren_match (Buffer *bp, size_t pt, size_t *here, size_t *there, bool *mismatch);

/* clipboard.c */
void clipboard_changed (void);
void clipboard_flush (void);
char *clipboard_fetch (size_t *len);
void killring_init (void);

char *expand_path (const char *path);
char *compact_path (const char *path);
bool find_file (const char *filename);
void buffer_note_disk (Buffer *bp);
bool buffers_check_disk (bool all);
int write_to_disk (Buffer *bp, const char *filename, mode_t mode);
bool file_copy (const char *from, const char *to);
void file_init (void);

/* terminal.c */
bool shell_char_mode (const Buffer *bp);
bool shell_process_alive (const Buffer *bp);
bool shell_draw (Window *wp, size_t topline, size_t leftcol);
bool shell_cursor (Window *wp, size_t *y, size_t *x);
void shell_send_paste (const char *text);
void shell_free (Buffer *bp);
void terminal_init (void);

/* compile.c */
void compilation_free (Buffer *bp);
bool compilation_process_alive (const Buffer *bp);
void compile_init (void);

/* magit.c */
void magit_free (Buffer *bp);
bool magit_buffer_is_transient (const Buffer *bp);
void magit_init (void);

/* dired.c */
bool dired (const char *dir);
void dired_free (Buffer *bp);
void dired_init (void);

/*--------------------------------------------------------------------------
 * Image, PDF and HTML modes (modes/).
 *--------------------------------------------------------------------------*/

/* Previews of images and PDFs, drawn in their windows by xzile-gui
   (modes/preview.c). */

/* A preview buffer for `filename', if it is a binary file to show as
   a preview rather than as text; or NULL. */
Buffer *preview_visit (const char *filename);
/* Whether the buffer is shown as a preview rather than as text. */
bool preview_showing (const Buffer *bp);
bool preview_draw (Window *wp, size_t topline, size_t leftcol);
void preview_begin_redisplay (void);
/* The status line text for a window showing a preview, or NULL. */
char *preview_status (Window *wp);
/* Whether saving `bp' would overwrite a file with what is not its
   text; if so, says so. */
bool preview_refuse_save (Buffer *bp);
/* Reload a preview whose file changed. Returns true if the buffer has
   no text to revert. */
bool preview_follow_disk (Buffer *bp);
void preview_free (Buffer *bp);
/* For the mouse: whether cell `y', `x' shows a preview, and moving or
   zooming it by pixels. `px', `py' are relative to cell 0, 0. */
bool preview_at (size_t y, size_t x);
bool preview_pointer_scroll (size_t y, size_t x, double dx, double dy, bool turn_pages);
bool preview_pointer_zoom (size_t y, size_t x, double px, double py, double factor);
void preview_init (void);
void image_mode_init (void);
void pdf_mode_init (void);

/* HTML, edited as text and shown by a browser engine (modes/html.c). */

/* Choose html-mode for an HTML file just visited. */
void html_after_visit (Buffer *bp);
void html_free (Buffer *bp);
void html_mode_init (void);

void search_init (void);
void shell_init (void);
void registers_init (void);
void help_init (void);
void eval_init (void);
void lisp_init (void);

/*--------------------------------------------------------------------------
 * Regular expressions (regex.c).
 *--------------------------------------------------------------------------*/

typedef struct Regex Regex;

/* Compile `pattern' (Emacs syntax unless `plain'); on error return NULL
   and set `*err' to a static message. */
Regex *regex_compile (const char *pattern, size_t len, bool plain, bool icase, const char **err);
void regex_free (Regex *re);
/* Search `text' for a match. Forward searches find the leftmost match;
   backward searches find the match starting nearest the end. `before'
   and `after' are the bytes adjoining the text, or -1 at the start or
   end of the buffer. Returns the match start, -1 if there is none, or
   REGEX_TOO_COMPLEX if matching was abandoned; stores the match end in
   `*end'. */
#define REGEX_TOO_COMPLEX -2
long regex_search (const Regex *re, const char *text, size_t len, bool forward,
		   int before, int after, size_t *end);

/*--------------------------------------------------------------------------
 * Themes and faces (theme.c).
 *--------------------------------------------------------------------------*/

#define FACE_DEFAULT "default"
#define FACE_REGION "region"
#define FACE_MODE_LINE "mode-line"
#define FACE_MODE_LINE_INACTIVE "mode-line-inactive"
#define FACE_MINIBUFFER_PROMPT "minibuffer-prompt"
#define FACE_VERTICAL_BORDER "vertical-border"
#define FACE_LINE_NUMBER "line-number"
#define FACE_LINE_NUMBER_CURRENT_LINE "line-number-current-line"
#define FACE_TRAILING_WHITESPACE "trailing-whitespace"
#define FACE_ISEARCH "isearch"
#define FACE_LAZY_HIGHLIGHT "lazy-highlight"
#define FACE_MATCH "match"
#define FACE_ERROR "error"
#define FACE_WARNING "warning"
#define FACE_SUCCESS "success"
#define FACE_FONT_LOCK_COMMENT "font-lock-comment-face"
#define FACE_FONT_LOCK_STRING "font-lock-string-face"
#define FACE_FONT_LOCK_KEYWORD "font-lock-keyword-face"
#define FACE_FONT_LOCK_FUNCTION_NAME "font-lock-function-name-face"
#define FACE_FONT_LOCK_VARIABLE_NAME "font-lock-variable-name-face"
#define FACE_FONT_LOCK_TYPE "font-lock-type-face"
#define FACE_FONT_LOCK_CONSTANT "font-lock-constant-face"
#define FACE_FONT_LOCK_BUILTIN "font-lock-builtin-face"
#define FACE_FONT_LOCK_PREPROCESSOR "font-lock-preprocessor-face"
#define FACE_FONT_LOCK_WARNING "font-lock-warning-face"
#define FACE_MAGIT_SECTION_HEADING "magit-section-heading"
#define FACE_MAGIT_DIFF_FILE_HEADING "magit-diff-file-heading"
#define FACE_MAGIT_DIFF_HUNK_HEADING "magit-diff-hunk-heading"
#define FACE_MAGIT_DIFF_ADDED "magit-diff-added"
#define FACE_MAGIT_DIFF_REMOVED "magit-diff-removed"
#define FACE_MAGIT_HASH "magit-hash"
#define FACE_MAGIT_BRANCH_LOCAL "magit-branch-local"
#define FACE_MAGIT_BRANCH_REMOTE "magit-branch-remote"
#define FACE_MAGIT_LOG_DATE "magit-log-date"
#define FACE_PICKER_SELECTION "picker-selection"
#define FACE_PICKER_MATCH "picker-match"
#define FACE_PICKER_ANNOTATION "picker-annotation"
#define FACE_PICKER_GROUP "picker-group"
#define FACE_SHOW_PAREN_MATCH "show-paren-match"
#define FACE_SHOW_PAREN_MISMATCH "show-paren-mismatch"
#define FACE_CURSOR "cursor"

/* Resolve `face_name' in the current theme for the current terminal.
   Returns false for unknown faces. */
bool resolve_terminal_style (const char *face_name, TerminalStyle *style);
void theme_init (void);

/*--------------------------------------------------------------------------
 * Syntax highlighting modes (syntax.c).
 *--------------------------------------------------------------------------*/

#define SYNTAX_STATE_NORMAL 0

struct CommentStyle {
	const char *line_comment_prefix;
	const char *block_comment_open;
	const char *block_comment_close;
	bool prefer_line_comments;
};

/* `face_names' receives one face name (or NULL) per byte of the line. */
typedef int (*ScanLineFn) (const Mode *mode, Buffer *bp, size_t line_idx, size_t start_o,
			   size_t len, int start_state, const char **face_names);

struct Mode {
	const char *name;
	ScanLineFn scan_line;
	const CommentStyle *comment_style;
	const char *const *keywords;
	size_t nkeywords;
};

const Mode *filetype_resolve_highlighter (const char *filename);
void filetype_apply_to_buffer (Buffer *bp);

/*--------------------------------------------------------------------------
 * Debug logging (debug.c).
 *--------------------------------------------------------------------------*/

void debug_set_categories (const char *spec);
void debug_set_log_path (const char *path);
void debug_init_from_env (void);
bool debug_enabled (const char *category);
void debug_log (const char *category, const char *fmt, ...) PRINTF_LIKE (2, 3);
void debug_shutdown (void);

#endif
