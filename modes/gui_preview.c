/* Showing images and PDFs in xzile-gui: workers, textures, drawing

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
 * Decoding happens on the CPU, in worker threads: images in
 * image_decode.c, PDF pages, at the size they are shown, in
 * pdf_render.c. The main thread turns the results into
 * textures, which the renderer (the GPU, unless SDL fell back to its
 * software renderer) scales and moves: panning and zooming draw what is
 * cached and decode nothing. An image keeps levels of detail, each half
 * the size of the last, so that it shrinks smoothly. A zoomed PDF page
 * is drawn scaled at once, and rendered again at the new size in the
 * background; the pages either side are rendered ahead.
 *
 * TEXTURE_BUDGET is a soft limit: past it, textures not on screen are
 * freed, the least recently shown first. Those on screen are kept, so
 * that drawing never waits for decoding; how large each can be is
 * bounded instead (image_decode.c, MAX_PAGE_TEXELS). PDF pages are read
 * only when shown or about to be. A page that cannot be rendered is not
 * tried again until the file is reloaded.
 *
 * `make gui' includes both image and PDF rendering libraries.
 */

#include "preview.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>

#include <SDL.h>

#include "../src/screen.h"
#include "gui_preview.h"
#include "gui_render.h"

#define NWORKERS 2
#define TEXTURE_BUDGET ((size_t) 256 << 20)
/* PDF pages are rendered at most this large, and shown enlarged. */
#define MAX_PAGE_TEXELS (12.0 * 1024 * 1024)
/* PDFs at 100% are shown at 96 pixels to the inch. */
#define PDF_PX_PER_PT (96.0 / 72.0)
#define CHECKER_SIZE 256
#define CHECKER_CELL 8

typedef struct {
	SDL_Texture *tex;
	int w, h;
} Tex;

typedef struct {
	long page;
	Tex t;
	uint64_t used;		/* The frame it was last drawn in. */
	bool stale;		/* From before the file was reloaded. */
} PageTex;

typedef struct {
	long page;
	char *error;
} PageError;

struct PreviewDoc {
	PreviewDoc *next;
	enum PreviewKind kind;
	char *path;
	unsigned gen;		/* Results of older generations are dropped. */
	int refs;		/* The owner's, and one per job. */
	bool closed;
	bool busy;		/* A worker has a job for it; under `lock'. */
	enum PreviewState state;
	char *error;
	bool loading;
	uint64_t used;

	/* An image. */
	long img_w, img_h;
	Tex mips[IMAGE_MAX_LEVELS];
	int nmips;
	bool reduced, alpha;

	/* A PDF. */
	long npages;
	double *page_w, *page_h;	/* In points. */
	PageTex *pages;
	size_t npagetex, pagetex_cap;
	PageError *page_errors;	/* Pages that could not be rendered. */
	size_t npage_errors;
	PdfFile *pdf;		/* Used only by workers. */
};

enum JobType { JOB_LOAD, JOB_RENDER, JOB_FREE_PDF };

typedef struct Job Job;
struct Job {
	Job *next;
	enum JobType type;
	PreviewDoc *doc;
	unsigned gen;
	int prio;		/* 0 for what is shown, 1 for what may be. */
	bool running;
	long page;
	int w, h;
	PdfFile *pdf;		/* For JOB_FREE_PDF. */

	/* Results. */
	char *error;
	DecodedImage img;
	SDL_Surface *surface;	/* A page. */
	long npages;
	double *page_w, *page_h;
};

/* Everything below `lock' is shared with the workers. Otherwise all is
   for the main thread. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
static Job *queue;		/* Waiting and running jobs. */
static Job *done;		/* Finished, newest first. */
static bool quitting;
static bool wake_pending;
/* Poppler is not safe to use from two threads at once. */
static bool pdf_running;
static int running_workers;
static pthread_t workers[NWORKERS];
static int nworkers;
static Uint32 wake_event = (Uint32) -1;
static int max_tex_w, max_tex_h;

static SDL_Renderer *renderer;
static char renderer_name[64] = "unknown";
static bool accelerated;
static int pad, cell_w = 8, cell_h = 16;
static float scale = 1;
static PreviewDoc *docs;
static size_t texture_bytes;
static uint64_t frame;
static SDL_Texture *checker;

typedef struct {
	PreviewDoc *doc;
	PreviewView view;
	size_t y, x, rows, cols;
} Place;

static Place *places;
static size_t nplaces, places_cap;


/*--------------------------------------------------------------------------
 * Workers.
 *--------------------------------------------------------------------------*/

static void
load (Job *j)
{
	PreviewDoc *d = j->doc;
	if (d->kind == PREVIEW_IMAGE) {
		j->error = image_decode (d->path, max_tex_w, max_tex_h, &j->img);
		return;
	}
	PdfFile *pdf = NULL;
	j->error = pdf_render_open (d->path, &pdf, &j->npages, &j->page_w, &j->page_h);
	if (j->error == NULL) {
		/* No other job of the document is running. */
		pdf_render_close (d->pdf);
		d->pdf = pdf;
	}
}

static bool
is_pdf_job (const Job *j)
{
	return j->type == JOB_FREE_PDF || (j->doc != NULL && j->doc->kind == PREVIEW_PDF);
}

/* The most urgent job that can run now, marked as running. */
static Job *
take_job (void)
{
	Job *best = NULL;
	for (Job *j = queue; j != NULL; j = j->next)
		if (!j->running && (j->doc == NULL || !j->doc->busy) && !(is_pdf_job (j) && pdf_running) &&
		    (best == NULL || j->prio < best->prio))
			best = j;
	if (best != NULL) {
		best->running = true;
		if (best->doc != NULL)
			best->doc->busy = true;
		if (is_pdf_job (best))
			pdf_running = true;
	}
	return best;
}

static void
unqueue (Job *j)
{
	for (Job **jp = &queue; *jp != NULL; jp = &(*jp)->next)
		if (*jp == j) {
			*jp = j->next;
			return;
		}
}

static void *
worker (void *arg)
{
	(void) arg;
	pthread_mutex_lock (&lock);
	while (!quitting) {
		Job *j = take_job ();
		if (j == NULL) {
			pthread_cond_wait (&work_cond, &lock);
			continue;
		}
		pthread_mutex_unlock (&lock);
		switch (j->type) {
		case JOB_LOAD:
			load (j);
			break;
		case JOB_RENDER:
			j->error = pdf_render_page (j->doc->pdf, j->page, j->w, j->h, &j->surface);
			break;
		case JOB_FREE_PDF:
			pdf_render_close (j->pdf);
			break;
		}
		pthread_mutex_lock (&lock);
		if (j->doc != NULL)
			j->doc->busy = false;
		if (is_pdf_job (j))
			pdf_running = false;
		unqueue (j);
		if (j->type == JOB_FREE_PDF)
			free (j);
		else {
			j->next = done;
			done = j;
			/* SDL may be shut down once `quitting' is set. */
			if (!wake_pending && !quitting && wake_event != (Uint32) -1) {
				SDL_Event ev;
				SDL_zero (ev);
				ev.type = wake_event;
				SDL_PushEvent (&ev);
				wake_pending = true;
			}
		}
		pthread_cond_broadcast (&work_cond);
	}
	running_workers--;
	pthread_cond_broadcast (&exit_cond);
	pthread_mutex_unlock (&lock);
	return NULL;
}

static void
submit (Job *j)
{
	pthread_mutex_lock (&lock);
	Job **jp = &queue;
	while (*jp != NULL)
		jp = &(*jp)->next;
	*jp = j;
	while (!quitting && nworkers < NWORKERS && pthread_create (&workers[nworkers], NULL, worker, NULL) == 0) {
		nworkers++;
		running_workers++;
	}
	pthread_cond_broadcast (&work_cond);
	pthread_mutex_unlock (&lock);
}

static Job *
new_job (enum JobType type, PreviewDoc *d, int prio)
{
	Job *j = xzalloc (sizeof (Job));
	j->type = type;
	j->doc = d;
	j->gen = d->gen;
	j->prio = prio;
	d->refs++;
	return j;
}

static void
job_free (Job *j)
{
	image_decoded_free (&j->img);
	SDL_FreeSurface (j->surface);
	free (j->error);
	free (j->page_w);
	free (j->page_h);
	free (j);
}


/*--------------------------------------------------------------------------
 * Documents and textures.
 *--------------------------------------------------------------------------*/

static void
tex_free (Tex *t)
{
	if (t->tex != NULL) {
		SDL_DestroyTexture (t->tex);
		texture_bytes -= (size_t) t->w * (size_t) t->h * 4;
	}
	t->tex = NULL;
	t->w = t->h = 0;
}

static bool
tex_from_surface (Tex *t, SDL_Surface *s, bool blend)
{
	SDL_Texture *tex = SDL_CreateTexture (renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, s->w, s->h);
	if (tex == NULL)
		return false;
	if (SDL_UpdateTexture (tex, NULL, s->pixels, s->pitch) != 0) {
		SDL_DestroyTexture (tex);
		return false;
	}
	SDL_SetTextureBlendMode (tex, blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	t->tex = tex;
	t->w = s->w;
	t->h = s->h;
	texture_bytes += (size_t) s->w * (size_t) s->h * 4;
	return true;
}

static void
free_mips (PreviewDoc *d)
{
	for (int i = 0; i < d->nmips; i++)
		tex_free (&d->mips[i]);
	d->nmips = 0;
}

static void
remove_page (PreviewDoc *d, size_t i)
{
	tex_free (&d->pages[i].t);
	d->pages[i] = d->pages[--d->npagetex];
}

static void
free_pages (PreviewDoc *d)
{
	while (d->npagetex > 0)
		remove_page (d, 0);
}

static PageTex *
find_page (PreviewDoc *d, long page)
{
	for (size_t i = 0; i < d->npagetex; i++)
		if (d->pages[i].page == page)
			return &d->pages[i];
	return NULL;
}

static const char *
page_error (const PreviewDoc *d, long page)
{
	for (size_t i = 0; i < d->npage_errors; i++)
		if (d->page_errors[i].page == page)
			return d->page_errors[i].error;
	return NULL;
}

static void
note_page_error (PreviewDoc *d, long page, char *error)
{
	if (page_error (d, page) != NULL) {
		free (error);
		return;
	}
	d->page_errors = xrealloc (d->page_errors, (d->npage_errors + 1) * sizeof (PageError));
	d->page_errors[d->npage_errors++] = (PageError) { page, error };
}

static void
clear_page_errors (PreviewDoc *d)
{
	for (size_t i = 0; i < d->npage_errors; i++)
		free (d->page_errors[i].error);
	free (d->page_errors);
	d->page_errors = NULL;
	d->npage_errors = 0;
}

static void
doc_unref (PreviewDoc *d)
{
	if (--d->refs > 0)
		return;
	if (d->pdf != NULL) {
		/* Not here: a worker may be busy with poppler. */
		Job *j = xzalloc (sizeof (Job));
		j->type = JOB_FREE_PDF;
		j->pdf = d->pdf;
		submit (j);
	}
	free (d->path);
	free (d->error);
	free (d->page_w);
	free (d->page_h);
	free (d->pages);
	clear_page_errors (d);
	free (d);
}

/* Cancel the waiting jobs of `d' that `keep' does not want kept. */
static void
cancel_jobs (PreviewDoc *d, bool (*keep) (const Job *j, long page), long page)
{
	Job *cancelled = NULL;
	pthread_mutex_lock (&lock);
	for (Job **jp = &queue; *jp != NULL;) {
		Job *j = *jp;
		if (j->doc == d && !j->running && (keep == NULL || !keep (j, page))) {
			*jp = j->next;
			j->next = cancelled;
			cancelled = j;
		} else
			jp = &j->next;
	}
	pthread_mutex_unlock (&lock);
	for (Job *j = cancelled, *next; j != NULL; j = next) {
		next = j->next;
		doc_unref (j->doc);
		job_free (j);
	}
}

static void
set_failed (PreviewDoc *d, char *error)
{
	free_mips (d);
	free_pages (d);
	free (d->error);
	d->error = error;
	d->state = PREVIEW_FAILED;
}

static void
finish_load (PreviewDoc *d, Job *j)
{
	d->loading = false;
	if (j->error != NULL) {
		set_failed (d, j->error);
		j->error = NULL;
		return;
	}
	free (d->error);
	d->error = NULL;

	if (d->kind == PREVIEW_IMAGE) {
		free_mips (d);
		for (int i = 0; i < j->img.nlevels; i++) {
			if (!tex_from_surface (&d->mips[i], j->img.levels[i], j->img.alpha))
				break;
			d->nmips++;
		}
		if (d->nmips == 0) {
			set_failed (d, xasprintf ("cannot make a texture: %s", SDL_GetError ()));
			return;
		}
		d->img_w = j->img.width;
		d->img_h = j->img.height;
		d->reduced = j->img.reduced;
		d->alpha = j->img.alpha;
	} else {
		free (d->page_w);
		free (d->page_h);
		d->page_w = j->page_w;
		d->page_h = j->page_h;
		j->page_w = j->page_h = NULL;
		d->npages = j->npages;
		/* Old pages stay on screen until they are rendered again. */
		for (size_t i = 0; i < d->npagetex;)
			if (d->pages[i].page >= d->npages)
				remove_page (d, i);
			else
				d->pages[i++].stale = true;
	}
	d->state = PREVIEW_READY;
}

static void
finish_render (PreviewDoc *d, Job *j)
{
	if (j->page >= d->npages)
		return;
	PageTex *pt = find_page (d, j->page);
	if (j->surface == NULL) {
		note_page_error (d, j->page, j->error != NULL ? j->error : xstrdup ("cannot render the page"));
		j->error = NULL;
		/* An old rendering would hide that this one failed. */
		if (pt != NULL)
			remove_page (d, (size_t) (pt - d->pages));
		return;
	}
	if (pt == NULL) {
		if (d->npagetex == d->pagetex_cap) {
			d->pagetex_cap = d->pagetex_cap ? d->pagetex_cap * 2 : 8;
			d->pages = xrealloc (d->pages, d->pagetex_cap * sizeof (PageTex));
		}
		pt = &d->pages[d->npagetex++];
		memset (pt, 0, sizeof *pt);
		pt->page = j->page;
	}
	tex_free (&pt->t);
	pt->stale = false;
	pt->used = frame;
	if (!tex_from_surface (&pt->t, j->surface, false)) {
		note_page_error (d, j->page, xasprintf ("cannot make a texture: %s", SDL_GetError ()));
		remove_page (d, (size_t) (pt - d->pages));
	}
}

/* Free the least recently drawn textures while over budget. */
static void
trim_textures (void)
{
	while (texture_bytes > TEXTURE_BUDGET) {
		PreviewDoc *victim = NULL;
		size_t page = 0;
		bool is_page = false;
		uint64_t oldest = frame;
		for (PreviewDoc *d = docs; d != NULL; d = d->next) {
			if (d->nmips > 0 && d->used < oldest) {
				oldest = d->used;
				victim = d;
				is_page = false;
			}
			for (size_t i = 0; i < d->npagetex; i++)
				if (d->pages[i].used < oldest) {
					oldest = d->pages[i].used;
					victim = d;
					page = i;
					is_page = true;
				}
		}
		if (victim == NULL)
			break;
		if (is_page)
			remove_page (victim, page);
		else
			free_mips (victim);
	}
}

static void
start_load (PreviewDoc *d)
{
	d->loading = true;
	submit (new_job (JOB_LOAD, d, 0));
}

static bool
close_enough (int have, int want)
{
	return abs (have - want) <= MAX (2, want / 50);
}

/* The size to render a page shown `w' by `h', within the limits. */
static void
target_size (double w, double h, int *tw, int *th)
{
	double f = 1;
	if (max_tex_w > 0 && w > max_tex_w)
		f = MIN (f, max_tex_w / w);
	if (max_tex_h > 0 && h > max_tex_h)
		f = MIN (f, max_tex_h / h);
	if (w * h * f * f > MAX_PAGE_TEXELS)
		f = sqrt (MAX_PAGE_TEXELS / (w * h));
	*tw = MAX (1, (int) lround (w * f));
	*th = MAX (1, (int) lround (h * f));
}

/* Ask for page `page' rendered `w' by `h', unless that is in hand. */
static void
request_render (PreviewDoc *d, long page, int w, int h, int prio)
{
	pthread_mutex_lock (&lock);
	bool running_same = false;
	for (Job *j = queue; j != NULL; j = j->next)
		if (j->doc == d && j->type == JOB_RENDER && j->page == page && j->gen == d->gen) {
			if (!j->running) {
				j->w = w;
				j->h = h;
				j->prio = MIN (j->prio, prio);
				pthread_mutex_unlock (&lock);
				return;
			}
			running_same = running_same || close_enough (j->w, w);
		}
	pthread_mutex_unlock (&lock);
	if (running_same)
		return;
	Job *j = new_job (JOB_RENDER, d, prio);
	j->page = page;
	j->w = w;
	j->h = h;
	submit (j);
}

static bool
near_page (const Job *j, long page)
{
	return j->type != JOB_RENDER || labs (j->page - page) <= 1;
}

/* Pages kept either side of the one shown, so that going back and
   forth needs no rendering. */
#define KEEP_PAGES 2

/* Forget pages far from those shown, other than those just drawn. */
static void
prune_pages (PreviewDoc *d, long page)
{
	for (size_t i = 0; i < d->npagetex;)
		if (labs (d->pages[i].page - page) > KEEP_PAGES && d->pages[i].used != frame)
			remove_page (d, i);
		else
			i++;
}


/*--------------------------------------------------------------------------
 * Drawing.
 *--------------------------------------------------------------------------*/

static bool
page_size (PreviewDoc *d, long page, double *w, double *h)
{
	if (d->state != PREVIEW_READY)
		return false;
	if (d->kind == PREVIEW_IMAGE) {
		if (d->nmips == 0)
			return false;
		*w = (double) d->img_w;
		*h = (double) d->img_h;
		return true;
	}
	if (d->npages == 0)
		return false;
	page = MAX (0, MIN (page, d->npages - 1));
	*w = d->page_w[page] * PDF_PX_PER_PT * scale;
	*h = d->page_h[page] * PDF_PX_PER_PT * scale;
	return true;
}

static SDL_Texture *
checker_texture (void)
{
	if (checker != NULL)
		return checker;
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat (0, CHECKER_SIZE, CHECKER_SIZE, 32, SDL_PIXELFORMAT_ARGB8888);
	if (s == NULL)
		return NULL;
	for (int y = 0; y < CHECKER_SIZE; y++) {
		Uint32 *p = (Uint32 *) ((Uint8 *) s->pixels + y * s->pitch);
		for (int x = 0; x < CHECKER_SIZE; x++)
			p[x] = ((x / CHECKER_CELL + y / CHECKER_CELL) & 1) ? 0xffbdbdbd : 0xffe6e6e6;
	}
	checker = SDL_CreateTextureFromSurface (renderer, s);
	SDL_FreeSurface (s);
	return checker;
}

/* A chequerboard behind the transparent parts of an image. */
static void
draw_checker (const SDL_FRect *dst, const SDL_Rect *area)
{
	SDL_Texture *t = checker_texture ();
	SDL_Rect d = { (int) floorf (dst->x), (int) floorf (dst->y), (int) ceilf (dst->w), (int) ceilf (dst->h) }, clip;
	if (t == NULL || !SDL_IntersectRect (&d, area, &clip))
		return;
	SDL_RenderSetClipRect (renderer, &clip);
	int x0 = d.x + (clip.x - d.x) / CHECKER_SIZE * CHECKER_SIZE;
	int y0 = d.y + (clip.y - d.y) / CHECKER_SIZE * CHECKER_SIZE;
	for (int y = y0; y < clip.y + clip.h; y += CHECKER_SIZE)
		for (int x = x0; x < clip.x + clip.w; x += CHECKER_SIZE) {
			SDL_Rect r = { x, y, CHECKER_SIZE, CHECKER_SIZE };
			SDL_RenderCopy (renderer, t, NULL, &r);
		}
	SDL_RenderSetClipRect (renderer, area);
}

static void
draw_image (PreviewDoc *d, const SDL_FRect *dst, const SDL_Rect *area)
{
	/* The smallest level of detail at least as large as shown. */
	int level = 0;
	while (level + 1 < d->nmips && d->mips[level + 1].w >= dst->w && d->mips[level + 1].h >= dst->h)
		level++;
	Tex *t = &d->mips[level];
	if (d->alpha)
		draw_checker (dst, area);
	/* Show the pixels of a much enlarged image as such. */
	SDL_SetTextureScaleMode (t->tex, dst->w >= 3.0f * (float) t->w ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
	SDL_RenderCopyF (renderer, t->tex, NULL, dst);
}

static void
draw_page (PreviewDoc *d, long page, const SDL_FRect *dst)
{
	PageTex *pt = find_page (d, page);
	if (pt != NULL) {
		pt->used = frame;
		SDL_SetTextureScaleMode (pt->t.tex, SDL_ScaleModeLinear);
		SDL_RenderCopyF (renderer, pt->t.tex, NULL, dst);
	} else {
		/* A blank page until it is rendered. */
		SDL_SetRenderDrawColor (renderer, 255, 255, 255, 255);
		SDL_RenderFillRectF (renderer, dst);
	}
}

/* Ask for the page shown, if it is not rendered at about the size
   shown, and for those either side. */
static void
request_pages (PreviewDoc *d, long page, const SDL_FRect *dst, double page_scale)
{
	int tw, th;
	target_size (dst->w, dst->h, &tw, &th);
	PageTex *pt = find_page (d, page);
	if (page_error (d, page) == NULL && (pt == NULL || pt->stale || !close_enough (pt->t.w, tw))) {
		request_render (d, page, tw, th, 0);
		return;
	}
	/* Render the pages either side ahead, at the same scale. */
	for (long n = page - 1; n <= page + 1; n += 2) {
		if (n < 0 || n >= d->npages || page_error (d, n) != NULL)
			continue;
		int nw, nh;
		target_size (d->page_w[n] * PDF_PX_PER_PT * scale * page_scale,
			     d->page_h[n] * PDF_PX_PER_PT * scale * page_scale, &nw, &nh);
		PageTex *np = find_page (d, n);
		if (np == NULL || np->stale || !close_enough (np->t.w, nw))
			request_render (d, n, nw, nh, 1);
	}
}

/* The parts of the place still covered by picture cells: other things,
   such as a picker, may have been drawn over it since. Runs of cells
   in a row, each joined to the same run in the row above. */
static size_t
picture_rects (const Place *p, const Cell *cells, size_t rows, size_t cols, SDL_Rect **rects, size_t *cap)
{
	size_t n = 0, xend = MIN (p->x + p->cols, cols);
	for (size_t y = p->y; y < p->y + p->rows && y < rows; y++)
		for (size_t x = p->x; x < xend;) {
			if (!(cells[y * cols + x].attr.flags & ATTR_PICTURE)) {
				x++;
				continue;
			}
			size_t end = x + 1;
			while (end < xend && (cells[y * cols + end].attr.flags & ATTR_PICTURE))
				end++;
			SDL_Rect r = { pad + (int) x * cell_w, pad + (int) y * cell_h, (int) (end - x) * cell_w, cell_h };
			size_t i = 0;
			while (i < n && !((*rects)[i].x == r.x && (*rects)[i].w == r.w &&
					  (*rects)[i].y + (*rects)[i].h == r.y))
				i++;
			if (i < n)
				(*rects)[i].h += cell_h;
			else {
				if (n == *cap) {
					*cap = *cap ? *cap * 2 : 16;
					*rects = xrealloc (*rects, *cap * sizeof (SDL_Rect));
				}
				(*rects)[n++] = r;
			}
			x = end;
		}
	return n;
}

static void
draw_place (Place *p, const Cell *cells, size_t rows, size_t cols)
{
	static SDL_Rect *rects;
	static size_t cap;
	PreviewDoc *d = p->doc;
	d->used = frame;
	SDL_Rect area = { pad + (int) p->x * cell_w, pad + (int) p->y * cell_h,
			  (int) p->cols * cell_w, (int) p->rows * cell_h };
	if (area.w <= 0 || area.h <= 0)
		return;
	/* Its textures were freed: decode it again. */
	if (d->kind == PREVIEW_IMAGE && d->state == PREVIEW_READY && d->nmips == 0 && !d->loading)
		start_load (d);
	double pw, ph;
	if (!page_size (d, p->view.page, &pw, &ph))
		return;

	PreviewRect r = preview_layout (&p->view, area.w, area.h, pw, ph);
	SDL_FRect dst = { (float) (area.x + r.x), (float) (area.y + r.y), (float) r.w, (float) r.h };
	long page = MAX (0, MIN (p->view.page, d->npages - 1));
	/* preview.c says why in its place. */
	if (d->kind == PREVIEW_PDF && page_error (d, page) != NULL)
		return;
	size_t n = picture_rects (p, cells, rows, cols, &rects, &cap);
	for (size_t i = 0; i < n; i++) {
		SDL_Rect *clip = &rects[i];
		SDL_RenderSetClipRect (renderer, clip);
		if (!d->alpha) {
			float off = 3 * scale;
			SDL_FRect shadow = { dst.x + off, dst.y + off, dst.w, dst.h };
			SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor (renderer, 0, 0, 0, 60);
			SDL_RenderFillRectF (renderer, &shadow);
			SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_NONE);
		}
		if (d->kind == PREVIEW_IMAGE)
			draw_image (d, &dst, clip);
		else
			draw_page (d, page, &dst);
	}
	if (d->kind == PREVIEW_PDF && n > 0)
		request_pages (d, page, &dst, r.scale);
}

void
gui_preview_draw (const Cell *cells, size_t rows, size_t cols)
{
	if (nplaces == 0)
		return;
	frame++;
	for (size_t i = 0; i < nplaces; i++)
		draw_place (&places[i], cells, rows, cols);
	SDL_RenderSetClipRect (renderer, NULL);
	/* Pages flicked past need not be rendered, or kept. */
	for (size_t i = 0; i < nplaces; i++)
		if (places[i].doc->kind == PREVIEW_PDF) {
			cancel_jobs (places[i].doc, near_page, places[i].view.page);
			prune_pages (places[i].doc, places[i].view.page);
		}
	trim_textures ();
}


/*--------------------------------------------------------------------------
 * The backend.
 *--------------------------------------------------------------------------*/

static bool
b_supports (enum PreviewKind kind)
{
	return kind == PREVIEW_IMAGE ? image_decode_supported ()
		: kind == PREVIEW_PDF ? pdf_render_supported () : false;
}

static PreviewDoc *
b_open (enum PreviewKind kind, const char *path)
{
	PreviewDoc *d = xzalloc (sizeof (PreviewDoc));
	d->kind = kind;
	d->path = xstrdup (path);
	d->gen = 1;
	d->refs = 1;
	d->state = PREVIEW_LOADING;
	d->next = docs;
	docs = d;
	if (b_supports (kind))
		start_load (d);
	else
		set_failed (d, xstrdup ("not supported by this build"));
	return d;
}

static void
b_close (PreviewDoc *d)
{
	if (d == NULL)
		return;
	d->closed = true;
	free_mips (d);
	free_pages (d);
	size_t k = 0;
	for (size_t i = 0; i < nplaces; i++)
		if (places[i].doc != d)
			places[k++] = places[i];
	nplaces = k;
	for (PreviewDoc **dp = &docs; *dp != NULL; dp = &(*dp)->next)
		if (*dp == d) {
			*dp = d->next;
			break;
		}
	cancel_jobs (d, NULL, 0);
	doc_unref (d);
}

static void
b_reload (PreviewDoc *d)
{
	if (!b_supports (d->kind))
		return;
	d->gen++;
	cancel_jobs (d, NULL, 0);
	clear_page_errors (d);
	if (d->state == PREVIEW_FAILED) {
		free (d->error);
		d->error = NULL;
		d->state = PREVIEW_LOADING;
	}
	start_load (d);
}

static void
b_info (PreviewDoc *d, long page, PreviewInfo *info)
{
	memset (info, 0, sizeof *info);
	info->pages = d->kind == PREVIEW_PDF ? d->npages : 1;
	info->reduced = d->reduced;
	if (d->state == PREVIEW_FAILED) {
		info->state = PREVIEW_FAILED;
		info->error = d->error;
	} else if (page_size (d, page, &info->width, &info->height)) {
		info->state = PREVIEW_READY;
		if (d->kind == PREVIEW_IMAGE) {
			info->width = (double) d->img_w;
			info->height = (double) d->img_h;
		} else
			info->page_error = page_error (d, MAX (0, MIN (page, d->npages - 1)));
	} else
		info->state = PREVIEW_LOADING;
}

static void
b_place (PreviewDoc *d, const PreviewView *view, size_t y, size_t x, size_t rows, size_t cols)
{
	if (nplaces == places_cap) {
		places_cap = places_cap ? places_cap * 2 : 8;
		places = xrealloc (places, places_cap * sizeof (Place));
	}
	places[nplaces++] = (Place) { d, *view, y, x, rows, cols };
}

static void
b_clear (void)
{
	nplaces = 0;
}

static void
b_cell_pixels (size_t rows, size_t cols, double *w, double *h)
{
	*w = (double) cols * cell_w;
	*h = (double) rows * cell_h;
}

static char *
b_describe (PreviewDoc *d)
{
	size_t pages = d->kind == PREVIEW_PDF ? d->npagetex : 0;
	StrBuf sb;
	sb_init (&sb);
	sb_addf (&sb, "CPU decoding, %s renderer (%s)", renderer_name, accelerated ? "GPU" : "CPU");
	if (d->kind == PREVIEW_PDF)
		sb_addf (&sb, ", %zu pages cached", pages);
	sb_addf (&sb, ", textures %zu/%zu MiB", texture_bytes >> 20, TEXTURE_BUDGET >> 20);
	return sb_take (&sb);
}

static const PreviewBackend backend = {
	b_supports, b_open, b_close, b_reload, b_info, b_place, b_clear, b_cell_pixels, b_describe,
};

void
gui_preview_init (SDL_Renderer *r)
{
	renderer = r;
	wake_event = SDL_RegisterEvents (1);
	SDL_RendererInfo info;
	if (SDL_GetRendererInfo (r, &info) == 0) {
		max_tex_w = info.max_texture_width;
		max_tex_h = info.max_texture_height;
		snprintf (renderer_name, sizeof renderer_name, "%s", info.name);
		accelerated = (info.flags & SDL_RENDERER_ACCELERATED) != 0;
	}
	image_decode_init ();
	preview_backend = &backend;
}

Uint32
gui_preview_event (void)
{
	return wake_event;
}

void
gui_preview_set_metrics (int p, int cw, int ch, float s)
{
	pad = p;
	cell_w = cw;
	cell_h = ch;
	scale = s;
}

bool
gui_preview_poll (void)
{
	pthread_mutex_lock (&lock);
	Job *list = done;
	done = NULL;
	wake_pending = false;
	pthread_mutex_unlock (&lock);

	/* Oldest first. */
	Job *jobs = NULL;
	while (list != NULL) {
		Job *next = list->next;
		list->next = jobs;
		jobs = list;
		list = next;
	}
	bool changed = false;
	for (Job *j = jobs, *next; j != NULL; j = next) {
		next = j->next;
		PreviewDoc *d = j->doc;
		if (!d->closed && j->gen == d->gen) {
			if (j->type == JOB_LOAD)
				finish_load (d, j);
			else
				finish_render (d, j);
			changed = true;
		}
		doc_unref (d);
		job_free (j);
	}
	if (changed)
		trim_textures ();
	return changed;
}

void
gui_preview_reset (void)
{
	for (PreviewDoc *d = docs; d != NULL; d = d->next) {
		free_mips (d);
		free_pages (d);
	}
	if (checker != NULL) {
		SDL_DestroyTexture (checker);
		checker = NULL;
	}
}

void
gui_preview_shutdown (void)
{
	pthread_mutex_lock (&lock);
	quitting = true;
	pthread_cond_broadcast (&work_cond);
	/* Wait a little for a page being rendered; exiting ends it anyway. */
	struct timespec until;
	clock_gettime (CLOCK_REALTIME, &until);
	until.tv_sec += 1;
	while (running_workers > 0 && pthread_cond_timedwait (&exit_cond, &lock, &until) != ETIMEDOUT)
		;
	bool stopped = running_workers == 0;
	pthread_mutex_unlock (&lock);
	for (int i = 0; i < nworkers; i++)
		if (stopped)
			pthread_join (workers[i], NULL);
		else
			pthread_detach (workers[i]);
	nworkers = 0;

	gui_preview_reset ();
	nplaces = 0;
	if (stopped)
		image_decode_quit ();
}
