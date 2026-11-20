// SPDX-License-Identifier: MIT
/*
 * bui_comp.c - compositore software per i backend senza X (drm, mem).
 *
 * La shell possiede tutto lo schermo: home (desktop), barra (dock) e popup
 * sono livelli che qui vengono composti in software, solo nei rettangoli
 * cambiati. Quello che il backend X11 chiede al server (ordine delle
 * finestre, grab di puntatore e tastiera dei popup modali, fuoco della
 * tastiera sulla home, forma arrotondata del toast, cattura del fondo sotto
 * un popup) e' rifatto qui con le stesse regole, quindi le viste e la shell
 * si comportano allo stesso modo con entrambi.
 *
 * Costo di un fotogramma: il disegno delle finestre nelle loro tele (come
 * con X) piu' UNA copia dei pixel cambiati nella superficie d'uscita. Con X
 * le copie erano due (immagine XShm -> ShadowFB -> dumb buffer).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bui.h"
#include "bui_be.h"
#include "bui_comp.h"

#define MAX_WINS 12
#define BAND_GAP 12	/* righe: piu' vicine di cosi' due fasce si inviano insieme */

static struct {
	int sw, sh;
	bui_px *target;
	int stride;
	void (*present)(const bui_rect *, int);
	bui_win *wins[MAX_WINS];	/* dal basso verso l'alto */
	int nwins;
	bui_win scr;			/* solo per la lista dei rettangoli da ricomporre */
	bui_win *ptr;			/* finestra che riceve il tocco in corso */
	uint32_t last_present;
	int trace;
} K;

static int layer(const bui_win *w)
{
	switch (w->kind) {
	case BUI_WIN_DESKTOP: return 0;
	case BUI_WIN_DOCK: return 2;
	case BUI_WIN_POPUP: return 3;
	default: return 1;
	}
}

static bui_rect win_rect(const bui_win *w)
{
	return bui_r(w->x, w->y, w->w, w->h);
}

int bui_comp_screen_w(void) { return K.sw; }
int bui_comp_screen_h(void) { return K.sh; }

void bui_comp_damage_screen(bui_rect r)
{
	bui_win_damage(&K.scr, r);
}

int bui_comp_init(int sw, int sh, bui_px *target, int stride,
		  void (*present)(const bui_rect *, int))
{
	memset(&K, 0, sizeof(K));
	K.sw = sw;
	K.sh = sh;
	K.target = target;
	K.stride = stride;
	K.present = present;
	K.scr.w = sw;
	K.scr.h = sh;
	K.trace = getenv("BUI_TRACE") != NULL;
	return 0;
}

static void win_free(bui_win *w)
{
	free(w->cv.px);
	free(w->backdrop);
	free(w);
}

void bui_comp_shutdown(void)
{
	while (K.nwins)
		bui_comp_win_destroy(K.wins[K.nwins - 1]);
	K.target = NULL;
}

/* ------------------------------------------------------------------ */
/* composizione                                                       */
/* ------------------------------------------------------------------ */

/* pixel vuoti a sinistra/destra nella riga `ry` di un popup arrotondato */
static int round_inset(const bui_win *w, int ry)
{
	if (!w->radius)
		return 0;
	if (ry < w->radius)
		return bui_corner_inset(w->radius, ry);
	if (ry >= w->h - w->radius)
		return bui_corner_inset(w->radius, w->h - 1 - ry);
	return 0;
}

/*
 * Copia in dst (che corrisponde al rettangolo dello schermo con angolo
 * ox, oy) cio' che le finestre [0, upto) mostrano dentro r. Comincia dalla
 * finestra piu' alta che copre r per intero: i livelli sotto non servono.
 */
static void composite(bui_rect r, bui_px *dst, int dstride, int ox, int oy, int upto)
{
	int i, start = 0, y, found = 0;

	for (i = upto - 1; i >= 0; i--) {
		bui_win *w = K.wins[i];
		bui_rect k;

		if (w->mapped && !w->radius && bui_rect_isect(r, win_rect(w), &k) && k.w == r.w &&
		    k.h == r.h) {
			start = i;
			found = 1;
			break;
		}
	}
	if (!found) {
		/* niente sotto: nero (succede solo prima che la home esista) */
		for (y = r.y; y < r.y + r.h; y++)
			memset(dst + (y - oy) * dstride + (r.x - ox), 0, (size_t)r.w * sizeof(bui_px));
	}
	for (i = start; i < upto; i++) {
		bui_win *w = K.wins[i];
		bui_rect k;

		if (!w->mapped || !w->cv.px || !bui_rect_isect(r, win_rect(w), &k))
			continue;
		for (y = k.y; y < k.y + k.h; y++) {
			int ry = y - w->y, in = round_inset(w, ry);
			int x0 = k.x, x1 = k.x + k.w;

			if (x0 < w->x + in)
				x0 = w->x + in;
			if (x1 > w->x + w->w - in)
				x1 = w->x + w->w - in;
			if (x1 <= x0)
				continue;
			memcpy(dst + (y - oy) * dstride + (x0 - ox),
			       w->cv.px + ry * w->cv.stride + (x0 - w->x), (size_t)(x1 - x0) * sizeof(bui_px));
		}
	}
}

int bui_comp_bands(const bui_rect *r, int n, int sw, int gap, bui_rect *out)
{
	int i, j, m = 0;

	for (i = 0; i < n; i++) {
		int y0 = r[i].y, y1 = r[i].y + r[i].h;

		if (r[i].w <= 0 || r[i].h <= 0)
			continue;
		/* inserimento ordinato per riga iniziale */
		for (j = m; j > 0 && out[j - 1].y > y0; j--)
			out[j] = out[j - 1];
		out[j] = bui_r(0, y0, sw, y1 - y0);
		m++;
	}
	/* fonde le fasce che si toccano o sono vicine */
	for (i = 0, j = 0; i < m; i++) {
		if (j > 0 && out[i].y <= out[j - 1].y + out[j - 1].h + gap) {
			int e = out[i].y + out[i].h;

			if (e > out[j - 1].y + out[j - 1].h)
				out[j - 1].h = e - out[j - 1].y;
			continue;
		}
		out[j++] = out[i];
	}
	return j;
}

static int any_dirty(void)
{
	int i;

	if (K.scr.ndirty)
		return 1;
	for (i = 0; i < K.nwins; i++)
		if (K.wins[i]->mapped && K.wins[i]->ndirty)
			return 1;
	return 0;
}

static void paint_win(bui_win *w, uint32_t now)
{
	bui_rect rects[BUI_MAX_DIRTY];
	int i, n = w->ndirty;

	memcpy(rects, w->dirty, sizeof(rects));
	w->ndirty = 0;
	for (i = 0; i < n; i++) {
		w->cv.clip = rects[i];
		if (w->paint)
			w->paint(w, &w->cv, rects[i]);
		bui_comp_damage_screen(bui_r(rects[i].x + w->x, rects[i].y + w->y, rects[i].w, rects[i].h));
		if (K.trace)
			fprintf(stderr, "bui: put %lu %s %dx%d+%d+%d\n", (unsigned long)(now % 100000),
				w->kind == BUI_WIN_DOCK ? "bar" : w->kind == BUI_WIN_DESKTOP ? "home" : "popup",
				rects[i].w, rects[i].h, rects[i].x, rects[i].y);
	}
	w->cv.clip = bui_r(0, 0, w->w, w->h);
	w->last_flush = now ? now : 1;
}

int bui_comp_flush(uint32_t now)
{
	bui_rect rects[BUI_MAX_DIRTY], bands[BUI_MAX_DIRTY];
	int i, n, el;

	if (bui__suspended() || !K.target || !any_dirty())
		return -1;
	/* al passo del pannello: un fotogramma ogni ~30 ms; il primo dopo una
	 * pausa (il "premuto" di un tocco) parte subito */
	el = (int)(now - K.last_present);
	if (K.last_present && el >= 0 && el < BUI_FRAME_MS - 3)
		return BUI_FRAME_MS - 3 - el;
	for (i = 0; i < K.nwins; i++)
		if (K.wins[i]->mapped && K.wins[i]->ndirty)
			paint_win(K.wins[i], now);
	n = K.scr.ndirty;
	memcpy(rects, K.scr.dirty, sizeof(rects));
	K.scr.ndirty = 0;
	for (i = 0; i < n; i++)
		composite(rects[i], K.target, K.stride, 0, 0, K.nwins);
	n = bui_comp_bands(rects, n, K.sw, BAND_GAP, bands);
	if (n && K.present)
		K.present(bands, n);
	K.last_present = now ? now : 1;
	return -1;
}

/* ------------------------------------------------------------------ */
/* finestre                                                           */
/* ------------------------------------------------------------------ */

static int win_alloc(bui_win *w)
{
	free(w->cv.px);
	w->cv.px = calloc((size_t)w->w * w->h, sizeof(bui_px));
	if (!w->cv.px)
		return -1;
	w->cv.w = w->w;
	w->cv.h = w->h;
	w->cv.stride = w->w;
	w->cv.clip = bui_r(0, 0, w->w, w->h);
	w->own_px = 1;
	return 0;
}

static void insert(bui_win *w)
{
	int i = K.nwins;

	/* in cima al proprio livello */
	while (i > 0 && layer(K.wins[i - 1]) > layer(w)) {
		K.wins[i] = K.wins[i - 1];
		i--;
	}
	K.wins[i] = w;
	K.nwins++;
}

static void unlink_win(bui_win *w)
{
	int i;

	for (i = 0; i < K.nwins; i++)
		if (K.wins[i] == w) {
			memmove(&K.wins[i], &K.wins[i + 1], sizeof(K.wins[0]) * (size_t)(K.nwins - i - 1));
			K.nwins--;
			return;
		}
}

bui_win *bui_comp_win_create(int kind, bui_rect r, const char *name)
{
	bui_win *w;

	(void)name;
	if (K.nwins >= MAX_WINS || r.w <= 0 || r.h <= 0)
		return NULL;
	w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;
	w->x = r.x;
	w->y = r.y;
	w->w = r.w;
	w->h = r.h;
	w->kind = kind;
	w->input = kind == BUI_WIN_DESKTOP || kind == BUI_WIN_NORMAL;
	if (win_alloc(w) < 0) {
		free(w);
		return NULL;
	}
	insert(w);
	bui_win_damage_all(w);
	return w;
}

void bui_comp_win_destroy(bui_win *w)
{
	if (!w)
		return;
	unlink_win(w);
	if (w->mapped)
		bui_comp_damage_screen(win_rect(w));
	if (K.ptr == w)
		K.ptr = NULL;
	if (w->kind == BUI_WIN_POPUP)
		bui__popup_closed_now();
	win_free(w);
}

void bui_comp_win_show(bui_win *w)
{
	if (w->kind == BUI_WIN_POPUP) {
		/* in cima a tutti */
		unlink_win(w);
		K.wins[K.nwins++] = w;
	}
	w->mapped = 1;
	bui_comp_damage_screen(win_rect(w));
}

void bui_comp_win_hide(bui_win *w)
{
	if (w->mapped)
		bui_comp_damage_screen(win_rect(w));
	w->mapped = 0;
	if (K.ptr == w)
		K.ptr = NULL;
}

void bui_comp_win_move_resize(bui_win *w, bui_rect r)
{
	int resized = r.w != w->w || r.h != w->h;

	if (!resized && r.x == w->x && r.y == w->y)
		return;
	if (w->mapped)
		bui_comp_damage_screen(win_rect(w));
	w->x = r.x;
	w->y = r.y;
	if (resized) {
		w->w = r.w;
		w->h = r.h;
		w->ndirty = 0;
		if (win_alloc(w) < 0) {
			fprintf(stderr, "bui: memoria esaurita\n");
			exit(1);
		}
		bui_win_damage_all(w);
	}
	if (w->resized && (resized || w->kind != BUI_WIN_POPUP))
		w->resized(w);
	if (w->kind != BUI_WIN_POPUP && !resized)
		bui_win_damage_all(w);	/* come con X: la posizione conta per chi disegna */
	if (w->mapped)
		bui_comp_damage_screen(win_rect(w));
}

void bui_comp_win_set_strut_top(bui_win *w, int h)
{
	(void)w;	/* niente gestore di finestre: la home sa gia' dov'e' la barra */
	(void)h;
}

bui_win *bui_comp_popup_open(bui_rect r, unsigned flags)
{
	bui_win *w;
	int y;

	if (r.w > K.sw)
		r.w = K.sw;
	if (r.h > K.sh)
		r.h = K.sh;
	if (r.x + r.w > K.sw)
		r.x = K.sw - r.w;
	if (r.y + r.h > K.sh)
		r.y = K.sh - r.h;
	if (r.x < 0)
		r.x = 0;
	if (r.y < 0)
		r.y = 0;
	w = bui_comp_win_create(BUI_WIN_POPUP, r, "brainshell-popup");
	if (!w)
		return NULL;
	/* il fondo: cio' che le finestre sotto mostrano, ricomposto dalle loro
	 * tele (mai letto dalla superficie d'uscita) */
	w->backdrop = malloc((size_t)w->w * w->h * sizeof(bui_px));
	if (w->backdrop) {
		composite(r, w->backdrop, w->w, r.x, r.y, K.nwins - 1);
		if (flags & BUI_POPUP_DIM)
			bui__dim(w->backdrop, w->w, w->h, w->w);
		/* la tela parte dal fondo: un primo fotogramma parziale non mostra nero */
		for (y = 0; y < w->h; y++)
			memcpy(w->cv.px + y * w->cv.stride, w->backdrop + y * w->w,
			       (size_t)w->w * sizeof(bui_px));
	}
	bui_comp_win_show(w);
	if (flags & BUI_POPUP_MODAL) {
		w->grabbed = 1;
		w->input = 1;
		/* come XGrabPointer: il resto del tocco in corso va al popup */
		K.ptr = w;
	}
	return w;
}

bui_win *bui_comp_popup_open_round(bui_rect r, int radius)
{
	bui_win *w = bui_comp_win_create(BUI_WIN_POPUP, r, "brainshell-popup");

	if (!w)
		return NULL;
	if (radius > r.h / 2)
		radius = r.h / 2;
	if (radius > r.w / 2)
		radius = r.w / 2;
	if (radius > 64)
		radius = 64;
	w->radius = (uint8_t)(radius >= 2 ? radius : 0);
	bui_comp_win_show(w);
	return w;
}

void bui_comp_redraw_all(void)
{
	int i;

	for (i = 0; i < K.nwins; i++)
		bui_win_damage_all(K.wins[i]);
	bui_comp_damage_screen(bui_r(0, 0, K.sw, K.sh));
}

int bui_comp_popups(void)
{
	int i, n = 0;

	for (i = 0; i < K.nwins; i++)
		n += K.wins[i]->kind == BUI_WIN_POPUP && K.wins[i]->mapped;
	return n;
}

/* ------------------------------------------------------------------ */
/* ingresso                                                           */
/* ------------------------------------------------------------------ */

static bui_win *grab_target(void)
{
	int i;

	for (i = K.nwins - 1; i >= 0; i--)
		if (K.wins[i]->mapped && K.wins[i]->grabbed)
			return K.wins[i];
	return NULL;
}

static bui_win *window_at(int x, int y)
{
	int i;

	for (i = K.nwins - 1; i >= 0; i--)
		if (K.wins[i]->mapped && bui_rect_has(win_rect(K.wins[i]), x, y))
			return K.wins[i];
	return NULL;
}

static void deliver(bui_win *w, int type, int x, int y, uint32_t t)
{
	bui_pointer p;

	memset(&p, 0, sizeof(p));
	p.type = type;
	p.x = x - w->x;
	p.y = y - w->y;
	p.time = t;
	if (w->pointer)
		w->pointer(w, &p);
}

void bui_comp_pointer(int type, int x, int y, uint32_t t)
{
	bui_win *w;

	if (type == BUI_PRESS) {
		w = grab_target();
		if (!w)
			w = window_at(x, y);
		/* il filtro puo' scartare il tocco (schermo spento: il primo sveglia soltanto) */
		if (bui__input_filter(type, NULL, 0)) {
			if (w && w->down) {
				w->down = 0;
				deliver(w, BUI_CANCEL, x, y, t);
			}
			K.ptr = NULL;
			return;
		}
		if (!w)
			return;
		if (w->down)	/* rilascio perso: chiude il tocco precedente */
			deliver(w, BUI_CANCEL, x, y, t);
		w->down = 1;
		K.ptr = w;
		deliver(w, BUI_PRESS, x, y, t);
		return;
	}
	if (bui__input_filter(type, NULL, 0))
		return;
	w = K.ptr;
	if (!w || !w->down)
		return;
	if (type == BUI_RELEASE) {
		K.ptr = NULL;
		w->down = 0;
	}
	deliver(w, type, x, y, t);
}

void bui_comp_cancel_pointer(void)
{
	bui_win *w = K.ptr;

	K.ptr = NULL;
	if (w && w->down) {
		w->down = 0;
		deliver(w, BUI_CANCEL, w->x, w->y, bui_now());
	}
}

void bui_comp_key(const bui_key *k)
{
	bui_win *w = grab_target();
	int i;

	if (bui__input_filter(BUI_IN_KEY, k, 0))
		return;
	/* la tastiera va al popup modale piu' alto, altrimenti alla home */
	for (i = K.nwins - 1; !w && i >= 0; i--)
		if (K.wins[i]->mapped && K.wins[i]->input && K.wins[i]->kind == BUI_WIN_DESKTOP)
			w = K.wins[i];
	for (i = K.nwins - 1; !w && i >= 0; i--)
		if (K.wins[i]->mapped && K.wins[i]->input)
			w = K.wins[i];
	if (w && w->key)
		w->key(w, k);
}
