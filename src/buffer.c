/* Buffer-oriented functions

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

#include "xzile.h"

#include <assert.h>
#include <ctype.h>
#include <unistd.h>

/* The current buffer; the first buffer in list. */
Buffer *cur_bp = NULL;
Buffer *head_bp = NULL;

#define LINE_INDEX_STRIDE 256

/* Minimum gap size after resize. */
#define MIN_GAP 1024
/* Maximum permitted gap size. */
#define MAX_GAP 4096


/* Line index: the offset of every LINE_INDEX_STRIDE'th line. */

static void
reset_line_index (Buffer *bp)
{
	if (bp->line_checkpoints == NULL) {
		bp->line_checkpoints_cap = 16;
		bp->line_checkpoints = xmalloc (bp->line_checkpoints_cap * sizeof (size_t));
	}
	bp->line_checkpoints[0] = 0;
	bp->line_checkpoints_n = 1;
	bp->line_indexed_upto_line = 0;
	bp->line_indexed_upto_offset = 0;
}

static void
add_line_checkpoint (Buffer *bp, size_t o)
{
	if (bp->line_checkpoints_n == bp->line_checkpoints_cap) {
		bp->line_checkpoints_cap *= 2;
		bp->line_checkpoints = xrealloc (bp->line_checkpoints,
						 bp->line_checkpoints_cap * sizeof (size_t));
	}
	bp->line_checkpoints[bp->line_checkpoints_n++] = o;
}

static void
ensure_line_index_to_offset (Buffer *bp, size_t offset)
{
	size_t o = bp->line_indexed_upto_offset;
	size_t line = bp->line_indexed_upto_line;
	while (o < offset) {
		size_t next_o = buffer_next_line (bp, o);
		if (next_o == NOPOS)
			break;
		o = next_o;
		line++;
		if ((line % LINE_INDEX_STRIDE) == 0)
			add_line_checkpoint (bp, o);
	}

	bp->line_indexed_upto_offset = o;
	bp->line_indexed_upto_line = line;
}

static size_t
find_line_checkpoint_index (Buffer *bp, size_t offset)
{
	ensure_line_index_to_offset (bp, offset);

	for (size_t i = bp->line_checkpoints_n - 1; i > 0; i--)
		if (bp->line_checkpoints[i] <= offset)
			return i;
	return 0;
}

static void
invalidate_line_index_at (Buffer *bp, size_t offset)
{
	size_t line_idx = buffer_offset_to_line (bp, offset);
	size_t checkpoint_line = (line_idx / LINE_INDEX_STRIDE) * LINE_INDEX_STRIDE;
	size_t checkpoint_idx = checkpoint_line / LINE_INDEX_STRIDE;

	if (bp->line_checkpoints_n > checkpoint_idx + 1)
		bp->line_checkpoints_n = checkpoint_idx + 1;

	bp->line_indexed_upto_line = checkpoint_line;
	bp->line_indexed_upto_offset = bp->line_checkpoints[checkpoint_idx];
}

size_t
buffer_offset_to_line (Buffer *bp, size_t offset)
{
	size_t checkpoint_idx = find_line_checkpoint_index (bp, offset);
	size_t n = checkpoint_idx * LINE_INDEX_STRIDE;
	size_t o = bp->line_checkpoints[checkpoint_idx];
	for (; buffer_end_of_line (bp, o) < offset; o = buffer_next_line (bp, o))
		n++;
	return n;
}


/* Syntax state cache: the parser state at the start of each line, valid
   for lines [0, syntax_valid_upto]. */

static void
reset_syntax_cache (Buffer *bp)
{
	free (bp->syntax_states);
	bp->syntax_states = NULL;
	bp->syntax_states_cap = 0;
	bp->syntax_valid_upto = 0;
	bp->syntax_valid_offset = 0;
}

void
buffer_set_highlighter (Buffer *bp, const Mode *mode)
{
	if (bp->highlighter == mode)
		return;
	bp->highlighter = mode;
	reset_syntax_cache (bp);
}

static void
set_syntax_state (Buffer *bp, size_t line, int state)
{
	if (line >= bp->syntax_states_cap) {
		size_t cap = bp->syntax_states_cap ? bp->syntax_states_cap : 64;
		while (cap <= line)
			cap *= 2;
		bp->syntax_states = xrealloc (bp->syntax_states, cap * sizeof (int));
		bp->syntax_states_cap = cap;
	}
	bp->syntax_states[line] = state;
}

static void
update_syntax_cache (Buffer *bp, size_t target_line)
{
	int state = bp->syntax_states[bp->syntax_valid_upto];
	size_t o = bp->syntax_valid_offset;
	const char **dummy_faces = NULL;
	size_t dummy_len = 0;

	size_t i;
	for (i = bp->syntax_valid_upto; i < target_line; i++) {
		size_t next_o = buffer_next_line (bp, o);
		if (next_o == NOPOS)
			break;

		size_t line_len = buffer_end_of_line (bp, o) - buffer_start_of_line (bp, o);
		if (dummy_faces == NULL || dummy_len < line_len) {
			dummy_len = line_len + 128;
			dummy_faces = xrealloc (dummy_faces, dummy_len * sizeof (char *));
		}

		state = bp->highlighter->scan_line (bp->highlighter, bp, i, o, line_len, state, dummy_faces);
		o = next_o;
		set_syntax_state (bp, i + 1, state);
	}
	free (dummy_faces);

	bp->syntax_valid_upto = i;
	bp->syntax_valid_offset = o;
}

int
buffer_get_line_start_state (Buffer *bp, size_t line_idx)
{
	if (bp->highlighter == NULL)
		return SYNTAX_STATE_NORMAL;

	if (bp->syntax_states == NULL) {
		set_syntax_state (bp, 0, SYNTAX_STATE_NORMAL);
		bp->syntax_valid_upto = 0;
		bp->syntax_valid_offset = 0;
	}

	if (line_idx > bp->syntax_valid_upto)
		update_syntax_cache (bp, line_idx);

	if (line_idx <= bp->syntax_valid_upto)
		return bp->syntax_states[line_idx];

	return SYNTAX_STATE_NORMAL;
}

static void
invalidate_syntax_cache_at (Buffer *bp, size_t o)
{
	if (bp->highlighter == NULL || bp->syntax_states == NULL)
		return;

	size_t line_idx = buffer_offset_to_line (bp, o);
	if (line_idx < bp->syntax_valid_upto) {
		bp->syntax_valid_upto = line_idx;
		bp->syntax_valid_offset = buffer_start_of_line (bp, o);
	}
}


/* The point, and gap movement. */

size_t
buffer_pt (const Buffer *bp)
{
	return bp->pt_;
}

static void
set_pt (Buffer *bp, size_t value)
{
	size_t pt = bp->pt_, gap = bp->gap;
	if (value < pt) {
		estr_move (bp->text, value + gap, value, pt - value);
		estr_set (bp->text, value, '\0', MIN (pt - value, gap));
	} else if (value > pt) {
		estr_move (bp->text, pt, pt + gap, value - pt);
		estr_set (bp->text, value + gap - MIN (value - pt, gap), '\0', MIN (value - pt, gap));
	}
	bp->pt_ = value;
	bp->goalc = buffer_calculate_goalc (bp, value);
}

/*
 * Allocate a new buffer structure, set the default local
 * variable values, and insert it into the buffer list.
 */
Buffer *
buffer_new (Estr *es)
{
	Buffer *bp = xzalloc (sizeof (Buffer));
	bp->text = es != NULL ? es : estr_new (eol_lf);
	bp->dir = get_current_dir ();
	reset_line_index (bp);

	/* Insert into buffer list. */
	bp->next = head_bp;
	head_bp = bp;

	buffer_init (bp);
	return bp;
}

/*
 * Initialise a buffer
 */
void
buffer_init (Buffer *bp)
{
	if (get_variable_bool ("auto-fill-mode"))
		bp->autofill = true;
}


/* Buffer methods that know about the gap. */

EstrView
buffer_pre_point (const Buffer *bp)
{
	return ev_substring (estr_view (bp->text), 0, bp->pt_);
}

EstrView
buffer_post_point (const Buffer *bp)
{
	size_t post_gap = bp->pt_ + bp->gap;
	return ev_substring (estr_view (bp->text), post_gap, bp->text->len - post_gap);
}

static size_t
realo_to_o (const Buffer *bp, size_t o)
{
	if (o == NOPOS)
		return o;
	else if (o < bp->pt_ + bp->gap)
		return MIN (o, bp->pt_);
	else
		return o - bp->gap;
}

static size_t
o_to_realo (const Buffer *bp, size_t o)
{
	return o < bp->pt_ ? o : o + bp->gap;
}

size_t
buffer_length (const Buffer *bp)
{
	return realo_to_o (bp, bp->text->len);
}

const char *
buffer_eol (const Buffer *bp)
{
	return bp->text->eol;
}

size_t
buffer_line_len (Buffer *bp, size_t o)
{
	EstrView ev = estr_view (bp->text);
	size_t ro = o_to_realo (bp, o);
	return realo_to_o (bp, ev_end_of_line (ev, ro)) - realo_to_o (bp, ev_start_of_line (ev, ro));
}

char
buffer_get_char (const Buffer *bp, size_t o)
{
	return bp->text->text[o_to_realo (bp, o)];
}

uint32_t
buffer_get_utf8_char (const Buffer *bp, size_t o, size_t *char_len)
{
	size_t len = buffer_length (bp);
	if (o >= len) {
		*char_len = 0;
		return 0;
	}

	char buf[4];
	size_t n = MIN (len - o, sizeof buf);
	for (size_t k = 0; k < n; k++)
		buf[k] = buffer_get_char (bp, o + k);
	return utf8_decode (buf, n, 0, char_len);
}

size_t
buffer_prev_line (const Buffer *bp, size_t o)
{
	return realo_to_o (bp, ev_prev_line (estr_view (bp->text), o_to_realo (bp, o)));
}

size_t
buffer_next_line (const Buffer *bp, size_t o)
{
	return realo_to_o (bp, ev_next_line (estr_view (bp->text), o_to_realo (bp, o)));
}

size_t
buffer_start_of_line (const Buffer *bp, size_t o)
{
	return realo_to_o (bp, ev_start_of_line (estr_view (bp->text), o_to_realo (bp, o)));
}

size_t
buffer_end_of_line (const Buffer *bp, size_t o)
{
	return realo_to_o (bp, ev_end_of_line (estr_view (bp->text), o_to_realo (bp, o)));
}

size_t
buffer_line_o (const Buffer *bp)
{
	return buffer_start_of_line (bp, bp->pt_);
}

/*
 * Replace `del' chars after point with `es'.
 */
bool
buffer_replace_estr (Buffer *bp, size_t del, EstrView es)
{
	if (buffer_warn_if_readonly (bp))
		return false;

	size_t pt = bp->pt_;
	invalidate_line_index_at (bp, pt);
	invalidate_syntax_cache_at (bp, pt);

	size_t newlen = ev_len_with_eol (es, buffer_eol (bp));
	if (!bp->noundo)
		undo_save_block_in (bp, pt, del, newlen);

	/* Adjust gap. */
	size_t oldgap = bp->gap;
	size_t added_gap = oldgap + del < newlen ? MIN_GAP : 0;
	if (added_gap > 0) {
		/* If gap would vanish, open it to MIN_GAP. */
		estr_insert (bp->text, pt, (newlen + MIN_GAP) - (oldgap + del));
		bp->gap = MIN_GAP;
	} else if (oldgap + del > MAX_GAP + newlen) {
		/* If gap would be larger than MAX_GAP, restrict it to MAX_GAP. */
		estr_remove (bp->text, pt + newlen + MAX_GAP, (oldgap + del) - (MAX_GAP + newlen));
		bp->gap = MAX_GAP;
	} else
		bp->gap = oldgap + del - newlen;

	/* Zero any new bit of gap not produced by estr_insert. */
	if (MAX (oldgap, newlen) + added_gap < bp->gap + newlen)
		estr_set (bp->text, pt + MAX (oldgap, newlen) + added_gap, '\0',
			  newlen + bp->gap - MAX (oldgap, newlen) - added_gap);

	/* Insert `newlen' chars, and adjust raw point position. */
	estr_replace (bp->text, pt, es);
	bp->pt_ += newlen;
	bp->goalc = buffer_calculate_goalc (bp, bp->pt_);

	/* Adjust markers. */
	for (Marker *m = bp->markers; m != NULL; m = m->next)
		if (m->o > pt)
			m->o = MAX (pt, m->o + newlen - del);

	bp->modified = true;
	if (ev_next_line (es, 0) != NOPOS)
		thisflag |= FLAG_NEED_RESYNC;
	return true;
}

Estr *
buffer_get_region (const Buffer *bp, Region r)
{
	Estr *es = estr_new (buffer_eol (bp));
	EstrView text = estr_view (bp->text);
	size_t pt = bp->pt_;
	if (r.start < pt)
		estr_cat (es, ev_substring (text, r.start, MIN (r.end, pt) - r.start));
	if (r.end > pt) {
		size_t from = MAX (r.start, pt);
		estr_cat (es, ev_substring (text, bp->gap + from, r.end - from));
	}
	return es;
}


/* Buffer methods that don't know about the gap. */

bool
buffer_insert_estr (Buffer *bp, EstrView es)
{
	return buffer_replace_estr (bp, 0, es);
}

bool
buffer_is_empty_line (Buffer *bp)
{
	return buffer_line_len (bp, bp->pt_) == 0;
}

bool
buffer_is_blank_line (Buffer *bp)
{
	size_t len = buffer_line_len (bp, bp->pt_), lo = buffer_line_o (bp);
	for (size_t i = 0; i < len; i++) {
		char c = buffer_get_char (bp, lo + i);
		if (c != ' ' && c != '\t')
			return false;
	}
	return true;
}

/* Returns the character following point in the current buffer. */
char
buffer_following_char (Buffer *bp)
{
	if (buffer_eobp (bp))
		return 0;
	else if (buffer_eolp (bp))
		return '\n';
	else
		return buffer_get_char (bp, bp->pt_);
}

/* Return the character preceding point in the current buffer. */
char
buffer_preceding_char (Buffer *bp)
{
	if (buffer_bobp (bp))
		return 0;
	else if (buffer_bolp (bp))
		return '\n';
	else
		return buffer_get_char (bp, bp->pt_ - 1);
}

/* Return true if point is at the beginning of the buffer. */
bool
buffer_bobp (const Buffer *bp)
{
	return bp->pt_ == 0;
}

/* Return true if point is at the end of the buffer. */
bool
buffer_eobp (const Buffer *bp)
{
	return bp->pt_ == buffer_length (bp);
}

/* Return true if point is at the beginning of a line. */
bool
buffer_bolp (const Buffer *bp)
{
	return bp->pt_ == buffer_line_o (bp);
}

/* Return true if point is at the end of a line. */
bool
buffer_eolp (Buffer *bp)
{
	return bp->pt_ - buffer_line_o (bp) == buffer_line_len (bp, bp->pt_);
}

/*
 * Insert the character `c' at point.
 */
bool
buffer_insert_char (Buffer *bp, char c)
{
	return buffer_insert_estr (bp, ev_of (&c, 1, eol_lf));
}

bool
buffer_delete_char (Buffer *bp)
{
	bp->mark_active = false;

	if (buffer_eobp (bp)) {
		minibuf_error ("End of buffer");
		return false;
	}

	if (buffer_warn_if_readonly (bp))
		return false;

	if (buffer_eolp (bp)) {
		buffer_replace_estr (bp, strlen (buffer_eol (bp)), ev_empty ());
		thisflag |= FLAG_NEED_RESYNC;
	} else {
		size_t char_len;
		buffer_get_utf8_char (bp, bp->pt_, &char_len);
		buffer_replace_estr (bp, char_len, ev_empty ());
	}
	bp->modified = true;

	return true;
}

bool
buffer_backward_delete_char (Buffer *bp)
{
	bp->mark_active = false;

	if (!buffer_move_char (bp, -1)) {
		minibuf_error ("Beginning of buffer");
		return false;
	}

	buffer_delete_char (bp);
	return true;
}

void
buffer_insert_buffer (Buffer *bp, Buffer *src)
{
	/* Copy text to avoid problems when src == bp. */
	Estr *pre = estr_new (buffer_eol (src));
	estr_cat (pre, buffer_pre_point (src));
	Estr *post = estr_new (buffer_eol (src));
	estr_cat (post, buffer_post_point (src));
	buffer_insert_estr (bp, estr_view (pre));
	buffer_insert_estr (bp, estr_view (post));
	estr_free (pre);
	estr_free (post);
}

/*
 * Get filename, or buffer name if null.
 */
const char *
buffer_get_filename_or_name (const Buffer *bp)
{
	return bp->filename != NULL ? bp->filename : bp->name;
}

const char *
buffer_mode_name (const Buffer *bp)
{
	if (bp->mode_label != NULL)
		return bp->mode_label;
	return bp->highlighter != NULL ? bp->highlighter->name : "Fundamental";
}

const CommentStyle *
buffer_get_comment_style (const Buffer *bp)
{
	return bp->highlighter != NULL ? bp->highlighter->comment_style : NULL;
}

/*
 * Set a new filename, and from it a name, for the buffer.
 */
void
buffer_set_names (Buffer *bp, const char *new_filename)
{
	char *filename;
	if (new_filename[0] != '/') {
		char *cwd = get_current_dir ();
		filename = path_join (cwd, new_filename);
		free (cwd);
	} else
		filename = xstrdup (new_filename);
	free (bp->filename);
	bp->filename = filename;

	filetype_apply_to_buffer (bp);

	char *base = path_basename (filename);
	free (bp->name);
	bp->name = NULL;
	bp->name = buffer_unique_name (base);
	free (base);
}

/*
 * Return `base', or `base<N>' if that names a buffer already.
 */
char *
buffer_unique_name (const char *base)
{
	char *name = xstrdup (base);
	for (size_t i = 2; buffer_find (name) != NULL; i++) {
		free (name);
		name = xasprintf ("%s<%zu>", base, i);
	}
	return name;
}

const char *
buffer_get_variable (Buffer *bp, const char *name)
{
	VarEntry *v = get_variable_entry (bp, name);
	return v != NULL ? v->val : NULL;
}

/*
 * Search for a buffer named `name'.
 */
Buffer *
buffer_find (const char *name)
{
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		if (bp->name != NULL && strcmp (bp->name, name) == 0)
			return bp;
	return NULL;
}

bool
buffer_is_live (const Buffer *bp)
{
	for (Buffer *it = head_bp; it != NULL; it = it->next)
		if (it == bp)
			return true;
	return false;
}

/*
 * Move the given buffer to head.
 */
static void
move_to_head (Buffer *bp)
{
	Buffer *prev = NULL;
	for (Buffer *it = head_bp; it != bp; prev = it, it = it->next)
		;
	if (prev != NULL) {
		prev->next = bp->next;
		bp->next = head_bp;
		head_bp = bp;
	}
}

/* Make `bp' the most recently used buffer. */
void
buffer_make_recent (Buffer *bp)
{
	move_to_head (bp);
}

/*
 * Switch to the specified buffer.
 *
 * If reorder is true, the buffer is moved to the front of the
 * buffer list (MRU).
 */
void
buffer_switch_to (Buffer *bp, bool reorder)
{
	assert (cur_wp->bp == cur_bp);

	/* The buffer is the current buffer; return safely.  */
	if (cur_bp == bp)
		return;

	/* Set current buffer.  */
	cur_bp = bp;
	cur_wp->bp = cur_bp;

	if (reorder)
		move_to_head (bp);

	/* Change to buffer's default directory.  */
	if (chdir (bp->dir) != 0) { /* Ignore error. */ }

	thisflag |= FLAG_NEED_RESYNC;
}

/*
 * Print an error message into the echo area and return true
 * if the current buffer is readonly; otherwise return false.
 */
bool
buffer_warn_if_readonly (Buffer *bp)
{
	if (bp->readonly) {
		minibuf_error ("Buffer is readonly: %s", bp->name);
		return true;
	}
	return false;
}

bool
buffer_warn_if_no_mark (Buffer *bp)
{
	if (bp->mark == NULL) {
		minibuf_error ("The mark is not set now");
		return true;
	} else if (!bp->mark_active) {
		minibuf_error ("The mark is not active now");
		return true;
	}
	return false;
}

/*
 * Set the specified buffer temporary flag and move the buffer
 * to the end of the buffer list.
 */
void
buffer_set_temporary (Buffer *bp)
{
	bp->temporary = true;

	if (bp == head_bp) {
		if (head_bp->next == NULL)
			return;
		head_bp = head_bp->next;
	} else if (bp->next == NULL)
		return;

	Buffer *it;
	for (it = head_bp; it != NULL; it = it->next)
		if (it->next == bp) {
			it->next = it->next->next;
			break;
		}

	assert (head_bp != NULL);
	for (it = head_bp; it->next != NULL; it = it->next)
		;

	it->next = bp;
	bp->next = NULL;
}

/*
 * Return a safe tab width.
 */
size_t
buffer_tab_width (Buffer *bp)
{
	long res;
	if (!parse_number (buffer_get_variable (bp, "tab-width"), &res) || res < 1)
		res = 8;
	return (size_t) res;
}

static void
buffer_free (Buffer *bp)
{
	/* Detach any markers still pointing here; they may be owned
	   elsewhere (e.g. the mark ring). */
	Marker *own_mark = bp->mark;
	for (Marker *m = bp->markers, *next; m != NULL; m = next) {
		next = m->next;
		m->bp = NULL;
		m->next = NULL;
	}
	free (own_mark);
	dired_free (bp);
	shell_free (bp);
	compilation_free (bp);
	magit_free (bp);
	preview_free (bp);
	html_free (bp);
	free (bp->mode_label);
	undo_free_all (bp);
	if (bp->vars != NULL)
		map_free (bp->vars, NULL);
	estr_free (bp->text);
	free (bp->line_checkpoints);
	free (bp->syntax_states);
	free (bp->name);
	free (bp->filename);
	free (bp->dir);
	free (bp);
}

/*
 * Remove the buffer from the buffer list and deallocate
 * its space.  Recreate the scratch buffer when required.
 */
void
buffer_kill (Buffer *bp)
{
	Buffer *next_bp;
	if (bp->next != NULL)
		next_bp = bp->next;
	else
		next_bp = head_bp == bp ? NULL : head_bp;

	/* Windows displaying the buffer to kill go back to the buffer
	   they showed before it, or else to the next buffer. */
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (wp->bp == bp) {
			size_t o, topdelta;
			Buffer *prev = winhist_replacement (wp, bp, &o, &topdelta);
			marker_free (wp->saved_pt);
			wp->saved_pt = NULL;
			wp->bp = prev != NULL ? prev : next_bp;
			wp->topdelta = 0;
			if (prev != NULL) {
				o = MIN (o, buffer_length (prev));
				if (wp == cur_wp)
					buffer_goto_offset (prev, o);
				else {
					wp->saved_pt = xzalloc (sizeof (Marker));
					marker_move (wp->saved_pt, prev, o);
				}
				size_t line = buffer_offset_to_line (prev, o);
				wp->topdelta = MIN (MIN (topdelta, line), wp->eheight > 0 ? wp->eheight - 1 : 0);
				wp->lastpointn = line;
			}
		}

	/* Remove the buffer from the buffer list. */
	if (cur_bp == bp)
		cur_bp = cur_wp->bp;
	if (head_bp == bp)
		head_bp = head_bp->next;
	for (Buffer *it = head_bp; it != NULL && it->next != NULL; it = it->next)
		if (it->next == bp) {
			it->next = it->next->next;
			break;
		}

	buffer_free (bp);

	/* If no buffers left, recreate scratch buffer and point windows at
	   it. */
	if (next_bp == NULL) {
		cur_bp = head_bp = next_bp = create_scratch_buffer ();
		for (Window *wp = head_wp; wp != NULL; wp = wp->next)
			wp->bp = head_bp;
	}

	/* Resync windows that need it. */
	for (Window *wp = head_wp; wp != NULL; wp = wp->next)
		if (wp->bp == next_bp)
			window_resync (wp);
}

/*
 * Check if the buffer has been modified.  If so, asks the user if
 * they want to save the changes.  If the response is positive, return
 * true, else false.
 */
bool
buffer_check_modified (Buffer *bp)
{
	if (!bp->modified || bp->nosave)
		return true;

	int ans = minibuf_read_yesno ("Buffer %s modified; kill anyway? (yes or no) ", bp->name);
	if (ans < 0)
		funcall ("keyboard-quit");
	return ans == 1;
}


/* Basic movement routines */

static void
goto_end_of_line (Buffer *bp)
{
	buffer_goto_offset (bp, buffer_line_o (bp) + buffer_line_len (bp, bp->pt_));
	bp->goalc = NOPOS;
}

/* Return the offset of the character that ends at point, at or after
   the start of its line. */
static size_t
prev_char_o (Buffer *bp)
{
	size_t p = bp->pt_ - 1;
	size_t limit = buffer_line_o (bp);
	while (p > limit && ((unsigned char) buffer_get_char (bp, p) & 0xC0) == 0x80)
		p--;
	size_t char_len;
	buffer_get_utf8_char (bp, p, &char_len);
	return p + char_len == bp->pt_ ? p : bp->pt_ - 1;
}

bool
buffer_move_char (Buffer *bp, long offset)
{
	int dir = offset >= 0 ? 1 : -1;
	unsigned long n = offset >= 0 ? (unsigned long) offset : (unsigned long) -offset;
	for (unsigned long i = 0; i < n; i++) {
		if (dir > 0) {
			if (!buffer_eolp (bp)) {
				size_t char_len;
				buffer_get_utf8_char (bp, bp->pt_, &char_len);
				set_pt (bp, bp->pt_ + char_len);
			} else if (!buffer_eobp (bp)) {
				thisflag |= FLAG_NEED_RESYNC;
				set_pt (bp, bp->pt_ + strlen (buffer_eol (bp)));
				buffer_goto_offset (bp, buffer_line_o (bp));
			} else
				return false;
		} else {
			if (!buffer_bolp (bp))
				set_pt (bp, prev_char_o (bp));
			else if (!buffer_bobp (bp)) {
				thisflag |= FLAG_NEED_RESYNC;
				set_pt (bp, bp->pt_ - strlen (buffer_eol (bp)));
				goto_end_of_line (bp);
			} else
				return false;
		}
	}

	return true;
}

/*
 * Calculate the goal column.
 *
 * Iterates from the start of the line to `o` decoding full UTF-8
 * characters and accumulating display columns: tabs expand to the next
 * tab stop, every other code point contributes its wcwidth.
 */
size_t
buffer_calculate_goalc (Buffer *bp, size_t o)
{
	size_t col = 0, t = buffer_tab_width (bp);
	size_t pos = buffer_start_of_line (bp, o);

	while (pos < o) {
		size_t char_len;
		uint32_t ch = buffer_get_utf8_char (bp, pos, &char_len);
		if (ch == '\t')
			col += t - col % t;
		else
			col += (size_t) utf8_char_display_width (ch);
		if (char_len == 0)
			break;
		pos += char_len;
	}

	return col;
}

/*
 * Move point to the character whose left edge is at the goal column.
 *
 * Walks the current line forward one code point at a time.  Stops
 * before any character whose left edge would meet or exceed `goalc`,
 * or before a wide character that would straddle `goalc`.
 */
void
buffer_goto_goalc (Buffer *bp)
{
	size_t col = 0, t = buffer_tab_width (bp);
	size_t i = buffer_line_o (bp);
	size_t line_end = i + buffer_line_len (bp, bp->pt_);

	while (i < line_end) {
		if (col >= bp->goalc)
			break;

		size_t char_len;
		uint32_t ch = buffer_get_utf8_char (bp, i, &char_len);
		size_t w = ch == '\t' ? t - col % t : (size_t) utf8_char_display_width (ch);

		/* Stop before a character that would push us past goalc. */
		if (col + w > bp->goalc)
			break;

		col += w;
		i += char_len;
	}

	set_pt (bp, i);
}

bool
buffer_move_line (Buffer *bp, long n)
{
	bool forward = n >= 0;
	if (n < 0)
		n = -n;

	size_t save_goalc = bp->goalc;
	for (; n > 0; n--) {
		size_t o = forward ? buffer_next_line (bp, bp->pt_) : buffer_prev_line (bp, bp->pt_);
		if (o == NOPOS)
			break;
		set_pt (bp, o);
	}
	bp->goalc = save_goalc;

	buffer_goto_goalc (bp);
	bp->goalc = save_goalc;
	thisflag |= FLAG_NEED_RESYNC;

	return n == 0;
}

bool
buffer_move_word (Buffer *bp, long dir)
{
	bool gotword = false;
	do {
		for (; !(dir > 0 ? buffer_eolp (bp) : buffer_bolp (bp)); buffer_move_char (bp, dir)) {
			size_t char_o = dir < 0 ? prev_char_o (bp) : bp->pt_;
			size_t char_len;
			uint32_t ch = buffer_get_utf8_char (bp, char_o, &char_len);
			if (iswordcodepoint (ch))
				gotword = true;
			else if (gotword)
				break;
		}
	} while (!gotword && buffer_move_char (bp, dir));
	return gotword;
}

bool
buffer_move_sexp (Buffer *bp, long dir)
{
	bool gotsexp = false;
	bool single_quote = dir < 0, double_quote = single_quote;
	int level = 0;

	for (;;) {
		while (dir > 0 ? !buffer_eolp (bp) : !buffer_bolp (bp)) {
			size_t o = bp->pt_ - (dir < 0 ? 1 : 0);
			char c = buffer_get_char (bp, o);

			/* Skip escaped quotes. */
			if ((c == '\"' || c == '\'') && o > buffer_line_o (bp) &&
			    buffer_get_char (bp, o - 1) == '\\') {
				buffer_move_char (bp, dir);
				/* Treat escaped ' and " like word chars. */
				c = 'a';
			}

			if ((dir > 0 && isopenbracketchar (c, single_quote, double_quote)) ||
			    (dir <= 0 && isclosebracketchar (c, single_quote, double_quote))) {
				if (level == 0 && gotsexp)
					return true;

				level++;
				gotsexp = true;
				if (c == '\"')
					double_quote = !double_quote;
				if (c == '\'')
					single_quote = !single_quote;
			} else if ((dir > 0 && isclosebracketchar (c, single_quote, double_quote)) ||
				   (dir <= 0 && isopenbracketchar (c, single_quote, double_quote))) {
				if (level == 0 && gotsexp)
					return true;

				level--;
				gotsexp = true;
				if (c == '\"')
					double_quote = !double_quote;
				if (c == '\'')
					single_quote = !single_quote;

				if (level < 0) {
					minibuf_error ("Scan error: \"Containing expression ends prematurely\"");
					return false;
				}
			}

			buffer_move_char (bp, dir);

			if (!(isalnum ((unsigned char) c) || c == '$' || c == '_')) {
				if (gotsexp && level == 0) {
					if (!(isopenbracketchar (c, single_quote, double_quote) ||
					      isclosebracketchar (c, single_quote, double_quote)))
						buffer_move_char (bp, -dir);
					return true;
				}
			} else
				gotsexp = true;
		}
		if (gotsexp && level == 0)
			return true;
		if (!buffer_move_line (bp, dir > 0 ? 1 : -1)) {
			if (level != 0)
				minibuf_error ("Scan error: \"Unbalanced parentheses\"");
			break;
		}
		if (dir > 0)
			buffer_goto_offset (bp, buffer_line_o (bp));
		else
			goto_end_of_line (bp);
	}
	return false;
}

void
buffer_goto_offset (Buffer *bp, size_t o)
{
	size_t old_lineo = buffer_line_o (bp);
	set_pt (bp, o);
	if (buffer_line_o (bp) != old_lineo)
		thisflag |= FLAG_NEED_RESYNC;
}


/* Movement callbacks operating on the current buffer. */

bool
cur_move_char (long dir)
{
	return buffer_move_char (cur_bp, dir);
}

bool
cur_move_line (long dir)
{
	return buffer_move_line (cur_bp, dir);
}

bool
cur_move_word (long dir)
{
	return buffer_move_word (cur_bp, dir);
}

bool
cur_move_sexp (long dir)
{
	return buffer_move_sexp (cur_bp, dir);
}

bool
cur_delete_char (void)
{
	return buffer_delete_char (cur_bp);
}

bool
cur_backward_delete_char (void)
{
	return buffer_backward_delete_char (cur_bp);
}


Buffer *
create_auto_buffer (const char *name)
{
	Buffer *bp = buffer_new (NULL);
	bp->name = xstrdup (name);
	bp->needname = true;
	bp->temporary = true;
	bp->nosave = true;
	return bp;
}

Buffer *
create_scratch_buffer (void)
{
	return create_auto_buffer ("*scratch*");
}

static Completion *
make_buffer_completion (void)
{
	Completion *cp = completion_new (false);
	for (Buffer *bp = head_bp; bp != NULL; bp = bp->next)
		sv_add_sorted (&cp->completions, bp->name);
	return cp;
}

Completion *
buffer_make_completion (void)
{
	return make_buffer_completion ();
}

