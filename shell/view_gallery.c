// SPDX-License-Identifier: MIT
/*
 * view_gallery.c - Galleria: le immagini di ~/Immagini (sottocartelle
 * comprese, quindi anche le schermate) in una griglia di miniature.
 *
 * Niente di lento gira nel filo dell'interfaccia: la ricerca dei file, la
 * lettura delle miniature dalla cache e la loro creazione passano per
 * bs_work. La griglia e' una bui_list le cui righe contengono `cols`
 * miniature: scorrimento cinetico e disegno delle sole righe visibili
 * arrivano gratis. In memoria restano al massimo THUMB_MEM miniature
 * (51 kB l'una); le altre si rileggono dalla cache su disco quando tornano
 * visibili.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bs_img.h"
#include "bs_work.h"
#include "view_gallery.h"

#define MAX_IMAGES 5000
#define MAX_DEPTH 6
#define THUMB_MEM 48		/* miniature tenute in memoria (~2,4 MB) */
#define MAX_PENDING 4		/* richieste in coda: poche, cosi' seguono lo scorrimento */
#define GAP 8

struct item {
	char *path;
	long long mtime, size;
	uint8_t failed;
};

struct scan {
	char root[512];
	struct item *it;
	int n, cap, truncated, gen;
};

struct tjob {
	char path[1024];
	long long mtime, size;
	int index, gen, rc;
	bs_img img;
};

struct prune {
	unsigned long long *keys;
	int n;
};

static struct {
	bui_panel head, body;
	bui_list list;
	bui_rect area;
	int head_cap, viewer;
	char count[48];
	bui_widget *count_w;

	struct item *it;
	int n, truncated;
	int scanning, scan_job, gen;

	int cols, cell, x0;
	int sel, focus_vis, press_col;

	struct {
		int index;	/* -1: libero */
		bs_img img;
	} th[THUMB_MEM];
	struct {
		int id, index;
	} pend[MAX_PENDING];
	int pump_timer;
} G;

static bs_view *self;

static void pump(void *user);

/* ------------------------------------------------------------------ */
/* ricerca delle immagini (filo di lavoro)                            */
/* ------------------------------------------------------------------ */

static void scan_dir(struct scan *s, const char *dir, int depth, const volatile int *cancel)
{
	struct dirent *de;
	DIR *d = opendir(dir);
	char path[1024];
	struct stat st;

	if (!d)
		return;
	while ((de = readdir(d)) != NULL && !*cancel) {
		if (de->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path))
			continue;
		if (de->d_type == DT_DIR) {
			if (depth < MAX_DEPTH)
				scan_dir(s, path, depth + 1, cancel);
			continue;
		}
		if (de->d_type != DT_REG && de->d_type != DT_LNK && de->d_type != DT_UNKNOWN)
			continue;
		if (de->d_type != DT_UNKNOWN && !bs_img_is_image(de->d_name))
			continue;
		if (stat(path, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			/* solo cartelle vere: un collegamento potrebbe chiudere un anello */
			if (de->d_type == DT_UNKNOWN && depth < MAX_DEPTH && !lstat(path, &st) &&
			    S_ISDIR(st.st_mode))
				scan_dir(s, path, depth + 1, cancel);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !bs_img_is_image(de->d_name))
			continue;
		if (s->n >= MAX_IMAGES) {
			s->truncated = 1;
			break;
		}
		if (s->n == s->cap) {
			int nc = s->cap ? s->cap * 2 : 64;
			struct item *ni = realloc(s->it, sizeof(*ni) * (size_t)nc);

			if (!ni)
				break;
			s->it = ni;
			s->cap = nc;
		}
		memset(&s->it[s->n], 0, sizeof(s->it[0]));
		s->it[s->n].path = strdup(path);
		if (!s->it[s->n].path)
			break;
		s->it[s->n].mtime = (long long)st.st_mtime;
		s->it[s->n].size = (long long)st.st_size;
		s->n++;
	}
	closedir(d);
}

static int item_cmp(const void *a, const void *b)
{
	const struct item *x = a, *y = b;

	if (x->mtime != y->mtime)
		return x->mtime > y->mtime ? -1 : 1;	/* le piu' recenti in testa */
	return strcmp(x->path, y->path);
}

static void scan_run(void *arg, const volatile int *cancel)
{
	struct scan *s = arg;

	scan_dir(s, s->root, 0, cancel);
	if (s->n > 1)
		qsort(s->it, (size_t)s->n, sizeof(s->it[0]), item_cmp);
}

static void items_free(struct item *it, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free(it[i].path);
	free(it);
}

/* ------------------------------------------------------------------ */
/* pulizia della cache (filo di lavoro)                               */
/* ------------------------------------------------------------------ */

static int ull_cmp(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;

	return x < y ? -1 : x > y;
}

static void prune_run(void *arg, const volatile int *cancel)
{
	struct prune *p = arg;

	qsort(p->keys, (size_t)p->n, sizeof(p->keys[0]), ull_cmp);
	bs_img_thumb_prune(p->keys, p->n, cancel);
}

static void prune_done(void *arg, int cancelled)
{
	struct prune *p = arg;

	(void)cancelled;
	free(p->keys);
	free(p);
}

static void prune_start(void)
{
	struct prune *p;
	int i;

	if (G.truncated)
		return;		/* elenco incompleto: non si sa cosa sia davvero orfano */
	p = calloc(1, sizeof(*p));
	if (!p)
		return;
	p->keys = malloc(sizeof(p->keys[0]) * (size_t)(G.n ? G.n : 1));
	if (!p->keys) {
		free(p);
		return;
	}
	for (i = 0; i < G.n; i++)
		p->keys[i] = bs_img_thumb_key(G.it[i].path, G.it[i].mtime, G.it[i].size);
	p->n = G.n;
	if (!bs_work_submit(prune_run, prune_done, p, 0))
		prune_done(p, 1);
}

/* ------------------------------------------------------------------ */
/* miniature                                                          */
/* ------------------------------------------------------------------ */

static int rows(void)
{
	return G.cols > 0 ? (G.n + G.cols - 1) / G.cols : 0;
}

static const bs_img *thumb_get(int index)
{
	int i;

	for (i = 0; i < THUMB_MEM; i++)
		if (G.th[i].index == index)
			return &G.th[i].img;
	return NULL;
}

static void thumbs_clear(void)
{
	int i;

	for (i = 0; i < THUMB_MEM; i++) {
		bs_img_free(&G.th[i].img);
		G.th[i].index = -1;
	}
}

static void pending_cancel_all(void)
{
	int i;

	for (i = 0; i < MAX_PENDING; i++) {
		bs_work_cancel(G.pend[i].id);
		G.pend[i].id = 0;
	}
}

static int pending_has(int index)
{
	int i;

	for (i = 0; i < MAX_PENDING; i++)
		if (G.pend[i].id && G.pend[i].index == index)
			return 1;
	return 0;
}

/* righe visibili (compresa una di margine sopra e sotto) */
static void visible(int *first, int *last)
{
	int vh = G.list.w ? G.list.w->r.h : 0, rh = G.list.row_h > 0 ? G.list.row_h : 1;

	*first = G.list.sc.pos / rh - 1;
	*last = (G.list.sc.pos + vh - 1) / rh + 1;
}

static bui_rect cell_rect(int index)
{
	bui_rect r = bui_widget_rect(G.list.w);
	int row = index / G.cols, col = index % G.cols;

	return bui_r(r.x + G.x0 + col * (G.cell + GAP), r.y + row * G.list.row_h - G.list.sc.pos + GAP / 2,
		     G.cell, G.cell);
}

static void cell_damage(int index)
{
	bui_rect r;

	if (index < 0 || index >= G.n || !G.list.w || G.viewer)
		return;
	if (bui_rect_isect(bui_rect_inset(cell_rect(index), -4, -4), bui_widget_rect(G.list.w), &r))
		bs_view_damage(r);
}

static void thumb_store(int index, bs_img *img)
{
	int first, last, center, i, slot = -1, far = -1;

	visible(&first, &last);
	center = (first + last) / 2 * G.cols;
	for (i = 0; i < THUMB_MEM; i++) {
		int d;

		if (G.th[i].index < 0) {
			slot = i;
			break;
		}
		d = abs(G.th[i].index - center);
		if (d > far) {
			far = d;
			slot = i;
		}
	}
	bs_img_free(&G.th[slot].img);	/* la piu' lontana da cio' che si vede */
	G.th[slot].index = index;
	G.th[slot].img = *img;
	img->px = NULL;
}

static void thumb_run(void *arg, const volatile int *cancel)
{
	struct tjob *j = arg;

	j->rc = bs_img_thumb(j->path, j->mtime, j->size, cancel, &j->img);
}

static void thumb_done(void *arg, int cancelled)
{
	struct tjob *j = arg;
	int i;

	if (cancelled || j->gen != G.gen || !bs_view_is_active(self)) {
		bs_img_free(&j->img);
		free(j);
		return;
	}
	for (i = 0; i < MAX_PENDING; i++)
		if (G.pend[i].id && G.pend[i].index == j->index)
			G.pend[i].id = 0;
	if (j->index < G.n) {
		if (j->rc == BS_IMG_OK)
			thumb_store(j->index, &j->img);
		else
			G.it[j->index].failed = 1;
		cell_damage(j->index);
	}
	bs_img_free(&j->img);
	free(j);
	pump(NULL);
}

/* chiede le miniature che servono ora e annulla quelle uscite dallo schermo */
static void pump(void *user)
{
	int first, last, i, idx, slot;

	if (user)
		G.pump_timer = 0;
	if (!bs_view_is_active(self) || G.viewer || !G.list.w || G.cols < 1)
		return;
	visible(&first, &last);
	for (i = 0; i < MAX_PENDING; i++)
		if (G.pend[i].id && (G.pend[i].index / G.cols < first || G.pend[i].index / G.cols > last)) {
			bs_work_cancel(G.pend[i].id);
			G.pend[i].id = 0;
		}
	if (first < 0)
		first = 0;
	for (idx = first * G.cols; idx < G.n && idx < (last + 1) * G.cols; idx++) {
		struct tjob *j;

		if (G.it[idx].failed || thumb_get(idx) || pending_has(idx))
			continue;
		for (slot = 0; slot < MAX_PENDING && G.pend[slot].id; slot++)
			;
		if (slot == MAX_PENDING)
			return;
		j = calloc(1, sizeof(*j));
		if (!j)
			return;
		snprintf(j->path, sizeof(j->path), "%s", G.it[idx].path);
		j->mtime = G.it[idx].mtime;
		j->size = G.it[idx].size;
		j->index = idx;
		j->gen = G.gen;
		G.pend[slot].index = idx;
		G.pend[slot].id = bs_work_submit(thumb_run, thumb_done, j, 0);
		if (!G.pend[slot].id) {
			free(j);
			return;
		}
	}
}

static void pump_soon(void)
{
	if (!G.pump_timer)
		G.pump_timer = bui_timer_add(20, 0, pump, &G);
}

/* ------------------------------------------------------------------ */
/* elenco                                                             */
/* ------------------------------------------------------------------ */

static void set_count(void)
{
	if (G.scanning)
		G.count[0] = 0;
	else if (G.n == 1)
		snprintf(G.count, sizeof(G.count), "1 immagine");
	else
		snprintf(G.count, sizeof(G.count), "%d immagini%s", G.n, G.truncated ? " (le prime)" : "");
	if (G.count_w)
		bui_widget_damage(G.count_w);
}

static void scan_done(void *arg, int cancelled)
{
	struct scan *s = arg;

	if (cancelled || s->gen != G.gen || !bs_view_is_active(self)) {
		items_free(s->it, s->n);
		free(s);
		return;
	}
	G.scan_job = 0;
	G.scanning = 0;
	items_free(G.it, G.n);
	G.it = s->it;
	G.n = s->n;
	G.truncated = s->truncated;
	free(s);
	if (G.sel >= G.n)
		G.sel = G.n ? G.n - 1 : 0;
	bui_list_set_count(&G.list, rows());
	set_count();
	bs_view_damage(G.area);
	prune_start();
}

static void scan_start(void)
{
	const char *home = getenv("HOME");
	struct scan *s = calloc(1, sizeof(*s));

	if (!s)
		return;
	snprintf(s->root, sizeof(s->root), "%s/Immagini", home && *home ? home : "");
	s->gen = G.gen;
	G.scan_job = bs_work_submit(scan_run, scan_done, s, 1);
	if (!G.scan_job) {
		free(s);
		return;
	}
	G.scanning = 1;
}

/* ------------------------------------------------------------------ */
/* visualizzatore                                                     */
/* ------------------------------------------------------------------ */

static int src_count(void *user)
{
	(void)user;
	return G.n;
}

static const char *src_path(int i, void *user)
{
	(void)user;
	return i >= 0 && i < G.n ? G.it[i].path : NULL;
}

static void src_removed(int index, void *user)
{
	int i;

	(void)user;
	if (index < 0 || index >= G.n)
		return;
	/* gli indici scalano: le richieste in corso non valgono piu' */
	G.gen++;
	pending_cancel_all();
	free(G.it[index].path);
	memmove(&G.it[index], &G.it[index + 1], sizeof(G.it[0]) * (size_t)(G.n - index - 1));
	G.n--;
	for (i = 0; i < THUMB_MEM; i++) {
		if (G.th[i].index == index) {
			bs_img_free(&G.th[i].img);
			G.th[i].index = -1;
		} else if (G.th[i].index > index) {
			G.th[i].index--;
		}
	}
	bui_list_set_count(&G.list, rows());
	set_count();
}

static void src_closed(int index, void *user)
{
	(void)user;
	G.viewer = 0;
	if (index >= 0 && index < G.n) {
		G.sel = index;
		bui_list_reveal(&G.list, index / G.cols);
	}
	bs_view_damage(G.area);
}

static void open_viewer(int index)
{
	static const bs_viewer_src src = { src_count, src_path, src_removed, src_closed, NULL };

	if (index < 0 || index >= G.n)
		return;
	/* la coda serve all'immagine grande: le miniature aspettano */
	pending_cancel_all();
	G.sel = index;
	G.viewer = 1;
	bs_viewer_open(self->win, G.area, &src, index);
}

/* ------------------------------------------------------------------ */
/* disegno                                                            */
/* ------------------------------------------------------------------ */

static void draw_cell(bui_canvas *c, int idx, bui_rect r, int pressed)
{
	const bs_img *t = thumb_get(idx);
	bui_rect saved;

	if (t && t->px) {
		saved = bui_clip_push(c, r);
		if (t->w < r.w || t->h < r.h)
			bui_fill(c, r, BUI_C_SURFACE);
		bui_blit(c, r.x + (r.w - t->w) / 2, r.y + (r.h - t->h) / 2, t->px, t->w, t->h, t->w);
		if (pressed)
			bui_fill_a(c, r, BUI_C_BAR, 110);
		bui_clip_pop(c, saved);
		/* angoli arrotondati: una cornice del colore di fondo attorno alla miniatura */
		bui_rrect_line(c, bui_rect_inset(r, -4, -4), 12, 4, BUI_C_BG);
	} else {
		bui_rrect(c, r, 8, pressed ? BUI_C_SURFACE2 : BUI_C_SURFACE);
		bui_icon_in(c, G.it[idx].failed ? BUI_IC(broken, 40) : BUI_IC(image, 40), r, BUI_C_TEXT3);
		if (!G.it[idx].failed)
			pump_soon();
	}
	if (G.focus_vis && idx == G.sel)
		bui_rrect_line(c, r, 8, 3, BUI_C_ACCENT);
}

static void draw_row(bui_list *l, bui_canvas *c, int row, bui_rect r, unsigned flags)
{
	int col;

	(void)l;
	for (col = 0; col < G.cols; col++) {
		int idx = row * G.cols + col;

		if (idx >= G.n)
			break;
		draw_cell(c, idx, bui_r(r.x + G.x0 + col * (G.cell + GAP), r.y + GAP / 2, G.cell, G.cell),
			  (flags & BUI_F_PRESSED) && col == G.press_col);
	}
}

static void gl_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	bui_rect a = G.area;

	(void)v;
	if (G.viewer) {
		bs_viewer_draw(c, clip);
		return;
	}
	bui_panel_draw(&G.head, c, clip);
	bui_panel_draw(&G.body, c, clip);
	if (G.n == 0) {
		int cy = a.y + BUI_HEADER_H + (a.h - BUI_HEADER_H) / 2 - 76;
		bui_rect badge = bui_r(a.x + (a.w - 88) / 2, cy, 88, 88);

		if (G.scanning) {
			bui_text_box(c, BUI_FONT_BODY, bui_r(a.x + 16, cy + 60, a.w - 32, 32),
				     "Ricerca delle immagini\xE2\x80\xA6", BUI_C_TEXT2, BUI_AL_CENTER);
			return;
		}
		bui_rrect(c, badge, 44, BUI_C_SURFACE);
		bui_icon_in(c, BUI_IC(gallery, 40), badge, BUI_C_TEXT2);
		bui_text_box(c, BUI_FONT_TITLE, bui_r(a.x + 16, cy + 104, a.w - 32, 32), "Nessuna immagine",
			     BUI_C_TEXT, BUI_AL_CENTER);
		bui_text_wrap(c, BUI_FONT_BODY, bui_r(a.x + (a.w - 380) / 2, cy + 140, 380, 60),
			      "Le foto copiate nella cartella Immagini e le schermate compaiono qui.",
			      BUI_C_TEXT2, 26, BUI_AL_CENTER);
	}
}

/* ------------------------------------------------------------------ */
/* ingresso                                                           */
/* ------------------------------------------------------------------ */

static void on_select(bui_list *l, int row)
{
	(void)l;
	open_viewer(row * G.cols + G.press_col);
}

static void on_back(bui_widget *w)
{
	(void)w;
	bs_view_close();
}

static void hide_focus(void)
{
	if (G.focus_vis) {
		G.focus_vis = 0;
		cell_damage(G.sel);
	}
}

static int gl_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	if (G.viewer)
		return bs_viewer_pointer(ev);
	if (ev->type == BUI_PRESS || ev->type == BUI_WHEEL) {
		G.head_cap = bui_rect_has(G.head.vp, ev->x, ev->y);
		hide_focus();
		if (!G.head_cap && G.list.w && G.cols > 0) {
			int col = (ev->x - bui_widget_rect(G.list.w).x - G.x0 + GAP / 2) / (G.cell + GAP);

			G.press_col = col < 0 ? 0 : col >= G.cols ? G.cols - 1 : col;
		}
	}
	if (G.head_cap)
		return bui_panel_pointer(&G.head, ev);	/* puo' chiudere la vista */
	return bui_panel_pointer(&G.body, ev);
}

static void move_sel(int to)
{
	int old = G.sel;

	if (to < 0)
		to = 0;
	if (to >= G.n)
		to = G.n - 1;
	if (to < 0)
		return;
	if (!G.focus_vis) {
		/* il primo tasto mostra soltanto dove si trova la selezione */
		int first, last;

		visible(&first, &last);
		G.focus_vis = 1;
		to = G.sel;
		if (to / G.cols <= first || to / G.cols >= last)
			to = (first + 1) * G.cols;
		if (to >= G.n)
			to = G.n - 1;
	}
	G.sel = to;
	bui_list_reveal(&G.list, to / G.cols);
	cell_damage(old);
	cell_damage(to);
}

static int gl_key(bs_view *v, const bui_key *k)
{
	int page;

	(void)v;
	if (G.viewer)
		return bs_viewer_key(k);
	if (!G.n || G.cols < 1 || !G.list.w)
		return 0;
	page = (G.list.w->r.h / G.list.row_h) * G.cols;
	if (page < G.cols)
		page = G.cols;
	switch (k->key) {
	case BUI_K_LEFT:
		move_sel(G.sel - 1);
		return 1;
	case BUI_K_RIGHT:
		move_sel(G.sel + 1);
		return 1;
	case BUI_K_UP:
		move_sel(G.sel >= G.cols ? G.sel - G.cols : G.sel);
		return 1;
	case BUI_K_DOWN:
		move_sel(G.sel + G.cols < G.n ? G.sel + G.cols : G.focus_vis ? G.n - 1 : G.sel);
		return 1;
	case BUI_K_PGUP:
		move_sel(G.sel - page);
		return 1;
	case BUI_K_PGDN:
		move_sel(G.sel + page);
		return 1;
	case BUI_K_HOME:
		move_sel(0);
		return 1;
	case BUI_K_END:
		move_sel(G.n - 1);
		return 1;
	case BUI_K_ENTER:
	case ' ':
		if (!G.focus_vis)
			return 0;
		open_viewer(G.sel);
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* vita della vista                                                   */
/* ------------------------------------------------------------------ */

static void gl_layout(bs_view *v, bui_rect a)
{
	bui_rect body = bui_r(a.x, a.y + BUI_HEADER_H, a.w, a.h - BUI_HEADER_H);
	int keep_row = G.cols > 0 ? G.sel / G.cols : 0;

	(void)v;
	G.area = a;
	bui_header_build(&G.head, bui_r(a.x, a.y, a.w, BUI_HEADER_H), "Galleria", on_back);
	G.count_w = bui_add_label(&G.head, bui_r(a.w - 216, 0, 200, BUI_HEADER_H), G.count, BUI_LS_SMALL,
				  BUI_AL_RIGHT);
	/* miniature da BS_THUMB px al massimo: 5 colonne a 854, 3 a 480 */
	G.cols = (a.w - GAP) / (BS_THUMB + GAP);
	if (G.cols * (BS_THUMB + GAP) + GAP < a.w - BS_THUMB / 2)
		G.cols++;
	if (G.cols < 1)
		G.cols = 1;
	G.cell = (a.w - GAP * (G.cols + 1)) / G.cols;
	if (G.cell > BS_THUMB)
		G.cell = BS_THUMB;
	G.x0 = (a.w - G.cols * G.cell - (G.cols - 1) * GAP) / 2;
	bui_panel_clear(&G.body);
	bui_panel_set_viewport(&G.body, body, body.h);
	G.list.row_h = G.cell + GAP;
	G.list.draw_row = draw_row;
	G.list.on_select = on_select;
	G.list.count = rows();
	G.list.sel = -1;
	bui_add_list(&G.body, bui_r(0, 0, body.w, body.h), &G.list)->flags |= BUI_F_NOFOCUS;
	bui_list_reveal(&G.list, keep_row);
	if (G.viewer)
		bs_viewer_layout(a);
}

static void gl_enter(bs_view *v)
{
	int i;

	self = v;
	bui_panel_init(&G.head, v->win);
	bui_panel_init(&G.body, v->win);
	memset(&G.list, 0, sizeof(G.list));
	for (i = 0; i < THUMB_MEM; i++)
		G.th[i].index = -1;
	G.gen++;
	G.viewer = 0;
	G.sel = 0;
	G.focus_vis = 0;
	G.cols = 0;
	G.count[0] = 0;
	scan_start();
}

static void gl_leave(bs_view *v)
{
	(void)v;
	G.gen++;
	bs_viewer_close();
	G.viewer = 0;
	bs_work_cancel(G.scan_job);
	G.scan_job = 0;
	G.scanning = 0;
	pending_cancel_all();
	if (G.pump_timer)
		bui_timer_del(G.pump_timer);
	G.pump_timer = 0;
	thumbs_clear();
	items_free(G.it, G.n);
	G.it = NULL;
	G.n = 0;
	G.count_w = NULL;
	bui_panel_clear(&G.head);
	bui_panel_clear(&G.body);
}

bs_view bs_view_gallery = {
	.flags = BS_VIEW_NO_HEADER,
	.enter = gl_enter,
	.leave = gl_leave,
	.layout = gl_layout,
	.draw = gl_draw,
	.pointer = gl_pointer,
	.key = gl_key,
};
