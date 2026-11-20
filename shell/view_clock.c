// SPDX-License-Identifier: MIT
/*
 * view_clock.c - Orologio: ora e data, cronometro con giri, timer.
 *
 * Cronometro e timer continuano anche quando la vista e' chiusa: sono solo
 * istanti memorizzati. I tick esistono soltanto mentre la vista e' visibile
 * e ridisegnano il minimo (i decimi, o i secondi).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "shell.h"

enum { TAB_CLOCK, TAB_STOPWATCH, TAB_TIMER };
enum { TM_IDLE, TM_RUNNING, TM_PAUSED, TM_DONE };

#define MAX_LAPS 99

static struct {
	bui_panel p;
	bui_rect area;
	int tab;
	bui_widget *face;
	bui_rect r_small;		/* parte che cambia piu' spesso (secondi o decimi) */
	bui_rect r_big;			/* le cifre grandi */
	char big[16], small[8], sub[72];

	/* cronometro */
	int sw_running;
	uint32_t sw_start, sw_acc;	/* ms */
	uint32_t laps[MAX_LAPS];	/* tempo totale a ogni giro */
	int nlaps;
	bui_list lap_list;

	/* timer */
	int tm_state;
	int tm_set;			/* durata impostata, secondi */
	uint32_t tm_end;		/* scadenza (orologio monotono) */
	int tm_left;			/* ms rimasti quando e' in pausa */
	int tm_timer;
} K = { .tm_set = 300 };

static void rebuild(void);

int bs_clock_timer_ms(void)
{
	int left;

	if (K.tm_state != TM_RUNNING)
		return -1;
	left = (int32_t)(K.tm_end - bui_now());
	return left > 0 ? left : 0;
}

/* ------------------------------------------------------------------ */
/* stato                                                              */
/* ------------------------------------------------------------------ */

static uint32_t sw_elapsed(void)
{
	return K.sw_acc + (K.sw_running ? bui_now() - K.sw_start : 0);
}

static int tm_remaining_ms(void)
{
	int left;

	switch (K.tm_state) {
	case TM_RUNNING:
		left = (int32_t)(K.tm_end - bui_now());
		return left > 0 ? left : 0;
	case TM_PAUSED:
		return K.tm_left;
	case TM_DONE:
		return 0;
	default:
		return K.tm_set * 1000;
	}
}

static void fmt_ms(char *buf, size_t len, uint32_t ms, int tenths)
{
	unsigned s = ms / 1000, h = s / 3600, m = (s / 60) % 60;
	int n;

	if (h)
		n = snprintf(buf, len, "%u:%02u:%02u", h, m, s % 60);
	else
		n = snprintf(buf, len, "%02u:%02u", m, s % 60);
	if (tenths && n > 0 && (size_t)n < len)
		snprintf(buf + n, len - (size_t)n, ",%u", (ms / 100) % 10);
}

/* aggiorna i testi della faccia; ritorna 2 se cambia la parte grande, 1 solo la piccola */
static int update_texts(void)
{
	char big[sizeof(K.big)], small[sizeof(K.small)];
	time_t t;
	struct tm tm;
	uint32_t ms;
	int r = 0;

	small[0] = 0;
	switch (K.tab) {
	case TAB_CLOCK:
		t = time(NULL);
		localtime_r(&t, &tm);
		snprintf(big, sizeof(big), "%02d:%02d", tm.tm_hour, tm.tm_min);
		snprintf(small, sizeof(small), "%02d", tm.tm_sec);
		bs_fmt_date(K.sub, sizeof(K.sub), 1);
		break;
	case TAB_STOPWATCH:
		ms = sw_elapsed();
		fmt_ms(big, sizeof(big), ms, 0);
		snprintf(small, sizeof(small), ",%u", (unsigned)(ms / 100) % 10);
		K.sub[0] = 0;
		break;
	default:
		/* arrotonda per eccesso: "00:01" resta finche' il secondo non e' passato */
		fmt_ms(big, sizeof(big), (uint32_t)(tm_remaining_ms() + 999) / 1000 * 1000, 0);
		snprintf(K.sub, sizeof(K.sub), "%s",
			 K.tm_state == TM_DONE ? "Tempo scaduto" : K.tm_state == TM_PAUSED ? "In pausa" : "");
		break;
	}
	if (strcmp(big, K.big))
		r = 2;
	else if (strcmp(small, K.small))
		r = 1;
	memcpy(K.big, big, sizeof(big));
	memcpy(K.small, small, sizeof(small));
	return r;
}

static void arm_tick(void)
{
	struct timespec ts;

	switch (K.tab) {
	case TAB_CLOCK:
		clock_gettime(CLOCK_REALTIME, &ts);
		bs_view_tick_in(1000 - (int)(ts.tv_nsec / 1000000) + 5);
		break;
	case TAB_STOPWATCH:
		if (K.sw_running)
			bs_view_tick_in(100 - (int)(sw_elapsed() % 100) + 1);
		else
			bs_view_tick_in(0);
		break;
	default:
		if (K.tm_state == TM_RUNNING)
			bs_view_tick_in(tm_remaining_ms() % 1000 + 5);
		else
			bs_view_tick_in(0);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* disegno della faccia                                               */
/* ------------------------------------------------------------------ */

static void face_draw(bui_widget *w, bui_canvas *c, bui_rect r)
{
	const bui_font *fb = BUI_FONT_HUGE, *fs = BUI_FONT_NUM;
	int wb = bui_text_width(fb, K.big, -1);
	int ws = K.small[0] ? bui_text_width(fs, K.tab == TAB_CLOCK ? "00" : ",0", -1) + 10 : 0;
	int has_sub = K.sub[0] || K.tab == TAB_TIMER;
	int block = fb->cap_h + (has_sub ? 44 : 0);
	int x = r.x + (r.w - wb - ws) / 2;
	int base = r.y + (r.h - block) / 2 + fb->cap_h;
	bui_px col = K.tab == TAB_TIMER && K.tm_state == TM_DONE ? BUI_C_ACCENT : BUI_C_TEXT;

	(void)w;
	bui_text(c, fb, x, base, K.big, col);
	K.r_big = bui_r(x - 4, base - fb->cap_h - 6, wb + 8, fb->cap_h + 12);
	if (K.small[0]) {
		bui_text(c, fs, x + wb + 10, base, K.small, BUI_C_TEXT2);
		K.r_small = bui_r(x + wb + 4, base - fs->cap_h - 6, ws + 6, fs->cap_h + 20);
	}
	if (K.tab == TAB_TIMER && (K.tm_state == TM_RUNNING || K.tm_state == TM_PAUSED)) {
		/* barra di avanzamento */
		int bw = wb, tot = K.tm_set * 1000;
		int done = tot > 0 ? (int)((long long)(tot - tm_remaining_ms()) * bw / tot) : 0;

		bui_rrect(c, bui_r(x, base + 16, bw, 6), 3, BUI_C_SURFACE2);
		if (done > 3)
			bui_rrect(c, bui_r(x, base + 16, done > bw ? bw : done, 6), 3, BUI_C_ACCENT);
		if (K.sub[0])
			bui_text_box(c, BUI_FONT_BODY, bui_r(r.x, base + 28, r.w, 26), K.sub, BUI_C_TEXT2,
				     BUI_AL_CENTER);
	} else if (K.sub[0]) {
		bui_text_box(c, BUI_FONT_BODY, bui_r(r.x, base + 18, r.w, 28), K.sub,
			     K.tab == TAB_TIMER ? BUI_C_ACCENT : BUI_C_TEXT2, BUI_AL_CENTER);
	}
}

static void refresh_face(void)
{
	char sub[sizeof(K.sub)];
	size_t len = strlen(K.big);
	int ch;

	memcpy(sub, K.sub, sizeof(sub));
	ch = update_texts();
	if (!K.face)
		return;
	if (K.tab == TAB_TIMER || strcmp(sub, K.sub) || strlen(K.big) != len) {
		/* cambia la disposizione (o la barra del timer): tutta la faccia */
		bui_widget_damage(K.face);
	} else if (ch == 2) {
		bs_view_damage(K.r_big);
		bs_view_damage(K.r_small);
	} else if (ch == 1) {
		bs_view_damage(K.r_small);
	}
}

/* ------------------------------------------------------------------ */
/* azioni                                                             */
/* ------------------------------------------------------------------ */

static void on_tab(bui_widget *w)
{
	if (K.tab == w->id)
		return;
	K.tab = w->id;
	rebuild();
}

static void sw_toggle(bui_widget *w)
{
	(void)w;
	if (K.sw_running) {
		K.sw_acc = sw_elapsed();
		K.sw_running = 0;
	} else {
		K.sw_start = bui_now();
		K.sw_running = 1;
	}
	rebuild();
}

static void sw_lap(bui_widget *w)
{
	(void)w;
	if (!K.sw_running || K.nlaps >= MAX_LAPS)
		return;
	memmove(&K.laps[1], &K.laps[0], sizeof(K.laps[0]) * (size_t)K.nlaps);
	K.laps[0] = sw_elapsed();
	K.nlaps++;
	bui_list_set_count(&K.lap_list, K.nlaps);
	bui_list_reveal(&K.lap_list, 0);
}

static void sw_reset(bui_widget *w)
{
	(void)w;
	K.sw_running = 0;
	K.sw_acc = 0;
	K.nlaps = 0;
	rebuild();
}

static void lap_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	char name[24], split[24], total[24];
	int n = K.nlaps - i;	/* il piu' recente in cima */
	uint32_t prev = i + 1 < K.nlaps ? K.laps[i + 1] : 0;

	(void)l;
	snprintf(name, sizeof(name), "Giro %d", n);
	fmt_ms(split, sizeof(split), K.laps[i] - prev, 1);
	fmt_ms(total, sizeof(total), K.laps[i], 1);
	bui_list_row(c, r, flags & BUI_F_FOCUS, NULL, name, total, split);
}

static void tm_fire(void *user)
{
	(void)user;
	K.tm_timer = 0;
	if (K.tm_state != TM_RUNNING)
		return;
	K.tm_state = TM_DONE;
	bui_toast("Timer terminato", 4000);
	if (bs_view_is_active(&bs_view_clock) && K.tab == TAB_TIMER)
		rebuild();
}

static void tm_cancel_timer(void)
{
	if (K.tm_timer)
		bui_timer_del(K.tm_timer);
	K.tm_timer = 0;
}

static void tm_toggle(bui_widget *w)
{
	(void)w;
	switch (K.tm_state) {
	case TM_RUNNING:
		K.tm_left = tm_remaining_ms();
		K.tm_state = TM_PAUSED;
		tm_cancel_timer();
		break;
	case TM_PAUSED:
		K.tm_end = bui_now() + (uint32_t)K.tm_left;
		K.tm_state = TM_RUNNING;
		K.tm_timer = bui_timer_add(K.tm_left, 0, tm_fire, NULL);
		break;
	default:
		if (K.tm_set <= 0)
			return;
		K.tm_end = bui_now() + (uint32_t)K.tm_set * 1000u;
		K.tm_state = TM_RUNNING;
		K.tm_timer = bui_timer_add(K.tm_set * 1000, 0, tm_fire, NULL);
		break;
	}
	rebuild();
}

static void tm_reset(bui_widget *w)
{
	(void)w;
	tm_cancel_timer();
	K.tm_state = TM_IDLE;
	rebuild();
}

static void tm_set(int seconds)
{
	if (seconds < 10)
		seconds = 10;
	if (seconds > 99 * 60)
		seconds = 99 * 60;
	tm_cancel_timer();
	K.tm_state = TM_IDLE;
	K.tm_set = seconds;
}

static void tm_preset(bui_widget *w)
{
	tm_set(w->id * 60);
	rebuild();
}

static void tm_adjust(bui_widget *w)
{
	if (K.tm_state != TM_IDLE && K.tm_state != TM_DONE)
		return;
	tm_set(K.tm_set + w->id);
	K.tm_state = TM_IDLE;
	rebuild();
}

/* ------------------------------------------------------------------ */
/* costruzione                                                        */
/* ------------------------------------------------------------------ */

static void rebuild(void)
{
	static const char *const tabs[3] = { "Orologio", "Cronometro", "Timer" };
	static const struct {
		const char *label;
		int min;
	} presets[6] = { { "1 min", 1 }, { "3 min", 3 }, { "5 min", 5 },
			 { "10 min", 10 }, { "15 min", 15 }, { "30 min", 30 } };
	bui_panel *p = &K.p;
	bui_rect a = K.area;
	int land = a.w > a.h;
	int focus_tab = p->focus_vis && p->focus && p->focus->style == BUI_BS_SEG;
	int tw = a.w - 32 < 3 * 168 ? (a.w - 32 - 8) / 3 : 164;
	int tx = (a.w - (3 * tw + 8)) / 2, top = 8 + 64 + 8;
	int i, bw, bx, by, fw, fh;
	bui_widget *w;

	bui_panel_clear(p);
	bui_panel_set_viewport(p, a, a.h);

	/* selettore a tre schede */
	w = bui_add_card(p, bui_r(tx, 8, 3 * tw + 8, 64));
	for (i = 0; i < 3; i++) {
		w = bui_add_button(p, bui_r(tx + 4 + i * tw, 12, tw, 56), tabs[i], NULL, BUI_BS_SEG, on_tab);
		w->id = i;
		if (i == K.tab) {
			w->flags |= BUI_F_SELECTED;
			if (focus_tab)
				bui_panel_focus(p, w, 1);
		}
	}

	K.face = NULL;
	K.big[0] = K.small[0] = 0;
	switch (K.tab) {
	case TAB_CLOCK:
		K.face = bui_add_custom(p, bui_r(0, top, a.w, a.h - top - 16), face_draw);
		break;

	case TAB_STOPWATCH:
		K.lap_list.row_h = 56;
		K.lap_list.count = K.nlaps;
		K.lap_list.draw_row = lap_row;
		K.lap_list.empty_text = "Nessun giro registrato";
		K.lap_list.sel = -1;
		if (land) {
			fw = a.w - 16 - 312 - 16;
			fh = a.h - top - 56 - 28;
			w = bui_add_card(p, bui_r(a.w - 16 - 312, top, 312, a.h - top - 12));
			bui_add_list(p, bui_r(a.w - 16 - 312, top + 8, 312, a.h - top - 12 - 16), &K.lap_list);
			by = a.h - 56 - 16;
		} else {
			fw = a.w;
			fh = 200;
			by = top + fh + 8;
			w = bui_add_card(p, bui_r(16, by + 56 + 20, a.w - 32, a.h - (by + 56 + 20) - 12));
			bui_add_list(p, bui_r(16, by + 56 + 28, a.w - 32, a.h - (by + 56 + 28) - 20),
				     &K.lap_list);
		}
		K.face = bui_add_custom(p, bui_r(0, top, fw, fh), face_draw);
		bw = fw - 32 < 3 * 148 + 16 ? (fw - 32 - 16) / 3 : 148;
		bx = (fw - (3 * bw + 16)) / 2;
		bui_add_button(p, bui_r(bx, by, bw, 56), K.sw_running ? "Pausa" : sw_elapsed() ? "Riprendi" : "Avvia",
			       K.sw_running ? BUI_IC(pause, 24) : BUI_IC(play, 24), BUI_BS_FILLED, sw_toggle);
		w = bui_add_button(p, bui_r(bx + bw + 8, by, bw, 56), "Giro", BUI_IC(flag, 24), BUI_BS_TONAL,
				   sw_lap);
		if (!K.sw_running)
			w->flags |= BUI_F_DISABLED;
		w = bui_add_button(p, bui_r(bx + 2 * (bw + 8), by, bw, 56), "Azzera", BUI_IC(replay, 24),
				   BUI_BS_TONAL, sw_reset);
		if (!K.sw_running && !sw_elapsed())
			w->flags |= BUI_F_DISABLED;
		break;

	default:
		fh = land ? a.h - top - 56 - 12 - 56 - 20 : 220;
		/* -1 min / +1 min ai lati delle cifre */
		bx = a.w / 2 - 150 - 24 - 64;
		if (bx < 12)
			bx = 12;
		K.face = bui_add_custom(p, bui_r(bx + 64, top, a.w - 2 * (bx + 64), fh), face_draw);
		w = bui_add_button(p, bui_r(bx, top + (fh - 64) / 2, 64, 64), NULL, BUI_IC(remove, 24),
				   BUI_BS_TONAL, tm_adjust);
		w->id = -60;
		if (K.tm_state == TM_RUNNING || K.tm_state == TM_PAUSED || K.tm_set <= 60)
			w->flags |= BUI_F_DISABLED;
		w = bui_add_button(p, bui_r(a.w - bx - 64, top + (fh - 64) / 2, 64, 64), NULL,
				   BUI_IC(add, 24), BUI_BS_TONAL, tm_adjust);
		w->id = 60;
		if (K.tm_state == TM_RUNNING || K.tm_state == TM_PAUSED)
			w->flags |= BUI_F_DISABLED;
		/* durate pronte */
		by = top + fh + 4;
		if (land) {
			bw = (a.w - 32 - 5 * 8) / 6;
			for (i = 0; i < 6; i++) {
				w = bui_add_button(p, bui_r(16 + i * (bw + 8), by, bw, 56), presets[i].label,
						   NULL, BUI_BS_TONAL, tm_preset);
				w->id = presets[i].min;
				if (K.tm_set == presets[i].min * 60)
					w->flags |= BUI_F_SELECTED;
			}
			by += 56 + 12;
		} else {
			bw = (a.w - 32 - 2 * 8) / 3;
			for (i = 0; i < 6; i++) {
				w = bui_add_button(p, bui_r(16 + (i % 3) * (bw + 8), by + (i / 3) * 64, bw, 56),
						   presets[i].label, NULL, BUI_BS_TONAL, tm_preset);
				w->id = presets[i].min;
				if (K.tm_set == presets[i].min * 60)
					w->flags |= BUI_F_SELECTED;
			}
			by += 2 * 64 + 12;
		}
		bw = a.w - 32 < 2 * 200 + 8 ? (a.w - 32 - 8) / 2 : 200;
		bx = (a.w - (2 * bw + 8)) / 2;
		bui_add_button(p, bui_r(bx, by, bw, 56),
			       K.tm_state == TM_RUNNING ? "Pausa" : K.tm_state == TM_PAUSED ? "Riprendi" : "Avvia",
			       K.tm_state == TM_RUNNING ? BUI_IC(pause, 24) : BUI_IC(play, 24), BUI_BS_FILLED,
			       tm_toggle);
		w = bui_add_button(p, bui_r(bx + bw + 8, by, bw, 56), "Azzera", BUI_IC(replay, 24),
				   BUI_BS_TONAL, tm_reset);
		if (K.tm_state == TM_IDLE)
			w->flags |= BUI_F_DISABLED;
		break;
	}
	update_texts();
	bs_view_damage(a);
	arm_tick();
}

/* ------------------------------------------------------------------ */
/* interfaccia della vista                                            */
/* ------------------------------------------------------------------ */

static void ck_enter(bs_view *v)
{
	bui_panel_init(&K.p, v->win);
}

static void ck_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&K.p);
	K.face = NULL;
}

static void ck_layout(bs_view *v, bui_rect a)
{
	(void)v;
	K.area = a;
	rebuild();
}

static void ck_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&K.p, c, clip);
}

static int ck_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&K.p, ev);
}

static int ck_key(bs_view *v, const bui_key *k)
{
	(void)v;
	if (k->key == ' ' && K.tab != TAB_CLOCK) {	/* spazio: avvia / ferma */
		if (K.tab == TAB_STOPWATCH)
			sw_toggle(NULL);
		else
			tm_toggle(NULL);
		return 1;
	}
	if (bui_panel_key(&K.p, k))
		return 1;
	if (k->key >= '1' && k->key <= '3') {
		if (K.tab != k->key - '1') {
			K.tab = k->key - '1';
			rebuild();
		}
		return 1;
	}
	return 0;
}

static void ck_tick(bs_view *v, uint32_t now)
{
	(void)v;
	(void)now;
	refresh_face();
	arm_tick();
}

bs_view bs_view_clock = {
	.enter = ck_enter,
	.leave = ck_leave,
	.layout = ck_layout,
	.draw = ck_draw,
	.pointer = ck_pointer,
	.key = ck_key,
	.tick = ck_tick,
};
