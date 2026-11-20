// SPDX-License-Identifier: MIT
/*
 * bui_w.c - widget, pannelli, scorrimento cinetico, liste, dialoghi, toast.
 *
 * Un pannello e' un contenitore di widget con un'area visibile; se il
 * contenuto e' piu' alto scorre in verticale con inerzia. Il pannello
 * distingue tocco e trascinamento, tiene lo stato "premuto" e chiede il
 * ridisegno del solo widget interessato.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bui.h"

enum { MODE_NONE, MODE_SCROLL, MODE_SLIDER, MODE_LIST };
enum { FX_NONE, FX_RELEASE, FX_VALUE };

#define KNOB 28
#define OVER_MAX 120		/* px di elastico oltre il limite */
#define SPRING_MS 220		/* ritorno dell'elastico */
#define FX_RELEASE_MS 150	/* dissolvenza del "premuto" */
#define FX_VALUE_MS 130		/* cursori, interruttori, selettori */
#define LONG_PRESS_MS 550
#define SB_ALPHA 110		/* opacita' piena della barra di scorrimento */
#define SB_HOLD_MS 700		/* resta visibile dopo l'ultimo movimento */

/* ------------------------------------------------------------------ */
/* scorrimento cinetico                                               */
/* ------------------------------------------------------------------ */

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void bui_scroll_grab(bui_scroll *s, int y, uint32_t t)
{
	s->fling = 0;
	s->spring = 0;
	s->vel = 0;
	s->frac = 0;
	s->grab_pos = s->pos;
	s->grab_y = y;
	s->hn = 1;
	s->hy[0] = y;
	s->ht[0] = t;
}

int bui_scroll_drag(bui_scroll *s, int y, uint32_t t)
{
	int raw = s->grab_pos - (y - s->grab_y);
	int np = clampi(raw, 0, s->max);
	/* oltre il limite il contenuto segue il dito a un terzo della velocita' */
	int ov = s->elastic ? clampi((raw - np) / 3, -OVER_MAX, OVER_MAX) : 0;
	int changed = np != s->pos || ov != s->over;

	if (s->hn == 4) {
		memmove(s->hy, s->hy + 1, 3 * sizeof(s->hy[0]));
		memmove(s->ht, s->ht + 1, 3 * sizeof(s->ht[0]));
		s->hn = 3;
	}
	s->hy[s->hn] = y;
	s->ht[s->hn] = t;
	s->hn++;
	s->pos = np;
	s->over = ov;
	return changed;
}

int bui_scroll_release(bui_scroll *s, uint32_t t)
{
	int i, last = s->hn - 1, first = last;
	int dt, v;

	s->fling = 0;
	if (s->over) {
		/* rilasciato oltre il limite: torna indietro con una molla */
		s->over0 = s->over;
		s->last = bui_now();
		s->vel = 0;
		s->spring = 1;
		s->fling = 1;
		return 1;
	}
	if (last < 1 || (int32_t)(t - s->ht[last]) > 90)
		return 0;	/* il dito si e' fermato prima di alzarsi */
	for (i = last - 1; i >= 0; i--)
		if ((int32_t)(s->ht[last] - s->ht[i]) <= 130)
			first = i;
	dt = (int32_t)(s->ht[last] - s->ht[first]);
	if (dt < 8)
		return 0;
	v = -(s->hy[last] - s->hy[first]) * 1000 / dt;
	v = clampi(v, -4500, 4500);
	if (v > -150 && v < 150)
		return 0;
	if ((v < 0 && s->pos <= 0) || (v > 0 && s->pos >= s->max))
		return 0;
	s->vel = v;
	s->frac = 0;
	s->last = bui_now();
	s->fling = 1;
	return 1;
}

int bui_scroll_step(bui_scroll *s, uint32_t now)
{
	int dt = (int32_t)(now - s->last);
	int d, np;

	if (!s->fling)
		return 0;
	if (s->spring) {
		int t = bui_tween(s->last, SPRING_MS, now);
		int ov = t >= 1024 ? 0 : s->over0 - s->over0 * bui_ease_out(t) / 1024;

		if (t >= 1024)
			s->spring = s->fling = 0;
		if (ov == s->over)
			return 0;
		s->over = ov;
		return 1;
	}
	dt = clampi(dt, 1, 100);
	s->last = now;
	d = s->vel * dt * 1024 / 1000 + s->frac;	/* 1/1024 px */
	np = s->pos + (d >> 10);			/* >> su negativi: arrotonda verso il basso */
	s->frac = d & 1023;
	s->vel -= s->vel * dt / 360;			/* attrito esponenziale, ~0,36 s */
	if (np <= 0 || np >= s->max) {
		np = clampi(np, 0, s->max);
		s->fling = 0;
	}
	if (s->vel > -45 && s->vel < 45)
		s->fling = 0;
	if (np == s->pos)
		return 0;
	s->pos = np;
	return 1;
}

int bui_scroll_by(bui_scroll *s, int dy)
{
	int np = clampi(s->pos + dy, 0, s->max);
	int was_over = s->over;

	s->fling = s->spring = 0;
	s->over = 0;
	if (np == s->pos && !was_over)
		return 0;
	s->pos = np;
	return 1;
}

/* ------------------------------------------------------------------ */
/* disegno dei widget                                                 */
/* ------------------------------------------------------------------ */

static void draw_label(bui_widget *w, bui_canvas *c, bui_rect r)
{
	const bui_font *f = BUI_FONT_BODY;
	bui_px col = BUI_C_TEXT;

	switch (w->style) {
	case BUI_LS_SMALL:
		f = BUI_FONT_SMALL;
		col = BUI_C_TEXT2;
		break;
	case BUI_LS_TITLE:
		f = BUI_FONT_TITLE;
		break;
	case BUI_LS_SECTION:
		f = BUI_FONT_SMALL_B;
		col = BUI_C_ACCENT;
		break;
	case BUI_LS_WRAP:
		col = BUI_C_TEXT2;
		break;
	}
	if (w->font)
		f = w->font;
	if (w->color)
		col = w->color;
	if (w->flags & BUI_F_DISABLED)
		col = BUI_C_TEXT3;
	if (w->icon && !w->text)
		bui_icon_in(c, w->icon, r, col);
	if (!w->text)
		return;
	if (w->style == BUI_LS_WRAP)
		bui_text_wrap(c, f, r, w->text, col, f->size + 8, w->align);
	else
		bui_text_box(c, f, r, w->text, col, w->align);
}

/* immagine da cui i riquadri "vetro" copiano il fondo */
static const bui_px *glass_px;
static int glass_w, glass_h, glass_stride;

void bui_glass_set(const bui_px *px, int w, int h, int stride)
{
	glass_px = px;
	glass_w = w;
	glass_h = h;
	glass_stride = stride;
}

/* quanto e' "premuto" un widget: 255 sotto il dito, poi sfuma dopo il rilascio */
static int press_level(const bui_widget *w)
{
	const bui_panel *p = w->panel;

	if (w->flags & BUI_F_PRESSED)
		return 255;
	if (p && p->fx_w == w && p->fx_kind == FX_RELEASE)
		return p->fx_val;
	return 0;
}

/* valore mostrato (x256) di cursori, interruttori e selettori: durante
 * l'animazione sta fra il vecchio e il nuovo */
static int shown_value(const bui_widget *w, int target)
{
	const bui_panel *p = w->panel;

	return p && p->fx_w == w && p->fx_kind == FX_VALUE ? p->fx_val : target;
}

static bui_px pmix(bui_px normal, bui_px pressed, int level)
{
	return level <= 0 ? normal : level >= 255 ? pressed : bui_mix(normal, pressed, level);
}

static void draw_button(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int L = press_level(w), sel = w->flags & BUI_F_SELECTED;
	int dis = w->flags & BUI_F_DISABLED;
	const bui_font *f = BUI_FONT_BODY_B;
	bui_px bg = BUI_C_SURFACE2, fg = BUI_C_TEXT, ring = BUI_C_ACCENT;
	int bg_alpha = 255, rad = r.h / 2 < bui_th.r_btn ? r.h / 2 : bui_th.r_btn;

	switch (w->style) {
	case BUI_BS_TONAL:
		bg = pmix(sel ? BUI_C_ACCENT_BG : BUI_C_SURFACE2, BUI_C_SURFACE3, L);
		if (sel)
			fg = BUI_C_ACCENT;
		break;
	case BUI_BS_FILLED:
		bg = pmix(BUI_C_ACCENT, BUI_C_ACCENT_P, L);
		fg = BUI_C_ON_ACCENT;
		ring = BUI_C_TEXT;
		break;
	case BUI_BS_DANGER:
		bg = pmix(BUI_C_DANGER, BUI_C_DANGER_P, L);
		fg = BUI_C_ON_DANGER;
		ring = BUI_C_TEXT;
		break;
	case BUI_BS_GHOST:
		bg_alpha = L;
		break;
	case BUI_BS_KEY:
		bg = pmix(BUI_C_SURFACE, BUI_C_SURFACE3, L);
		f = BUI_FONT_KEY;
		break;
	case BUI_BS_KEY_OP:
		bg = pmix(BUI_C_SURFACE2, BUI_C_SURFACE3, L);
		fg = BUI_C_ACCENT;
		f = BUI_FONT_KEY;
		break;
	case BUI_BS_SEG:
		if (sel) {
			bg = pmix(BUI_C_ACCENT_BG, BUI_C_SURFACE3, L);
		} else {
			bg = BUI_C_SURFACE3;
			bg_alpha = L;
		}
		fg = sel ? BUI_C_ACCENT : BUI_C_TEXT2;
		break;
	case BUI_BS_ROUND:
		bg_alpha = 0;
		if (L || (w->flags & BUI_F_FOCUS)) {
			/* cerchio centrato: l'area di tocco puo' essere piu' grande */
			int d = (r.w < r.h ? r.w : r.h) - 8;

			bui_rrect_a(c, bui_r(r.x + (r.w - d) / 2, r.y + (r.h - d) / 2, d, d), d / 2,
				    BUI_C_SURFACE2, (w->flags & BUI_F_FOCUS) ? 255 : L);
		}
		break;
	}
	if (w->font)
		f = w->font;
	if (w->color)
		fg = w->color;
	if (dis) {
		fg = BUI_C_TEXT3;
		if (w->style == BUI_BS_FILLED || w->style == BUI_BS_DANGER)
			bg = BUI_C_SURFACE2;
	}
	if (bg_alpha)
		bui_rrect_a(c, r, rad, bg, bg_alpha);

	if (w->icon && (!w->text || !*w->text)) {
		bui_icon_in(c, w->icon, r, fg);
	} else if (w->icon && (w->flags & BUI_F_ICON_TOP)) {
		int ch = w->icon->size + 8 + f->cap_h;
		int y = r.y + (r.h - ch) / 2;

		bui_icon_draw(c, w->icon, r.x + (r.w - w->icon->size) / 2, y, fg);
		bui_text_box(c, f, bui_r(r.x + 4, y + w->icon->size + 8, r.w - 8, f->cap_h), w->text,
			     fg, BUI_AL_CENTER);
	} else if (w->icon) {
		int tw = bui_text_width(f, w->text, -1);
		int tot = w->icon->size + 8 + tw;
		int x = r.x + (r.w - tot) / 2;

		if (x < r.x + 8)
			x = r.x + 8;
		bui_icon_draw(c, w->icon, x, r.y + (r.h - w->icon->size) / 2, fg);
		x += w->icon->size + 8;
		bui_text_box(c, f, bui_r(x, r.y, r.x + r.w - 8 - x, r.h), w->text, fg, BUI_AL_LEFT);
	} else if (w->text) {
		bui_text_box(c, f, bui_rect_inset(r, 8, 0), w->text, fg, BUI_AL_CENTER);
	}
	if ((w->flags & BUI_F_FOCUS) && w->style != BUI_BS_ROUND)
		bui_rrect_line(c, r, rad, 2, ring);
}

static int tile_style(const bui_widget *w)
{
	int st = bui_th.tile_style;

	if (st == BUI_TILE_GLASS && !(w->panel && w->panel->glass && glass_px))
		st = BUI_TILE_FILLED;
	return st;
}

static void draw_tile(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int L = press_level(w), dim = w->flags & BUI_F_DIM;
	int d = r.h >= 136 ? 72 : 56;
	const bui_font *f = w->font ? w->font : BUI_FONT_SMALL_B;
	int ch = d + 14 + f->cap_h;
	int y = r.y + (r.h - ch) / 2;
	int rad = bui_th.r_tile;
	bui_rect badge = bui_r(r.x + (r.w - d) / 2, y, d, d);
	/* premuto il riquadro si stringe di poco: e' la sua "molla" */
	bui_rect body = bui_rect_inset(r, L * 3 / 255, L * 3 / 255);
	/* tinta dell'icona: quella assegnata (tema a piu' colori) o l'accento */
	bui_px tint = w->color ? w->color : BUI_C_ACCENT;

	switch (tile_style(w)) {
	case BUI_TILE_GLASS:
		bui_blit_rr(c, body, rad, glass_px, glass_w, glass_h, glass_stride);
		if (L)
			bui_rrect_a(c, body, rad, BUI_C_TEXT, L * 36 / 255);
		bui_rrect_line(c, body, rad, 1, bui_mix(BUI_C_SURFACE3, BUI_C_TEXT, 56));
		break;
	case BUI_TILE_OUTLINE:
		if (L)
			bui_rrect_a(c, body, rad, BUI_C_SURFACE3, L);
		bui_rrect_line(c, body, rad, 1, L ? BUI_C_TEXT3 : BUI_C_SURFACE3);
		break;
	default:
		bui_rrect(c, body, rad, pmix(BUI_C_SURFACE, BUI_C_SURFACE3, L));
		break;
	}
	bui_rrect(c, badge, d / 2, dim ? BUI_C_SURFACE2 : w->color ? bui_mix(BUI_C_SURFACE, tint, 52)
								   : BUI_C_ACCENT_BG);
	bui_icon_in(c, w->icon, badge, dim ? BUI_C_TEXT2 : tint);
	bui_text_box(c, f, bui_r(r.x + 6, y + d + 14, r.w - 12, f->cap_h), w->text,
		     dim ? BUI_C_TEXT2 : BUI_C_TEXT, BUI_AL_CENTER);
	if (w->flags & BUI_F_FOCUS)
		bui_rrect_line(c, r, rad, 2, BUI_C_ACCENT);
}

static void slider_span(bui_rect r, int *x0, int *x1)
{
	*x0 = r.x + KNOB / 2 + 8;
	*x1 = r.x + r.w - KNOB / 2 - 8;
	if (*x1 <= *x0)
		*x1 = *x0 + 1;
}

static void draw_slider(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int x0, x1, kx, cy = r.y + r.h / 2;
	int range = w->vmax > w->vmin ? w->vmax - w->vmin : 1;
	int dis = w->flags & BUI_F_DISABLED;
	bui_px col = dis ? BUI_C_TEXT3 : BUI_C_ACCENT;

	slider_span(r, &x0, &x1);
	kx = x0 + (x1 - x0) * (shown_value(w, clampi(w->value, w->vmin, w->vmax) * 256) - w->vmin * 256) /
		  (range * 256);
	bui_rrect(c, bui_r(x0 - 5, cy - 5, x1 - x0 + 10, 10), 5, BUI_C_SURFACE3);
	bui_rrect(c, bui_r(x0 - 5, cy - 5, kx - x0 + 10, 10), 5, col);
	if (w->flags & (BUI_F_PRESSED | BUI_F_FOCUS))
		bui_rrect_a(c, bui_r(kx - 22, cy - 22, 44, 44), 22, col, 72);
	bui_rrect(c, bui_r(kx - KNOB / 2, cy - KNOB / 2, KNOB, KNOB), KNOB / 2,
		  dis ? BUI_C_TEXT2 : BUI_C_TEXT);
}

static int slider_value_at(const bui_widget *w, bui_rect r, int x)
{
	int x0, x1, range = w->vmax - w->vmin;

	slider_span(r, &x0, &x1);
	return clampi(w->vmin + ((x - x0) * range + (x1 - x0) / 2) / (x1 - x0), w->vmin, w->vmax);
}

static void draw_toggle(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int pressed = w->flags & BUI_F_PRESSED;
	int pos = shown_value(w, w->value ? 256 : 0);	/* 0 spento .. 256 acceso */
	int ks = 20 + 4 * pos / 256;
	bui_rect sw = bui_r(r.x + r.w - 16 - 52, r.y + (r.h - 32) / 2, 52, 32);

	if (pressed)
		bui_rrect(c, bui_rect_inset(r, 4, 2), 12, BUI_C_SURFACE2);
	bui_text_box(c, BUI_FONT_BODY, bui_r(r.x + 16, r.y, sw.x - r.x - 28, r.h), w->text,
		     BUI_C_TEXT, BUI_AL_LEFT);
	bui_rrect(c, sw, 16, pmix(BUI_C_SURFACE3, BUI_C_ACCENT, pos * 255 / 256));
	bui_rrect(c, bui_r(sw.x + 6 + (sw.w - 34) * pos / 256, sw.y + (32 - ks) / 2, ks, ks), ks / 2,
		  pmix(BUI_C_TEXT2, BUI_C_ON_ACCENT, pos * 255 / 256));
	if (w->flags & BUI_F_FOCUS)
		bui_rrect_line(c, bui_rect_inset(r, 4, 2), 12, 2, BUI_C_ACCENT);
	if (w->flags & BUI_F_DIVIDER)
		bui_fill(c, bui_r(r.x + 16, r.y + r.h - 1, r.w - 32, 1), BUI_C_LINE);
}

static int segs_count(const bui_widget *w)
{
	const char *t = w->text;
	int n = 1;

	if (!t || !*t)
		return 0;
	for (; *t; t++)
		n += *t == '\n';
	return n;
}

/* selettore a segmenti: la pastiglia della scelta scivola da una voce all'altra */
static void draw_segs(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int n = segs_count(w), i;
	int rad = r.h / 2 < bui_th.r_btn ? r.h / 2 : bui_th.r_btn;
	int sw, pos;
	const char *t = w->text;

	bui_rrect(c, r, rad, BUI_C_SURFACE2);
	if (n < 1)
		return;
	sw = (r.w - 8) / n;
	pos = shown_value(w, clampi(w->value, 0, n - 1) * 256);
	bui_rrect(c, bui_r(r.x + 4 + sw * pos / 256, r.y + 4, sw, r.h - 8), rad > 4 ? rad - 3 : rad,
		  (w->flags & BUI_F_DISABLED) ? BUI_C_SURFACE3 : BUI_C_ACCENT_BG);
	for (i = 0; i < n; i++) {
		char buf[32];
		int len = 0;

		while (*t && *t != '\n') {
			if (len < (int)sizeof(buf) - 1)
				buf[len++] = *t;
			t++;
		}
		if (*t)
			t++;
		buf[len] = 0;
		bui_text_box(c, i == w->value ? BUI_FONT_SMALL_B : BUI_FONT_SMALL,
			     bui_r(r.x + 4 + i * sw + 2, r.y, sw - 4, r.h), buf,
			     (w->flags & BUI_F_DISABLED) ? BUI_C_TEXT3 : i == w->value ? BUI_C_ACCENT
										       : BUI_C_TEXT2,
			     BUI_AL_CENTER);
	}
	if (w->flags & BUI_F_FOCUS)
		bui_rrect_line(c, r, rad, 2, BUI_C_ACCENT);
}

static void draw_row(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int x = r.x + 16, right = r.x + r.w - 16;

	if ((w->flags & BUI_F_PRESSED) && w->on_click)
		bui_rrect(c, bui_rect_inset(r, 4, 2), 12, BUI_C_SURFACE2);
	if (w->icon) {
		bui_icon_in(c, w->icon, bui_r(x, r.y, w->icon->size, r.h), BUI_C_TEXT2);
		x += w->icon->size + 16;
	}
	if (w->on_click) {
		right -= 24;
		bui_icon_in(c, BUI_IC(chevron, 24), bui_r(right, r.y, 24, r.h), BUI_C_TEXT3);
		right -= 4;
	}
	if (w->text2 && *w->text2) {
		int tw = bui_text_width(BUI_FONT_BODY, w->text2, -1);
		int maxw = (right - x) * 2 / 3;

		if (tw > maxw)
			tw = maxw;
		bui_text_box(c, BUI_FONT_BODY, bui_r(right - tw, r.y, tw, r.h), w->text2,
			     w->color ? w->color : BUI_C_TEXT2, BUI_AL_RIGHT);
		right -= tw + 12;
	}
	bui_text_box(c, BUI_FONT_BODY, bui_r(x, r.y, right - x, r.h), w->text, BUI_C_TEXT, BUI_AL_LEFT);
	if (w->flags & BUI_F_FOCUS)
		bui_rrect_line(c, bui_rect_inset(r, 4, 2), 12, 2, BUI_C_ACCENT);
	if (w->flags & BUI_F_DIVIDER)
		bui_fill(c, bui_r(r.x + 16, r.y + r.h - 1, r.w - 32, 1), BUI_C_LINE);
}

void bui_list_row(bui_canvas *c, bui_rect r, unsigned flags, const bui_icon *icon,
		  const char *title, const char *subtitle, const char *right)
{
	int x = r.x + 16, rx = r.x + r.w - 16;

	if (flags & BUI_F_PRESSED)
		bui_rrect(c, bui_rect_inset(r, 4, 2), 12, BUI_C_SURFACE2);
	else if (flags & BUI_F_SELECTED)
		bui_rrect(c, bui_rect_inset(r, 4, 2), 12, BUI_C_ACCENT_BG);
	if (flags & BUI_F_FOCUS)
		bui_rrect_line(c, bui_rect_inset(r, 4, 2), 12, 2, BUI_C_ACCENT);
	if (icon) {
		bui_icon_in(c, icon, bui_r(x, r.y, icon->size, r.h), BUI_C_TEXT2);
		x += icon->size + 16;
	}
	if (right && *right) {
		int tw = bui_text_width(BUI_FONT_BODY, right, -1);

		bui_text_box(c, BUI_FONT_BODY, bui_r(rx - tw, r.y, tw, r.h), right, BUI_C_TEXT2,
			     BUI_AL_RIGHT);
		rx -= tw + 12;
	}
	if (subtitle && *subtitle) {
		bui_text_box(c, BUI_FONT_BODY, bui_r(x, r.y + r.h / 2 - 22, rx - x, 22), title,
			     BUI_C_TEXT, BUI_AL_LEFT);
		bui_text_box(c, BUI_FONT_SMALL, bui_r(x, r.y + r.h / 2 + 2, rx - x, 20), subtitle,
			     BUI_C_TEXT2, BUI_AL_LEFT);
	} else {
		bui_text_box(c, BUI_FONT_BODY, bui_r(x, r.y, rx - x, r.h), title, BUI_C_TEXT, BUI_AL_LEFT);
	}
}

static void list_update_max(bui_list *l)
{
	int vh = l->w ? l->w->r.h : 0;
	int m = l->count * l->row_h - vh;

	l->sc.max = m > 0 ? m : 0;
	if (l->sc.pos > l->sc.max)
		l->sc.pos = l->sc.max;
}

/* rettangolo del cursore della barra di scorrimento dentro l'area r; 0 se non c'e' */
static int sb_thumb(bui_rect r, const bui_scroll *s, bui_rect *out)
{
	int content = r.h + s->max;
	int th, y;

	if (s->max <= 0)
		return 0;
	th = r.h * r.h / content;
	if (th < 24)
		th = 24;
	y = r.y + (r.h - th) * s->pos / s->max;
	*out = bui_r(r.x + r.w - 6, y + 2, 4, th - 4);
	return 1;
}

/* senza animazioni la barra e' sempre visibile; altrimenti compare quando
 * qualcosa scorre e poi svanisce */
static void draw_scrollbar(bui_canvas *c, bui_rect r, const bui_scroll *s)
{
	int alpha = bui_anim_level() == BUI_ANIM_NONE ? SB_ALPHA : s->bar;
	bui_rect t;

	if (alpha && sb_thumb(r, s, &t))
		bui_rrect_a(c, t, 2, BUI_C_TEXT2, alpha);
}

static void draw_list(bui_widget *w, bui_canvas *c, bui_rect r)
{
	bui_list *l = w->list;
	int i, focus;

	if (!l || !l->draw_row || l->row_h <= 0)
		return;
	if (!l->count && l->empty_text) {
		bui_text_box(c, BUI_FONT_BODY, bui_rect_inset(r, 16, 0), l->empty_text, BUI_C_TEXT3,
			     BUI_AL_CENTER);
		return;
	}
	focus = (w->flags & BUI_F_FOCUS) != 0;
	i = (l->sc.pos + l->sc.over) / l->row_h;
	for (i = i < 0 ? 0 : i; i < l->count; i++) {
		bui_rect rr = bui_r(r.x, r.y + i * l->row_h - l->sc.pos - l->sc.over, r.w, l->row_h);
		unsigned fl = 0;

		if (rr.y >= c->clip.y + c->clip.h)
			break;
		if (rr.y + rr.h <= c->clip.y)
			continue;
		if (i == l->pressed)
			fl |= BUI_F_PRESSED;
		if (i == l->sel && focus)
			fl |= BUI_F_FOCUS;
		l->draw_row(l, c, i, rr, fl);
	}
	draw_scrollbar(c, r, &l->sc);
}

static void draw_widget(bui_widget *w, bui_canvas *c, bui_rect r)
{
	switch (w->type) {
	case BUI_W_LABEL:
		draw_label(w, c, r);
		break;
	case BUI_W_BUTTON:
		draw_button(w, c, r);
		break;
	case BUI_W_TILE:
		draw_tile(w, c, r);
		break;
	case BUI_W_SLIDER:
		draw_slider(w, c, r);
		break;
	case BUI_W_TOGGLE:
		draw_toggle(w, c, r);
		break;
	case BUI_W_ROW:
		draw_row(w, c, r);
		break;
	case BUI_W_CARD:
		bui_rrect(c, r, bui_th.r_card, w->color ? w->color : BUI_C_SURFACE);
		break;
	case BUI_W_LIST:
		draw_list(w, c, r);
		break;
	case BUI_W_CUSTOM:
		if (w->draw)
			w->draw(w, c, r);
		break;
	case BUI_W_SEGS:
		draw_segs(w, c, r);
		break;
	}
}

void bui_widget_draw(bui_widget *w, bui_canvas *c, bui_rect r)
{
	bui_rect saved = bui_clip_push(c, r);

	draw_widget(w, c, r);
	bui_clip_pop(c, saved);
}

/* ------------------------------------------------------------------ */
/* pannello                                                           */
/* ------------------------------------------------------------------ */

static int panel_anim(void *user, uint32_t now);
static int list_anim(void *user, uint32_t now);
static int panel_fx(void *user, uint32_t now);

static int fx_target(const bui_widget *w)
{
	if (w->type == BUI_W_TOGGLE)
		return w->value ? 256 : 0;
	return w->value * 256;
}

/* ferma l'animazione breve in corso: il widget prende subito l'aspetto finale */
static void fx_stop(bui_panel *p)
{
	bui_widget *w = p->fx_w;

	if (!w)
		return;
	p->fx_w = NULL;
	p->fx_kind = FX_NONE;
	bui_anim_del(panel_fx, p);
	bui_widget_damage(w);
}

void bui_panel_fx_cancel(bui_panel *p)
{
	fx_stop(p);
}

static void fx_start(bui_panel *p, bui_widget *w, int kind, int from)
{
	if (p->fx_w == w && p->fx_kind == kind && kind == FX_VALUE)
		from = p->fx_val;	/* riparte da dov'e' arrivata */
	else
		fx_stop(p);
	p->fx_w = w;
	p->fx_kind = (uint8_t)kind;
	p->fx_from = p->fx_val = from;
	p->fx_start = bui_now();
	bui_anim_add(panel_fx, p);
}

static int panel_fx(void *user, uint32_t now)
{
	bui_panel *p = user;
	bui_widget *w = p->fx_w;
	int t, e;

	if (!w)
		return 0;
	t = bui_tween(p->fx_start, p->fx_kind == FX_RELEASE ? FX_RELEASE_MS : FX_VALUE_MS, now);
	if (t >= 1024) {
		p->fx_w = NULL;
		p->fx_kind = FX_NONE;
		bui_widget_damage(w);
		return 0;
	}
	e = bui_ease_out(t);
	if (p->fx_kind == FX_RELEASE)
		p->fx_val = 255 - 255 * e / 1024;
	else
		p->fx_val = bui_lerp(p->fx_from, fx_target(w), e);
	bui_widget_damage(w);
	return 1;
}

/* cambia il valore di cursore/interruttore/selettore facendolo scivolare */
static void value_anim(bui_widget *w, int old_shown)
{
	if (bui_anim_level() >= BUI_ANIM_REDUCED && w->panel && !(w->flags & BUI_F_HIDDEN))
		fx_start(w->panel, w, FX_VALUE, old_shown);
	else
		bui_widget_damage(w);
}

/* ---- barra di scorrimento che svanisce ---- */

static void sb_tick(void *user);

static void sc_touch(bui_panel *p, bui_scroll *s)
{
	s->act = bui_now();
	s->bar = SB_ALPHA;
	if (bui_anim_level() != BUI_ANIM_NONE && !p->sb_timer)
		p->sb_timer = bui_timer_add(SB_HOLD_MS, 0, sb_tick, p);
}

/* un passo della dissolvenza; ritorna fra quanti ms serve il prossimo (-1: mai) */
static int sb_fade(bui_panel *p, bui_scroll *s, bui_rect area, uint32_t now)
{
	int idle = (int32_t)(now - s->act);
	bui_rect t;

	if (!s->bar)
		return -1;
	if (s->fling)
		return SB_HOLD_MS;
	if (idle < SB_HOLD_MS)
		return SB_HOLD_MS - idle + 10;
	s->bar = s->bar > 70 ? 70 : s->bar > 35 ? 35 : 0;
	if (sb_thumb(area, s, &t) && bui_rect_isect(t, p->vp, &t))
		bui_win_damage(p->win, t);
	return s->bar ? 66 : -1;
}

static void sb_tick(void *user)
{
	bui_panel *p = user;
	uint32_t now = bui_now();
	int next = sb_fade(p, &p->sc, p->vp, now), i, d;

	p->sb_timer = 0;
	for (i = 0; i < p->n; i++)
		if (p->w[i].type == BUI_W_LIST && p->w[i].list) {
			d = sb_fade(p, &p->w[i].list->sc, bui_widget_rect(&p->w[i]), now);
			if (d >= 0 && (next < 0 || d < next))
				next = d;
		}
	if (next >= 0)
		p->sb_timer = bui_timer_add(next, 0, sb_tick, p);
}

static void panel_scrolled(bui_panel *p)
{
	sc_touch(p, &p->sc);
	bui_panel_damage(p);
}

static void list_scrolled(bui_widget *w)
{
	sc_touch(w->panel, &w->list->sc);
	bui_widget_damage(w);
}

void bui_panel_init(bui_panel *p, bui_win *win)
{
	memset(p, 0, sizeof(*p));
	p->win = win;
}

void bui_panel_clear(bui_panel *p)
{
	int i;

	bui_anim_del(panel_anim, p);
	bui_anim_del(panel_fx, p);
	bui_timer_del(p->lp_timer);
	bui_timer_del(p->sb_timer);
	p->lp_timer = p->sb_timer = 0;
	p->fx_w = NULL;
	p->fx_kind = FX_NONE;
	for (i = 0; i < p->n; i++)
		if (p->w[i].type == BUI_W_LIST && p->w[i].list) {
			bui_anim_del(list_anim, p->w[i].list);
			p->w[i].list->w = NULL;
		}
	p->n = 0;
	p->hot = p->focus = NULL;
	p->captured = 0;
	p->mode = MODE_NONE;
	p->anim = 0;
	memset(&p->sc, 0, sizeof(p->sc));
}

void bui_panel_set_viewport(bui_panel *p, bui_rect vp, int content_h)
{
	p->vp = vp;
	p->content_h = content_h;
	p->sc.max = content_h > vp.h ? content_h - vp.h : 0;
	if (p->sc.pos > p->sc.max)
		p->sc.pos = p->sc.max;
	/* la barra si fa vedere un momento: dice che il contenuto continua */
	if (p->sc.max > 0 && !p->no_scrollbar && p->win)
		sc_touch(p, &p->sc);
}

bui_rect bui_widget_rect(const bui_widget *w)
{
	const bui_panel *p = w->panel;

	return bui_r(p->vp.x + w->r.x, p->vp.y + w->r.y - p->sc.pos - p->sc.over, w->r.w, w->r.h);
}

void bui_widget_damage(bui_widget *w)
{
	bui_rect r;

	if (w && w->panel && bui_rect_isect(bui_widget_rect(w), w->panel->vp, &r))
		bui_win_damage(w->panel->win, r);
}

void bui_panel_damage(bui_panel *p)
{
	bui_win_damage(p->win, p->vp);
}

void bui_widget_set_text(bui_widget *w, const char *text)
{
	w->text = text;
	bui_widget_damage(w);
}

void bui_widget_set_value(bui_widget *w, int value)
{
	int old;

	if (w->value == value)
		return;
	old = shown_value(w, fx_target(w));
	w->value = value;
	if (w->type == BUI_W_SLIDER || w->type == BUI_W_TOGGLE || w->type == BUI_W_SEGS)
		value_anim(w, old);
	else
		bui_widget_damage(w);
}

void bui_widget_set_flag(bui_widget *w, unsigned flag, int on)
{
	unsigned nf = on ? (w->flags | flag) : (w->flags & ~flag);

	if (nf == w->flags)
		return;
	w->flags = (uint16_t)nf;
	bui_widget_damage(w);
}

void bui_panel_draw(bui_panel *p, bui_canvas *c, bui_rect clip)
{
	bui_rect k, saved;
	int i;

	if (!bui_rect_isect(clip, p->vp, &k))
		return;
	saved = bui_clip_push(c, k);
	k = c->clip;
	if (k.w > 0 && k.h > 0) {
		if (p->paint_bg)
			p->paint_bg(p, c, k);
		/* le ombre dei riquadri escono dal loro rettangolo: un passaggio a parte,
		 * limitato al clip, prima dei widget */
		for (i = 0; bui_th.tile_shadow && i < p->n; i++) {
			bui_widget *w = &p->w[i];

			if (w->type == BUI_W_TILE && !(w->flags & BUI_F_HIDDEN) &&
			    tile_style(w) == BUI_TILE_FILLED)
				bui_shadow(c, bui_widget_rect(w), bui_th.r_tile, bui_th.tile_shadow);
		}
		for (i = 0; i < p->n; i++) {
			bui_widget *w = &p->w[i];
			bui_rect wr, wk;

			if (w->flags & BUI_F_HIDDEN)
				continue;
			wr = bui_widget_rect(w);
			if (!bui_rect_isect(wr, k, &wk))
				continue;
			c->clip = wk;
			draw_widget(w, c, wr);
		}
		c->clip = k;
		if (!p->no_scrollbar)
			draw_scrollbar(c, p->vp, &p->sc);
	}
	bui_clip_pop(c, saved);
}

static int interactive(const bui_widget *w)
{
	if (w->flags & (BUI_F_HIDDEN | BUI_F_DISABLED))
		return 0;
	switch (w->type) {
	case BUI_W_BUTTON:
	case BUI_W_TILE:
	case BUI_W_SLIDER:
	case BUI_W_TOGGLE:
		return 1;
	case BUI_W_SEGS:
		return segs_count(w) > 0;
	case BUI_W_LIST:
		return w->list != NULL;
	case BUI_W_ROW:
	case BUI_W_CUSTOM:
		return w->on_click != NULL;
	default:
		return 0;
	}
}

/* x, y in coordinate della finestra */
static bui_widget *hit(bui_panel *p, int x, int y, int slop)
{
	int i;

	for (i = p->n - 1; i >= 0; i--) {
		bui_widget *w = &p->w[i];
		bui_rect r;

		if (!interactive(w))
			continue;
		r = bui_rect_inset(bui_widget_rect(w), -slop, -slop);
		if (bui_rect_has(r, x, y))
			return w;
	}
	return NULL;
}

static void set_pressed(bui_widget *w, int on)
{
	bui_widget_set_flag(w, BUI_F_PRESSED, on);
}

/* smooth: il pomello scivola verso il punto toccato invece di saltarci */
static void slider_drag(bui_widget *w, int x, int final, int smooth)
{
	int v = slider_value_at(w, bui_widget_rect(w), x);
	int old = shown_value(w, w->value * 256);

	if (v != w->value) {
		w->value = v;
		if (smooth) {
			value_anim(w, old);
		} else {
			if (w->panel->fx_w == w)
				fx_stop(w->panel);
			bui_widget_damage(w);
		}
		if (w->on_change)
			w->on_change(w, final);
	} else if (final && w->on_change) {
		w->on_change(w, 1);
	}
}

static void segs_set(bui_widget *w, int v)
{
	int n = segs_count(w), old = shown_value(w, w->value * 256);

	v = clampi(v, 0, n - 1);
	if (v == w->value)
		return;
	w->value = v;
	value_anim(w, old);
	if (w->on_change)
		w->on_change(w, 1);
}

static void activate(bui_widget *w)
{
	int old;

	switch (w->type) {
	case BUI_W_TOGGLE:
		old = shown_value(w, w->value ? 256 : 0);
		w->value = !w->value;
		value_anim(w, old);
		if (w->on_change)
			w->on_change(w, 1);
		break;
	case BUI_W_SEGS:
		/* da tastiera: passa alla voce successiva */
		segs_set(w, (w->value + 1) % (segs_count(w) > 0 ? segs_count(w) : 1));
		break;
	case BUI_W_LIST:
		if (w->list && w->list->sel >= 0 && w->list->sel < w->list->count && w->list->on_select)
			w->list->on_select(w->list, w->list->sel);
		break;
	default:
		if (w->on_click)
			w->on_click(w);
		break;
	}
}

/* fine del "premuto": pulsanti e riquadri sfumano invece di scattare */
static void release_pressed(bui_widget *w)
{
	if (!(w->flags & BUI_F_PRESSED))
		return;
	if (bui_anim_level() >= BUI_ANIM_REDUCED && (w->type == BUI_W_BUTTON || w->type == BUI_W_TILE)) {
		w->flags &= ~BUI_F_PRESSED;
		fx_start(w->panel, w, FX_RELEASE, 255);
	} else {
		set_pressed(w, 0);
	}
}

static void lp_cancel(bui_panel *p)
{
	if (p->lp_timer)
		bui_timer_del(p->lp_timer);
	p->lp_timer = 0;
}

static void lp_fire(void *user)
{
	bui_panel *p = user;
	bui_widget *w = p->hot;

	p->lp_timer = 0;
	if (!p->captured || !w || !(w->flags & BUI_F_PRESSED) || p->mode != MODE_NONE || !p->on_long_press)
		return;
	/* il tocco in corso non attivera' piu' nulla: da qui in poi e' di chi riceve la chiamata */
	set_pressed(w, 0);
	p->hot = NULL;
	p->captured = 0;
	p->on_long_press(p, w);
}

static int panel_anim(void *user, uint32_t now)
{
	bui_panel *p = user;

	if (bui_scroll_step(&p->sc, now))
		panel_scrolled(p);
	if (!p->sc.fling)
		p->anim = 0;
	return p->sc.fling;
}

static int list_anim(void *user, uint32_t now)
{
	bui_list *l = user;

	if (!l->w)
		return 0;
	if (bui_scroll_step(&l->sc, now))
		list_scrolled(l->w);
	return l->sc.fling;
}

static int list_row_at(const bui_widget *w, int y)
{
	bui_list *l = w->list;
	bui_rect r = bui_widget_rect(w);
	int i;

	if (l->row_h <= 0 || y < r.y)
		return -1;
	i = (y - r.y + l->sc.pos + l->sc.over) / l->row_h;
	return i >= 0 && i < l->count ? i : -1;
}

static void list_damage_row(bui_widget *w, int i)
{
	bui_list *l = w->list;
	bui_rect r = bui_widget_rect(w), rr;

	if (i < 0)
		return;
	rr = bui_r(r.x, r.y + i * l->row_h - l->sc.pos - l->sc.over, r.w, l->row_h);
	if (bui_rect_isect(rr, r, &rr) && bui_rect_isect(rr, w->panel->vp, &rr))
		bui_win_damage(w->panel->win, rr);
}

static void list_set_pressed(bui_widget *w, int i)
{
	bui_list *l = w->list;

	if (l->pressed == i)
		return;
	list_damage_row(w, l->pressed);
	l->pressed = i;
	list_damage_row(w, i);
}

static void hide_focus(bui_panel *p)
{
	if (p->focus_vis && p->focus) {
		p->focus->flags &= ~BUI_F_FOCUS;
		bui_widget_damage(p->focus);
	}
	p->focus_vis = 0;
}

int bui_panel_pointer(bui_panel *p, const bui_pointer *ev)
{
	bui_widget *w = p->hot;
	int dx, dy, adx, ady, was_fling;

	switch (ev->type) {
	case BUI_PRESS:
		if (!bui_rect_has(p->vp, ev->x, ev->y))
			return 0;
		hide_focus(p);
		p->captured = 1;
		p->mode = MODE_NONE;
		p->press_x = ev->x;
		p->press_y = ev->y;
		was_fling = p->sc.fling;
		p->sc.elastic = bui_anim_level() == BUI_ANIM_FULL;
		bui_scroll_grab(&p->sc, ev->y, ev->time);
		if (p->sc.over) {	/* fermato mentre tornava dall'elastico */
			p->sc.over = 0;
			bui_panel_damage(p);
		}
		lp_cancel(p);
		p->hot = w = hit(p, ev->x, ev->y, 0);
		if (was_fling) {
			/* il tocco ferma l'inerzia e non attiva nulla */
			p->hot = NULL;
			return 1;
		}
		if (!w)
			return 1;
		if (w->type == BUI_W_LIST) {
			bui_list *l = w->list;

			was_fling = l->sc.fling;
			l->sc.elastic = bui_anim_level() == BUI_ANIM_FULL;
			bui_scroll_grab(&l->sc, ev->y, ev->time);
			if (l->sc.over) {
				l->sc.over = 0;
				bui_widget_damage(w);
			}
			/* un tocco su una lista in movimento la ferma soltanto */
			list_set_pressed(w, was_fling ? -1 : list_row_at(w, ev->y));
			return 1;
		}
		if (p->fx_w == w && p->fx_kind == FX_RELEASE)
			fx_stop(p);
		set_pressed(w, 1);
		if (w->type == BUI_W_SLIDER && p->sc.max <= 0) {
			p->mode = MODE_SLIDER;
			slider_drag(w, ev->x, 0, 1);
		} else if (p->on_long_press) {
			p->lp_timer = bui_timer_add(LONG_PRESS_MS, 0, lp_fire, p);
		}
		return 1;

	case BUI_MOVE:
		if (!p->captured)
			return 0;
		dx = ev->x - p->press_x;
		dy = ev->y - p->press_y;
		adx = dx < 0 ? -dx : dx;
		ady = dy < 0 ? -dy : dy;
		if (p->mode == MODE_NONE && (adx > BUI_DRAG_SLOP || ady > BUI_DRAG_SLOP)) {
			lp_cancel(p);
			/* e' un trascinamento: a chi appartiene? */
			if (w && w->type == BUI_W_SLIDER && adx >= ady) {
				p->mode = MODE_SLIDER;
			} else if (w && w->type == BUI_W_LIST && ady > adx && w->list->sc.max > 0) {
				p->mode = MODE_LIST;
				list_set_pressed(w, -1);
				bui_scroll_grab(&w->list->sc, ev->y, ev->time);
			} else if (p->sc.max > 0 && ady > adx) {
				p->mode = MODE_SCROLL;
				if (w && w->type == BUI_W_LIST)
					list_set_pressed(w, -1);
				else if (w)
					set_pressed(w, 0);
				p->hot = w = NULL;
				bui_scroll_grab(&p->sc, ev->y, ev->time);
			}
		}
		switch (p->mode) {
		case MODE_SLIDER:
			slider_drag(w, ev->x, 0, 0);
			break;
		case MODE_SCROLL:
			if (bui_scroll_drag(&p->sc, ev->y, ev->time))
				panel_scrolled(p);
			break;
		case MODE_LIST:
			if (bui_scroll_drag(&w->list->sc, ev->y, ev->time))
				list_scrolled(w);
			break;
		default:
			/* il dito puo' uscire dal widget e rientrare */
			if (w && w->type == BUI_W_LIST) {
				if (adx > BUI_DRAG_SLOP || ady > BUI_DRAG_SLOP)
					list_set_pressed(w, -1);
			} else if (w) {
				set_pressed(w, bui_rect_has(bui_rect_inset(bui_widget_rect(w), -BUI_DRAG_SLOP,
									   -BUI_DRAG_SLOP), ev->x, ev->y));
			}
			break;
		}
		return 1;

	case BUI_RELEASE:
	case BUI_CANCEL:
		if (!p->captured)
			return 0;
		p->captured = 0;
		p->hot = NULL;
		lp_cancel(p);
		switch (p->mode) {
		case MODE_SLIDER:
			p->mode = MODE_NONE;
			set_pressed(w, 0);
			if (ev->type == BUI_RELEASE)
				slider_drag(w, ev->x, 1, 0);
			else if (w->on_change)
				w->on_change(w, 1);
			return 1;
		case MODE_SCROLL:
			p->mode = MODE_NONE;
			if (ev->type == BUI_RELEASE && bui_scroll_release(&p->sc, ev->time)) {
				p->anim = 1;
				bui_anim_add(panel_anim, p);
			}
			return 1;
		case MODE_LIST:
			p->mode = MODE_NONE;
			if (ev->type == BUI_RELEASE && bui_scroll_release(&w->list->sc, ev->time))
				bui_anim_add(list_anim, w->list);
			return 1;
		default:
			break;
		}
		if (!w)
			return 1;
		if (w->type == BUI_W_LIST) {
			bui_list *l = w->list;
			int row = l->pressed;

			list_set_pressed(w, -1);
			if (ev->type == BUI_RELEASE && row >= 0) {
				l->sel = row;
				if (l->on_select)
					l->on_select(l, row);
			}
			return 1;
		}
		if (w->flags & BUI_F_PRESSED) {
			if (ev->type != BUI_RELEASE) {
				set_pressed(w, 0);
			} else if (w->type == BUI_W_SLIDER) {
				set_pressed(w, 0);
				slider_drag(w, ev->x, 1, 1);
			} else if (w->type == BUI_W_SEGS) {
				bui_rect r = bui_widget_rect(w);
				int n = segs_count(w), sw = (r.w - 8) / (n > 0 ? n : 1);

				set_pressed(w, 0);
				segs_set(w, sw > 0 ? (ev->x - r.x - 4) / sw : 0);
			} else {
				release_pressed(w);
				activate(w);	/* puo' ricostruire il pannello: non toccare piu' w */
			}
		}
		return 1;

	case BUI_WHEEL:
		if (!bui_rect_has(p->vp, ev->x, ev->y))
			return 0;
		w = hit(p, ev->x, ev->y, 0);
		if (w && w->type == BUI_W_LIST && w->list->sc.max > 0) {
			if (bui_scroll_by(&w->list->sc, ev->dy * 64))
				list_scrolled(w);
		} else if (bui_scroll_by(&p->sc, ev->dy * 64)) {
			panel_scrolled(p);
		}
		return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* tastiera                                                           */
/* ------------------------------------------------------------------ */

static int focusable(const bui_widget *w)
{
	return interactive(w) && !(w->flags & BUI_F_NOFOCUS);
}

static void reveal(bui_panel *p, bui_widget *w)
{
	int np = p->sc.pos;

	if (p->sc.max <= 0)
		return;
	if (w->r.y - 8 < np)
		np = w->r.y - 8;
	else if (w->r.y + w->r.h + 8 > np + p->vp.h)
		np = w->r.y + w->r.h + 8 - p->vp.h;
	np = clampi(np, 0, p->sc.max);
	if (np != p->sc.pos) {
		p->sc.pos = np;
		p->sc.fling = 0;
		panel_scrolled(p);
	}
}

void bui_panel_focus(bui_panel *p, bui_widget *w, int visible)
{
	if (p->focus && (p->focus->flags & BUI_F_FOCUS)) {
		p->focus->flags &= ~BUI_F_FOCUS;
		bui_widget_damage(p->focus);
	}
	p->focus = w;
	p->focus_vis = visible && w;
	if (w && visible) {
		w->flags |= BUI_F_FOCUS;
		if (w->type == BUI_W_LIST && w->list->sel < 0 && w->list->count > 0 && w->list->row_h > 0)
			w->list->sel = (w->list->sc.pos + w->list->row_h - 1) / w->list->row_h;
		reveal(p, w);
		bui_widget_damage(w);
	}
}

static int gap(int a0, int a1, int b0, int b1)
{
	if (b0 >= a1)
		return b0 - a1;
	if (a0 >= b1)
		return a0 - b1;
	return 0;
}

static bui_widget *nav_find(bui_panel *p, bui_widget *f, int key)
{
	bui_widget *best = NULL;
	int i, best_score = 0;
	bui_rect a = f->r;

	for (i = 0; i < p->n; i++) {
		bui_widget *w = &p->w[i];
		bui_rect b = w->r;
		int prim, sec, score;

		if (w == f || !focusable(w))
			continue;
		switch (key) {
		case BUI_K_RIGHT:
			prim = b.x - (a.x + a.w);
			break;
		case BUI_K_LEFT:
			prim = a.x - (b.x + b.w);
			break;
		case BUI_K_DOWN:
			prim = b.y - (a.y + a.h);
			break;
		default:
			prim = a.y - (b.y + b.h);
			break;
		}
		if (prim < -4)
			continue;
		if (key == BUI_K_LEFT || key == BUI_K_RIGHT)
			sec = gap(a.y, a.y + a.h, b.y, b.y + b.h) * 4 +
			      abs((a.y + a.h / 2) - (b.y + b.h / 2)) / 8;
		else
			sec = gap(a.x, a.x + a.w, b.x, b.x + b.w) * 4 +
			      abs((a.x + a.w / 2) - (b.x + b.w / 2)) / 8;
		score = prim + sec;
		if (!best || score < best_score) {
			best = w;
			best_score = score;
		}
	}
	return best;
}

void bui_list_reveal(bui_list *l, int index)
{
	int np = l->sc.pos, vh;

	if (!l->w || index < 0)
		return;
	vh = l->w->r.h;
	if (index * l->row_h < np)
		np = index * l->row_h;
	else if ((index + 1) * l->row_h > np + vh)
		np = (index + 1) * l->row_h - vh;
	np = clampi(np, 0, l->sc.max);
	l->sc.fling = 0;
	if (np != l->sc.pos) {
		l->sc.pos = np;
		list_scrolled(l->w);
	}
}

void bui_list_set_count(bui_list *l, int count)
{
	l->count = count;
	if (l->sel >= count)
		l->sel = count - 1;
	l->pressed = -1;
	list_update_max(l);
	if (l->w)
		bui_widget_damage(l->w);
}

int bui_panel_key(bui_panel *p, const bui_key *k)
{
	bui_widget *f = p->focus, *n;
	int i, dir;

	if (f && !focusable(f))
		f = NULL;
	switch (k->key) {
	case BUI_K_UP:
	case BUI_K_DOWN:
	case BUI_K_LEFT:
	case BUI_K_RIGHT:
		if (!f || !p->focus_vis) {
			/* primo tasto: mostra soltanto dove si trova il fuoco */
			if (!f)
				for (i = 0; i < p->n && !f; i++)
					if (focusable(&p->w[i]))
						f = &p->w[i];
			if (!f)
				break;
			bui_panel_focus(p, f, 1);
			return 1;
		}
		if (f->type == BUI_W_SLIDER && (k->key == BUI_K_LEFT || k->key == BUI_K_RIGHT)) {
			int step = (f->vmax - f->vmin) / 20;
			int v;

			if (step < 1)
				step = 1;
			v = clampi(f->value + (k->key == BUI_K_RIGHT ? step : -step), f->vmin, f->vmax);
			if (v != f->value) {
				int old = shown_value(f, f->value * 256);

				f->value = v;
				value_anim(f, old);
				if (f->on_change)
					f->on_change(f, 1);
			}
			return 1;
		}
		if (f->type == BUI_W_SEGS && (k->key == BUI_K_LEFT || k->key == BUI_K_RIGHT)) {
			int v = f->value + (k->key == BUI_K_RIGHT ? 1 : -1);

			if (v >= 0 && v < segs_count(f)) {
				segs_set(f, v);
				return 1;
			}
		}
		if (f->type == BUI_W_LIST && (k->key == BUI_K_UP || k->key == BUI_K_DOWN)) {
			bui_list *l = f->list;
			int ns = l->sel + (k->key == BUI_K_DOWN ? 1 : -1);

			if (ns >= 0 && ns < l->count) {
				l->sel = ns;
				bui_list_reveal(l, ns);
				bui_widget_damage(f);
				return 1;
			}
		}
		n = nav_find(p, f, k->key);
		if (n) {
			bui_panel_focus(p, n, 1);
			return 1;
		}
		if (p->sc.max > 0 && (k->key == BUI_K_UP || k->key == BUI_K_DOWN)) {
			if (bui_scroll_by(&p->sc, k->key == BUI_K_DOWN ? 64 : -64))
				panel_scrolled(p);
			return 1;
		}
		break;
	case BUI_K_TAB:
		dir = (k->mods & BUI_MOD_SHIFT) ? -1 : 1;
		i = f ? (int)(f - p->w) : (dir > 0 ? -1 : p->n);
		for (n = NULL, i += dir; i >= 0 && i < p->n; i += dir)
			if (focusable(&p->w[i])) {
				n = &p->w[i];
				break;
			}
		if (!n)
			for (i = dir > 0 ? 0 : p->n - 1; i >= 0 && i < p->n; i += dir)
				if (focusable(&p->w[i])) {
					n = &p->w[i];
					break;
				}
		if (!n)
			break;
		bui_panel_focus(p, n, 1);
		return 1;
	case BUI_K_ENTER:
	case ' ':
		if (f && p->focus_vis) {
			activate(f);
			return 1;
		}
		break;
	case BUI_K_PGUP:
	case BUI_K_PGDN:
		dir = k->key == BUI_K_PGDN ? 1 : -1;
		if (f && f->type == BUI_W_LIST && f->list->sc.max > 0) {
			if (bui_scroll_by(&f->list->sc, dir * (f->r.h - f->list->row_h)))
				list_scrolled(f);
			return 1;
		}
		if (p->sc.max > 0) {
			if (bui_scroll_by(&p->sc, dir * (p->vp.h - BUI_TOUCH)))
				panel_scrolled(p);
			return 1;
		}
		break;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* costruttori                                                        */
/* ------------------------------------------------------------------ */

bui_widget *bui_add(bui_panel *p, int type, bui_rect r)
{
	static bui_widget spare;
	bui_widget *w;

	if (p->n >= BUI_PANEL_MAX) {
		fprintf(stderr, "bui: pannello pieno (%d widget)\n", BUI_PANEL_MAX);
		memset(&spare, 0, sizeof(spare));
		spare.panel = p;
		spare.flags = BUI_F_HIDDEN;
		return &spare;
	}
	w = &p->w[p->n++];
	memset(w, 0, sizeof(*w));
	w->type = (uint8_t)type;
	w->r = r;
	w->panel = p;
	return w;
}

bui_widget *bui_add_label(bui_panel *p, bui_rect r, const char *text, int style, int align)
{
	bui_widget *w = bui_add(p, BUI_W_LABEL, r);

	w->text = text;
	w->style = (uint8_t)style;
	w->align = (uint8_t)align;
	return w;
}

bui_widget *bui_add_button(bui_panel *p, bui_rect r, const char *text, const bui_icon *icon,
			   int style, void (*on_click)(bui_widget *))
{
	bui_widget *w = bui_add(p, BUI_W_BUTTON, r);

	w->text = text;
	w->icon = icon;
	w->style = (uint8_t)style;
	w->on_click = on_click;
	return w;
}

bui_widget *bui_add_tile(bui_panel *p, bui_rect r, const char *text, const bui_icon *icon,
			 void (*on_click)(bui_widget *))
{
	bui_widget *w = bui_add(p, BUI_W_TILE, r);

	w->text = text;
	w->icon = icon;
	w->on_click = on_click;
	return w;
}

bui_widget *bui_add_slider(bui_panel *p, bui_rect r, int vmin, int vmax, int value,
			   void (*on_change)(bui_widget *, int))
{
	bui_widget *w = bui_add(p, BUI_W_SLIDER, r);

	w->vmin = vmin;
	w->vmax = vmax;
	w->value = value;
	w->on_change = on_change;
	return w;
}

bui_widget *bui_add_toggle(bui_panel *p, bui_rect r, const char *text, int on,
			   void (*on_change)(bui_widget *, int))
{
	bui_widget *w = bui_add(p, BUI_W_TOGGLE, r);

	w->text = text;
	w->value = on;
	w->on_change = on_change;
	return w;
}

bui_widget *bui_add_row(bui_panel *p, bui_rect r, const bui_icon *icon, const char *text,
			const char *value, void (*on_click)(bui_widget *))
{
	bui_widget *w = bui_add(p, BUI_W_ROW, r);

	w->icon = icon;
	w->text = text;
	w->text2 = value;
	w->on_click = on_click;
	return w;
}

bui_widget *bui_add_card(bui_panel *p, bui_rect r)
{
	return bui_add(p, BUI_W_CARD, r);
}

bui_widget *bui_add_list(bui_panel *p, bui_rect r, bui_list *l)
{
	bui_widget *w = bui_add(p, BUI_W_LIST, r);

	w->list = l;
	l->w = w;
	l->pressed = -1;
	list_update_max(l);
	return w;
}

bui_widget *bui_add_custom(bui_panel *p, bui_rect r, void (*draw)(bui_widget *, bui_canvas *, bui_rect))
{
	bui_widget *w = bui_add(p, BUI_W_CUSTOM, r);

	w->draw = draw;
	return w;
}

bui_widget *bui_add_segs(bui_panel *p, bui_rect r, const char *labels, int value,
			 void (*on_change)(bui_widget *, int))
{
	bui_widget *w = bui_add(p, BUI_W_SEGS, r);

	w->text = labels;
	w->value = value;
	w->on_change = on_change;
	return w;
}

void bui_header_build(bui_panel *p, bui_rect r, const char *title, void (*on_back)(bui_widget *))
{
	bui_widget *w;

	bui_panel_clear(p);
	bui_panel_set_viewport(p, r, r.h);
	w = bui_add_button(p, bui_r(0, 0, 64, r.h), NULL, BUI_IC(back, 24), BUI_BS_ROUND, on_back);
	w->flags |= BUI_F_NOFOCUS;
	bui_add_label(p, bui_r(68, 0, r.w - 68 - 16, r.h), title, BUI_LS_TITLE, BUI_AL_LEFT);
}

/* ------------------------------------------------------------------ */
/* dialogo di conferma                                                */
/* ------------------------------------------------------------------ */

static struct {
	bui_win *win;
	bui_panel panel;
	bui_dialog d;
	bui_rect card, msg_r;
	char title[96], msg[320], btn[3][40];
	int nbtn, outside;
	int alpha;		/* 255 = del tutto comparso */
	uint32_t fade_start;
} dlg;

#define DLG_FADE_MS 140
#define DLG_SHADOW 14
#define TOAST_IN_MS 170
#define TOAST_OUT_MS 120

static struct {
	bui_win *win;
	char text[200];
	int timer, ms;
	bui_rect pill;
	bui_rect at;		/* posizione finale sullo schermo */
	uint32_t slide_start;
	int closing;
	void (*hook)(const char *);
} toast;

static int toast_slide(void *user, uint32_t now);
static int dlg_fade(void *user, uint32_t now);

static void toast_close(void)
{
	if (!toast.win)
		return;
	if (toast.timer)
		bui_timer_del(toast.timer);
	toast.timer = 0;
	bui_anim_del(toast_slide, NULL);
	bui_popup_close(toast.win);
	toast.win = NULL;
}

void bui_toast_set_hook(void (*hook)(const char *))
{
	toast.hook = hook;
}

/* rettangolo della scheda del dialogo con la sua ombra */
static bui_rect dlg_extent(void)
{
	return bui_r(dlg.card.x - DLG_SHADOW, dlg.card.y - DLG_SHADOW, dlg.card.w + 2 * DLG_SHADOW,
		     dlg.card.h + 2 * DLG_SHADOW + DLG_SHADOW / 3 + 1);
}

static void dlg_finish(int button)
{
	void (*done)(int, void *) = dlg.d.done;
	void *user = dlg.d.user;

	if (!dlg.win)
		return;
	bui_anim_del(dlg_fade, NULL);
	bui_panel_clear(&dlg.panel);
	bui_popup_close(dlg.win);
	dlg.win = NULL;
	if (done)
		done(button, user);
}

static void dlg_click(bui_widget *w)
{
	dlg_finish(w->id);
}

static void dlg_paint(bui_win *win, bui_canvas *c, bui_rect clip)
{
	bui_rect in;

	bui_popup_backdrop(win, c, clip);
	if (!bui_rect_isect(clip, bui_rect_inset(dlg.card, 8, 8), &in) || in.w != clip.w || in.h != clip.h)
		bui_shadow(c, dlg.card, bui_th.r_card + 4, DLG_SHADOW);
	bui_rrect(c, dlg.card, bui_th.r_card + 4, BUI_C_SURFACE);
	bui_rrect_line(c, dlg.card, bui_th.r_card + 4, 1, BUI_C_LINE);
	bui_text_box(c, BUI_FONT_TITLE, bui_r(dlg.card.x + 24, dlg.card.y + 20, dlg.card.w - 48, 32),
		     dlg.title, BUI_C_TEXT, BUI_AL_LEFT);
	bui_text_wrap(c, BUI_FONT_BODY, dlg.msg_r, dlg.msg, BUI_C_TEXT2, 26, BUI_AL_LEFT);
	bui_panel_draw(&dlg.panel, c, clip);
	if (dlg.alpha < 255 && win->backdrop) {
		/* comparsa: la scheda finita viene miscelata col fondo catturato */
		bui_rect saved = bui_clip_push(c, dlg_extent());

		bui_blit_a(c, 0, 0, win->backdrop, win->w, win->h, win->w, 255 - dlg.alpha);
		bui_clip_pop(c, saved);
	}
}

/* budget: scheda + ombra (al massimo ~484x300 px) per 4-5 fotogrammi */
static int dlg_fade(void *user, uint32_t now)
{
	int t = bui_tween(dlg.fade_start, DLG_FADE_MS, now);

	(void)user;
	if (!dlg.win)
		return 0;
	dlg.alpha = t >= 1024 ? 255 : 72 + 183 * bui_ease_out(t) / 1024;
	bui_win_damage(dlg.win, dlg_extent());
	return t < 1024;
}

static void dlg_pointer(bui_win *win, const bui_pointer *ev)
{
	(void)win;
	if (ev->type == BUI_PRESS)
		dlg.outside = !bui_rect_has(dlg.card, ev->x, ev->y);
	if (dlg.outside) {
		if (ev->type == BUI_RELEASE && !bui_rect_has(dlg.card, ev->x, ev->y))
			dlg_finish(dlg.d.cancel);
		return;
	}
	bui_panel_pointer(&dlg.panel, ev);
}

static void dlg_key(bui_win *win, const bui_key *k)
{
	(void)win;
	if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE) {
		dlg_finish(dlg.d.cancel);
		return;
	}
	bui_panel_key(&dlg.panel, k);
}

void bui_dialog_close(void)
{
	if (dlg.win)
		dlg_finish(dlg.d.cancel);
}

int bui_dialog_active(void)
{
	return dlg.win != NULL;
}

void bui_dialog_open(const bui_dialog *d)
{
	int sw = bui_screen_w(), sh = bui_screen_h();
	int cw = sw - 32 > 456 ? 456 : sw - 32;
	int inner = cw - 48, bw[3], tot = 0, i, mh, bh, ch, stacked, x, y;

	if (dlg.win)
		dlg_finish(dlg.d.cancel);
	toast_close();
	dlg.d = *d;
	snprintf(dlg.title, sizeof(dlg.title), "%s", d->title ? d->title : "");
	snprintf(dlg.msg, sizeof(dlg.msg), "%s", d->message ? d->message : "");
	dlg.nbtn = 0;
	for (i = 0; i < 3 && d->buttons[i]; i++) {
		snprintf(dlg.btn[i], sizeof(dlg.btn[i]), "%s", d->buttons[i]);
		bw[i] = bui_text_width(BUI_FONT_BODY_B, dlg.btn[i], -1) + 48;
		if (bw[i] < 120)
			bw[i] = 120;
		tot += bw[i] + (i ? 12 : 0);
		dlg.nbtn++;
	}
	stacked = tot > inner;
	mh = bui_text_wrap(NULL, BUI_FONT_BODY, bui_r(0, 0, inner, 0), dlg.msg, 0, 26, 0);
	bh = stacked ? dlg.nbtn * BUI_TOUCH + (dlg.nbtn - 1) * 8 : BUI_TOUCH;
	ch = 20 + 32 + 8 + mh + 24 + bh + 24;
	dlg.card = bui_r((sw - cw) / 2, (sh - ch) / 2, cw, ch);
	dlg.msg_r = bui_r(dlg.card.x + 24, dlg.card.y + 60, inner, mh);

	dlg.win = bui_popup_open(bui_r(0, 0, sw, sh), BUI_POPUP_MODAL | BUI_POPUP_DIM);
	if (!dlg.win) {
		if (d->done)
			d->done(d->cancel, d->user);
		return;
	}
	dlg.win->paint = dlg_paint;
	dlg.win->pointer = dlg_pointer;
	dlg.win->key = dlg_key;
	dlg.alpha = 255;
	if (bui_anim_level() == BUI_ANIM_FULL && dlg.win->backdrop) {
		dlg.alpha = 72;
		dlg.fade_start = bui_now();
		bui_anim_add(dlg_fade, NULL);
	}
	bui_panel_init(&dlg.panel, dlg.win);
	bui_panel_set_viewport(&dlg.panel, bui_r(0, 0, sw, sh), sh);
	y = dlg.card.y + ch - 24 - bh;
	x = dlg.card.x + cw - 24 - tot;
	for (i = 0; i < dlg.nbtn; i++) {
		bui_widget *w;

		if (stacked) {
			w = bui_add_button(&dlg.panel, bui_r(dlg.card.x + 24, y, inner, BUI_TOUCH),
					   dlg.btn[i], NULL, d->styles[i], dlg_click);
			y += BUI_TOUCH + 8;
		} else {
			w = bui_add_button(&dlg.panel, bui_r(x, y, bw[i], BUI_TOUCH), dlg.btn[i], NULL,
					   d->styles[i], dlg_click);
			x += bw[i] + 12;
		}
		w->id = i;
	}
	dlg.outside = 0;
}

/* ------------------------------------------------------------------ */
/* toast                                                              */
/* ------------------------------------------------------------------ */

/* il toast sale dal bordo in basso e ci ritorna: solo la finestra si sposta
 * (48 righe), il contenuto e' disegnato una volta */
static int toast_slide(void *user, uint32_t now)
{
	int sh = bui_screen_h();
	int t, e, y;

	(void)user;
	if (!toast.win)
		return 0;
	t = bui_tween(toast.slide_start, toast.closing ? TOAST_OUT_MS : TOAST_IN_MS, now);
	e = toast.closing ? 1024 - bui_ease_in_out(t) : bui_ease_out(t);
	y = bui_lerp(sh, toast.at.y, e);
	bui_win_move_resize(toast.win, bui_r(toast.at.x, y, toast.at.w, toast.at.h));
	if (t < 1024)
		return 1;
	if (toast.closing)
		toast_close();
	return 0;
}

static void toast_timeout(void *user)
{
	(void)user;
	toast.timer = 0;
	if (toast.win && bui_anim_level() >= BUI_ANIM_REDUCED && !toast.closing) {
		toast.closing = 1;
		toast.slide_start = bui_now();
		bui_anim_add(toast_slide, NULL);
		return;
	}
	toast_close();
}

static void toast_paint(bui_win *win, bui_canvas *c, bui_rect clip)
{
	(void)win;
	/* il bordo sfumato si fonde con un colore scuro: fa da contorno su ogni fondo */
	bui_fill(c, clip, bui_th.light ? BUI_C_TEXT2 : BUI_C_BAR);
	bui_rrect(c, toast.pill, toast.pill.h / 2, bui_th.light ? BUI_C_TEXT : BUI_C_SURFACE3);
	bui_text_box(c, BUI_FONT_BODY, bui_rect_inset(toast.pill, 24, 0), toast.text,
		     bui_th.light ? BUI_C_SURFACE : BUI_C_TEXT, BUI_AL_CENTER);
}

static void toast_pointer(bui_win *win, const bui_pointer *ev)
{
	(void)win;
	if (ev->type == BUI_RELEASE)
		toast_close();
}

static void toast_show(void *user)
{
	int sw = bui_screen_w(), sh = bui_screen_h();
	int tw, w, ms = toast.ms;
	int slide = bui_anim_level() >= BUI_ANIM_REDUCED;

	(void)user;
	toast.timer = 0;
	if (dlg.win)
		return;
	tw = bui_text_width(BUI_FONT_BODY, toast.text, -1);
	w = tw + 48 > sw - 48 ? sw - 48 : tw + 48;
	toast.at = bui_r((sw - w) / 2, sh - 48 - 32, w, 48);
	toast.closing = 0;
	/* con l'animazione nasce appena sotto lo schermo e sale */
	toast.win = bui_popup_open_round(bui_r(toast.at.x, slide ? sh : toast.at.y, w, 48), 24);
	if (!toast.win)
		return;
	toast.pill = bui_r(0, 0, w, 48);
	toast.win->paint = toast_paint;
	toast.win->pointer = toast_pointer;
	toast.timer = bui_timer_add(ms > 0 ? ms : 2500, 0, toast_timeout, NULL);
	if (slide) {
		toast.slide_start = bui_now();
		bui_anim_add(toast_slide, NULL);
	}
}

void bui_toast(const char *text, int ms)
{
	int since;

	toast_close();
	if (toast.timer)
		bui_timer_del(toast.timer);
	toast.timer = 0;
	if (dlg.win || !text || !*text)
		return;
	snprintf(toast.text, sizeof(toast.text), "%s", text);
	toast.ms = ms;
	if (toast.hook)
		toast.hook(toast.text);
	/* se un popup e' appena sparito, chi stava sotto deve avere il tempo di
	 * ridisegnarsi prima che il toast ne catturi il fondo */
	since = (int32_t)(bui_now() - bui_popup_last_close());
	if (since >= 0 && since < 150)
		toast.timer = bui_timer_add(150 - since, 0, toast_show, NULL);
	else
		toast_show(NULL);
}
