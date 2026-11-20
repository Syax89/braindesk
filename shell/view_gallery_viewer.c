// SPDX-License-Identifier: MIT
/*
 * view_gallery_viewer.c - visualizzatore di immagini a tutto schermo.
 *
 * L'immagine viene decodificata dal filo di lavoro gia' ridotta alla misura
 * dell'area (bs_img_load): in memoria restano al massimo tre immagini da
 * schermo (precedente, corrente, successiva: ~750 kB l'una), cosi' sfogliare
 * avanti e indietro e' immediato. Chi cambia immagine prima che la
 * decodifica finisca la annulla.
 *
 * Tocco: un colpetto mostra/nasconde la barra col nome; uno strisciamento
 * orizzontale cambia immagine. Tasti: frecce, Inizio/Fine, Invio (barra),
 * Canc (elimina), Esc/Backspace (indietro).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bs_img.h"
#include "bs_work.h"
#include "view_gallery.h"

#define NSLOT 3
#define BAR_H 56
#define SWIPE 48

enum { ST_FREE, ST_LOADING, ST_READY, ST_ERROR };
enum { B_NONE = -1, B_BACK, B_DELETE, B_PREV, B_NEXT, B_COUNT };

struct slot {
	int state, index, job, err;
	bs_img img;
};

struct vjob {
	char path[1024];
	int bw, bh, index, gen, rc;
	bs_img img;
};

static struct {
	int active, gen;
	bui_win *win;
	bui_rect area;
	bs_viewer_src src;
	int cur, n, dir;
	int overlay;
	struct slot slot[NSLOT];
	int spin_timer, spin;
	/* tocco */
	int down, moved, px, py, btn, btn_in;
	char name[256], info[64];
} V;

static void go(int index);

/* ------------------------------------------------------------------ */
/* geometria                                                          */
/* ------------------------------------------------------------------ */

static bui_rect btn_rect(int b)
{
	bui_rect a = V.area;

	switch (b) {
	case B_BACK:
		return bui_r(a.x, a.y, 64, BAR_H);
	case B_DELETE:
		return bui_r(a.x + a.w - 64, a.y, 64, BAR_H);
	case B_PREV:
		return bui_r(a.x + 8, a.y + (a.h - 64) / 2 + BAR_H / 4, 64, 64);
	default:
		return bui_r(a.x + a.w - 72, a.y + (a.h - 64) / 2 + BAR_H / 4, 64, 64);
	}
}

static int btn_enabled(int b)
{
	if (b == B_PREV)
		return V.cur > 0;
	if (b == B_NEXT)
		return V.cur < V.n - 1;
	return 1;
}

static bui_rect spin_rect(void)
{
	return bui_r(V.area.x + V.area.w / 2 - 32, V.area.y + V.area.h / 2 - 32, 64, 64);
}

static void damage_overlay(void)
{
	int b;

	bs_view_damage(bui_r(V.area.x, V.area.y, V.area.w, BAR_H));
	for (b = B_PREV; b <= B_NEXT; b++)
		bs_view_damage(btn_rect(b));
}

/* ------------------------------------------------------------------ */
/* immagini in memoria e lavori                                       */
/* ------------------------------------------------------------------ */

static struct slot *slot_find(int index)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		if (V.slot[i].state != ST_FREE && V.slot[i].index == index)
			return &V.slot[i];
	return NULL;
}

static void slot_drop(struct slot *s)
{
	if (s->state == ST_LOADING)
		bs_work_cancel(s->job);
	bs_img_free(&s->img);
	memset(s, 0, sizeof(*s));
}

static void slots_clear(void)
{
	int i;

	for (i = 0; i < NSLOT; i++)
		slot_drop(&V.slot[i]);
}

static void spin_stop(void)
{
	if (V.spin_timer)
		bui_timer_del(V.spin_timer);
	V.spin_timer = 0;
	V.spin = 0;
}

static void spin_tick(void *user)
{
	(void)user;
	V.spin++;
	bs_view_damage(spin_rect());
}

static void set_info(void)
{
	const struct slot *s = slot_find(V.cur);
	const char *p = V.src.path(V.cur, V.src.user), *b;
	int n;

	b = p ? strrchr(p, '/') : NULL;
	snprintf(V.name, sizeof(V.name), "%s", p ? (b ? b + 1 : p) : "");
	n = snprintf(V.info, sizeof(V.info), T("%d di %d"), V.cur + 1, V.n);
	if (s && s->state == ST_READY && n > 0 && n < (int)sizeof(V.info))
		snprintf(V.info + n, sizeof(V.info) - (size_t)n, "  \xC2\xB7  %d\xC3\x97%d", s->img.src_w,
			 s->img.src_h);
}

static void job_run(void *arg, const volatile int *cancel)
{
	struct vjob *j = arg;

	j->rc = bs_img_load(j->path, j->bw, j->bh, BS_IMG_FIT, cancel, &j->img);
}

static void preload(void);

static void job_done(void *arg, int cancelled)
{
	struct vjob *j = arg;
	struct slot *s;

	if (cancelled || !V.active || j->gen != V.gen || !(s = slot_find(j->index)) ||
	    s->state != ST_LOADING) {
		bs_img_free(&j->img);
		free(j);
		return;
	}
	s->job = 0;
	if (j->rc == BS_IMG_OK) {
		s->state = ST_READY;
		s->img = j->img;
	} else {
		s->state = ST_ERROR;
		s->err = j->rc;
		bs_img_free(&j->img);
	}
	if (j->index == V.cur) {
		spin_stop();
		set_info();
		bs_view_damage(V.area);
		preload();
	}
	free(j);
}

static struct slot *slot_load(int index, int front)
{
	struct slot *s = slot_find(index);
	const char *p;
	struct vjob *j;
	int i;

	if (s)
		return s;
	for (i = 0; i < NSLOT && !s; i++)
		if (V.slot[i].state == ST_FREE)
			s = &V.slot[i];
	p = V.src.path(index, V.src.user);
	if (!s || !p)
		return NULL;
	s->index = index;
	s->state = ST_ERROR;
	s->err = BS_IMG_E_MEM;
	j = calloc(1, sizeof(*j));
	if (!j)
		return s;
	snprintf(j->path, sizeof(j->path), "%s", p);
	j->bw = V.area.w;
	j->bh = V.area.h;
	j->index = index;
	j->gen = V.gen;
	s->job = bs_work_submit(job_run, job_done, j, front);
	if (!s->job) {
		free(j);
		return s;
	}
	s->state = ST_LOADING;
	return s;
}

/* prepara la vicina nel verso in cui si sta sfogliando */
static void preload(void)
{
	int nb = V.cur + (V.dir < 0 ? -1 : 1);

	if (nb < 0 || nb >= V.n)
		nb = V.cur - (V.dir < 0 ? -1 : 1);
	if (nb >= 0 && nb < V.n && nb != V.cur)
		slot_load(nb, 0);
}

static void go(int index)
{
	struct slot *s;
	int i;

	if (index < 0)
		index = 0;
	if (index >= V.n)
		index = V.n - 1;
	V.dir = index < V.cur ? -1 : 1;
	V.cur = index;
	/* restano solo la corrente e le due vicine */
	for (i = 0; i < NSLOT; i++)
		if (V.slot[i].state != ST_FREE && abs(V.slot[i].index - index) > 1)
			slot_drop(&V.slot[i]);
	s = slot_find(index);
	if (!s) {
		/* serve un posto: via la vicina dal lato opposto al verso */
		for (i = 0; i < NSLOT; i++)
			if (V.slot[i].state == ST_FREE)
				break;
		if (i == NSLOT) {
			s = slot_find(index - V.dir);
			slot_drop(s ? s : &V.slot[0]);
		}
		s = slot_load(index, 1);
	}
	spin_stop();
	if (s && s->state == ST_LOADING)
		V.spin_timer = bui_timer_add(120, 120, spin_tick, NULL);
	else
		preload();
	set_info();
	bs_view_damage(V.area);
}

/* ------------------------------------------------------------------ */
/* apertura, chiusura                                                 */
/* ------------------------------------------------------------------ */

void bs_viewer_open(bui_win *win, bui_rect area, const bs_viewer_src *src, int index)
{
	if (V.active)
		bs_viewer_close();
	V.active = 1;
	V.gen++;
	V.win = win;
	V.area = area;
	V.src = *src;
	V.n = src->count(src->user);
	V.overlay = 1;
	V.down = 0;
	V.btn = B_NONE;
	V.cur = index;
	go(index);
	V.dir = 1;
}

void bs_viewer_close(void)
{
	if (!V.active)
		return;
	spin_stop();
	slots_clear();
	V.active = 0;
	V.gen++;
}

int bs_viewer_active(void)
{
	return V.active;
}

void bs_viewer_layout(bui_rect area)
{
	int same = area.w == V.area.w && area.h == V.area.h;

	V.area = area;
	if (!V.active || same)
		return;
	/* altra misura: le immagini vanno decodificate di nuovo */
	V.gen++;
	slots_clear();
	go(V.cur);
}

static void back(void)
{
	bs_viewer_src src = V.src;
	int idx = V.n > 0 ? V.cur : -1;

	bs_viewer_close();
	if (src.closed)
		src.closed(idx, src.user);
}

/* ------------------------------------------------------------------ */
/* eliminazione                                                       */
/* ------------------------------------------------------------------ */

static void delete_done(int button, void *user)
{
	const char *p;
	int gen = (int)(intptr_t)user;

	if (button != 1 || !V.active || gen != V.gen)
		return;
	p = V.src.path(V.cur, V.src.user);
	if (!p || unlink(p) < 0) {
		bui_toast("Impossibile eliminare l'immagine", 2500);
		return;
	}
	if (V.src.removed)
		V.src.removed(V.cur, V.src.user);
	V.n = V.src.count(V.src.user);
	if (V.n <= 0) {
		back();
		bui_toast("Immagine eliminata", 1800);
		return;
	}
	/* gli indici dopo quello tolto sono scalati: si riparte puliti */
	V.gen++;
	slots_clear();
	go(V.cur);
	bui_toast("Immagine eliminata", 1800);
}

static void ask_delete(void)
{
	static char msg[320];
	bui_dialog d = {
		.title = "Eliminare l'immagine?",
		.message = msg,
		.buttons = { "Annulla", "Elimina" },
		.styles = { BUI_BS_TONAL, BUI_BS_DANGER },
		.cancel = 0,
		.done = delete_done,
		.user = (void *)(intptr_t)V.gen,
	};

	snprintf(msg, sizeof(msg), T("\xC2\xAB%s\xC2\xBB verr\xC3\xA0 eliminata definitivamente."), V.name);
	bui_dialog_open(&d);
}

/* ------------------------------------------------------------------ */
/* disegno                                                            */
/* ------------------------------------------------------------------ */

static void draw_spinner(bui_canvas *c)
{
	static const int8_t pos[8][2] = { { 0, -18 }, { 13, -13 }, { 18, 0 }, { 13, 13 },
					   { 0, 18 }, { -13, 13 }, { -18, 0 }, { -13, -13 } };
	bui_rect r = spin_rect();
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2, i;

	for (i = 0; i < 8; i++) {
		int age = (V.spin - i) & 7;

		bui_rrect_a(c, bui_r(cx + pos[i][0] - 4, cy + pos[i][1] - 4, 8, 8), 4, BUI_C_TEXT,
			    age == 0 ? 255 : 200 - age * 24);
	}
}

static void draw_button(bui_canvas *c, int b, const bui_icon *ic, bui_px col)
{
	bui_rect r = btn_rect(b);
	int pressed = V.btn == b && V.btn_in;
	int d = 48;
	bui_rect k = bui_r(r.x + (r.w - d) / 2, r.y + (r.h - d) / 2, d, d);

	if (b >= B_PREV)
		bui_rrect_a(c, k, d / 2, BUI_C_BAR, pressed ? 230 : 150);
	else if (pressed)
		bui_rrect_a(c, k, d / 2, BUI_C_TEXT, 50);
	bui_icon_in(c, ic, r, col);
}

void bs_viewer_draw(bui_canvas *c, bui_rect clip)
{
	const struct slot *s = slot_find(V.cur);
	bui_rect a = V.area, im = bui_r(a.x, a.y, 0, 0);
	const bui_px black = 0;

	(void)clip;
	if (s && s->state == ST_READY && s->img.px) {
		im = bui_r(a.x + (a.w - s->img.w) / 2, a.y + (a.h - s->img.h) / 2, s->img.w, s->img.h);
		/* nero solo attorno all'immagine */
		bui_fill(c, bui_r(a.x, a.y, a.w, im.y - a.y), black);
		bui_fill(c, bui_r(a.x, im.y + im.h, a.w, a.y + a.h - im.y - im.h), black);
		bui_fill(c, bui_r(a.x, im.y, im.x - a.x, im.h), black);
		bui_fill(c, bui_r(im.x + im.w, im.y, a.x + a.w - im.x - im.w, im.h), black);
		bui_blit(c, im.x, im.y, s->img.px, s->img.w, s->img.h, s->img.w);
	} else {
		bui_fill(c, a, black);
		if (s && s->state == ST_ERROR) {
			int cy = a.y + a.h / 2;

			bui_icon_in(c, BUI_IC(broken, 40), bui_r(a.x, cy - 52, a.w, 40), BUI_C_TEXT3);
			bui_text_box(c, BUI_FONT_BODY, bui_r(a.x + 16, cy, a.w - 32, 28),
				     bs_img_strerror(s->err), BUI_C_TEXT2, BUI_AL_CENTER);
		} else if (V.spin > 0) {
			draw_spinner(c);
		}
	}
	if (!V.overlay)
		return;
	bui_fill_a(c, bui_r(a.x, a.y, a.w, BAR_H), BUI_C_BAR, 190);
	draw_button(c, B_BACK, BUI_IC(back, 24), BUI_C_TEXT);
	draw_button(c, B_DELETE, BUI_IC(trash, 24), BUI_C_TEXT);
	bui_text_box(c, BUI_FONT_BODY_B, bui_r(a.x + 68, a.y + 7, a.w - 68 - 68, 22), V.name, BUI_C_TEXT,
		     BUI_AL_LEFT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(a.x + 68, a.y + 30, a.w - 68 - 68, 20), V.info, BUI_C_TEXT2,
		     BUI_AL_LEFT);
	if (btn_enabled(B_PREV))
		draw_button(c, B_PREV, BUI_IC(prev, 24), BUI_C_TEXT);
	if (btn_enabled(B_NEXT))
		draw_button(c, B_NEXT, BUI_IC(chevron, 24), BUI_C_TEXT);
}

/* ------------------------------------------------------------------ */
/* ingresso                                                           */
/* ------------------------------------------------------------------ */

static void btn_action(int b)
{
	switch (b) {
	case B_BACK:
		back();
		break;
	case B_DELETE:
		ask_delete();
		break;
	case B_PREV:
		if (V.cur > 0)
			go(V.cur - 1);
		break;
	case B_NEXT:
		if (V.cur < V.n - 1)
			go(V.cur + 1);
		break;
	}
}

int bs_viewer_pointer(const bui_pointer *ev)
{
	int b, dx, dy, in;

	if (!V.active)
		return 0;
	switch (ev->type) {
	case BUI_PRESS:
		V.down = 1;
		V.moved = 0;
		V.px = ev->x;
		V.py = ev->y;
		V.btn = B_NONE;
		for (b = 0; V.overlay && b < B_COUNT; b++)
			if (btn_enabled(b) && bui_rect_has(btn_rect(b), ev->x, ev->y)) {
				V.btn = b;
				V.btn_in = 1;
				bs_view_damage(btn_rect(b));
			}
		return 1;
	case BUI_MOVE:
		if (!V.down)
			return 0;
		dx = ev->x - V.px;
		dy = ev->y - V.py;
		if (abs(dx) > BUI_DRAG_SLOP || abs(dy) > BUI_DRAG_SLOP)
			V.moved = 1;
		if (V.btn != B_NONE) {
			in = bui_rect_has(bui_rect_inset(btn_rect(V.btn), -BUI_DRAG_SLOP, -BUI_DRAG_SLOP),
					  ev->x, ev->y);
			if (in != V.btn_in) {
				V.btn_in = in;
				bs_view_damage(btn_rect(V.btn));
			}
		}
		return 1;
	case BUI_RELEASE:
	case BUI_CANCEL:
		if (!V.down)
			return 0;
		V.down = 0;
		b = V.btn;
		V.btn = B_NONE;
		if (b != B_NONE) {
			bs_view_damage(btn_rect(b));
			if (ev->type == BUI_RELEASE && V.btn_in)
				btn_action(b);	/* puo' chiudere il visualizzatore */
			return 1;
		}
		if (ev->type != BUI_RELEASE)
			return 1;
		dx = ev->x - V.px;
		dy = ev->y - V.py;
		if (abs(dx) >= SWIPE && abs(dx) > abs(dy)) {
			if (dx < 0 && V.cur < V.n - 1)
				go(V.cur + 1);
			else if (dx > 0 && V.cur > 0)
				go(V.cur - 1);
		} else if (!V.moved) {
			V.overlay = !V.overlay;
			damage_overlay();
		}
		return 1;
	case BUI_WHEEL:
		if (ev->dy > 0 && V.cur < V.n - 1)
			go(V.cur + 1);
		else if (ev->dy < 0 && V.cur > 0)
			go(V.cur - 1);
		return 1;
	}
	return 0;
}

int bs_viewer_key(const bui_key *k)
{
	if (!V.active)
		return 0;
	switch (k->key) {
	case BUI_K_LEFT:
	case BUI_K_UP:
	case BUI_K_PGUP:
		if (V.cur > 0)
			go(V.cur - 1);
		return 1;
	case BUI_K_RIGHT:
	case BUI_K_DOWN:
	case BUI_K_PGDN:
		if (V.cur < V.n - 1)
			go(V.cur + 1);
		return 1;
	case BUI_K_HOME:
		if (V.cur != 0)
			go(0);
		return 1;
	case BUI_K_END:
		if (V.cur != V.n - 1)
			go(V.n - 1);
		return 1;
	case BUI_K_ENTER:
	case ' ':
		V.overlay = !V.overlay;
		damage_overlay();
		return 1;
	case BUI_K_DELETE:
		ask_delete();
		return 1;
	case BUI_K_ESC:
	case BUI_K_BACKSPACE:
		back();
		return 1;
	}
	return 1;
}
