// SPDX-License-Identifier: MIT
/*
 * bar.c - barra di stato (finestra dock in alto) e pannello delle
 * impostazioni rapide.
 *
 *   [Home] [linguette delle finestre esterne ...]   [volume] [batteria %] [HH:MM]
 *
 * Ogni parte ha il suo rettangolo: l'orologio ridisegna solo il proprio
 * una volta al minuto, la batteria solo quando cambia.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "shell.h"

#define MAX_CHIPS BS_MAX_CLIENTS
#define CHIP_H 30
#define CHIP_CLOSE_W 32

enum { Z_NONE, Z_HOME, Z_STATUS, Z_CHIP, Z_CLOSE, Z_CLOCK };
enum { POP_QS, POP_CAL };

#define SLIDE_IN_MS 170		/* il pannello scende da sotto la barra */
#define SLIDE_OUT_MS 120
#define PULSE_MS 960		/* un battito dell'icona della batteria in carica */

static struct {
	bui_win *win;
	bui_rect r_home, r_chips, r_status, r_vol, r_bat, r_clock;
	struct {
		bui_rect r;
		unsigned long id;
	} chip[MAX_CHIPS];
	int nchips;
	int more;		/* finestre senza linguetta per mancanza di spazio */
	bui_rect r_more;
	int zone, zone_on;
	unsigned long zone_id;
	char clock[8];
	int minute_timer;
	unsigned chips_sig;

	/* impostazioni rapide */
	bui_win *qs;
	bui_panel qsp;
	bui_rect qs_card;
	bui_widget *qs_bright, *qs_vol, *qs_batl;
	char qs_date[64], qs_bat[64];
	int shot_timer;
	/* il pannello aperto (impostazioni rapide o calendario) e il suo scorrimento */
	int pop_kind;
	int slide_off;		/* di quanti px la scheda e' ancora nascosta sopra */
	int slide_closing;
	uint32_t slide_start;
	/* calendario */
	int cal_y, cal_m;	/* mese mostrato (cal_m: 0..11) */
	char cal_title[40];
	/* battito della batteria in carica */
	int pulse_timer, pulse_k;
	uint32_t pulse_start;
	/* ultimi toast */
	struct {
		char text[96], when[8];
	} note[BS_NOTES_MAX];
	int nnotes;
} B;

/* ------------------------------------------------------------------ */
/* geometria                                                          */
/* ------------------------------------------------------------------ */

static void layout_chips(void)
{
	const bs_client *cl;
	int n = bs_wm_clients(&cl);
	int avail = B.r_chips.w, cw, i, x;

	B.nchips = 0;
	B.more = 0;
	if (n <= 0 || avail < 72)
		return;
	if (n > MAX_CHIPS)
		n = MAX_CHIPS;
	if ((avail - (n - 1) * 6) / n < 88) {
		/* non entrano tutte: in fondo resta un "+N" */
		avail -= 36;
		while (n > 1 && (avail - (n - 1) * 6) / n < 88)
			n--;
		B.more = bs_wm_clients(&cl) - n;
	}
	cw = (avail - (n - 1) * 6) / n;
	if (cw > 184)
		cw = 184;
	x = B.r_chips.x;
	for (i = 0; i < n; i++) {
		B.chip[i].r = bui_r(x, (BS_BAR_H - CHIP_H) / 2, cw, CHIP_H);
		B.chip[i].id = cl[i].id;
		x += cw + 6;
	}
	B.nchips = n;
	B.r_more = bui_r(x, 0, 36, BS_BAR_H);
}

static void layout(void)
{
	int w = B.win->w, x;
	int clock_w = bui_text_width(BUI_FONT_BODY_B, "00:00", -1);
	int pct_w = bs_pref_battery_percent ? bui_text_width(BUI_FONT_SMALL_B, "100%", -1) + 6 : 0;

	B.r_home = bui_r(0, 0, 56, BS_BAR_H);
	x = w - 12 - clock_w;
	B.r_clock = bui_r(x - 2, 0, clock_w + 4, BS_BAR_H);
	x -= 12 + pct_w + 28 + 14;	/* percentuale, batteria, lampo */
	B.r_bat = bui_r(x, 0, pct_w + 28 + 14, BS_BAR_H);
	x -= 8 + 24;
	B.r_vol = bui_r(x, 0, 24, BS_BAR_H);
	/* l'orologio e' un pulsante a parte (calendario) */
	B.r_status = bui_r(x - 10, 0, B.r_clock.x - 6 - (x - 10), BS_BAR_H);
	B.r_chips = bui_r(60, 0, B.r_status.x - 60 - 6, BS_BAR_H);
	layout_chips();
}

static void update_clock(void)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	snprintf(B.clock, sizeof(B.clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
}

/* ------------------------------------------------------------------ */
/* disegno                                                            */
/* ------------------------------------------------------------------ */

static void draw_battery(bui_canvas *c, bui_rect r)
{
	const bs_battery *b = bs_battery_get();
	int charging = b->status == BS_BAT_CHARGING;
	int x = r.x + 14, y = r.y + (r.h - 14) / 2;
	bui_px fill = BUI_C_TEXT;
	char pct[16];

	if (charging || b->status == BS_BAT_FULL)
		fill = B.pulse_k ? bui_mix(BUI_C_ACCENT, BUI_C_TEXT, B.pulse_k) : BUI_C_ACCENT;
	else if (b->capacity >= 0 && b->capacity <= 10)
		fill = BUI_C_DANGER;
	else if (b->capacity >= 0 && b->capacity <= 20)
		fill = BUI_C_WARN;
	if (charging)
		bui_icon_draw(c, BUI_IC(bolt, 16), r.x - 1, r.y + (r.h - 16) / 2, BUI_C_ACCENT);
	bui_rrect_line(c, bui_r(x, y, 24, 14), 4, 2, BUI_C_TEXT2);
	bui_fill(c, bui_r(x + 25, y + 4, 2, 6), BUI_C_TEXT2);
	if (b->capacity >= 0) {
		int w = 18 * b->capacity / 100;

		bui_fill(c, bui_r(x + 3, y + 3, w < 2 ? 2 : w, 8), fill);
	}
	if (!bs_pref_battery_percent)
		return;
	if (b->capacity >= 0)
		snprintf(pct, sizeof(pct), "%d%%", b->capacity);
	else
		snprintf(pct, sizeof(pct), "--");
	bui_text_box(c, BUI_FONT_SMALL_B, bui_r(x + 28 + 5, r.y, r.w - 14 - 28 - 5, r.h), pct,
		     BUI_C_TEXT, BUI_AL_LEFT);
}

static const bui_icon *volume_icon(void)
{
	int v = bs_volume();

	if (v == 0)
		return BUI_IC(vol_off, 24);
	return v > 0 && v < 40 ? BUI_IC(vol_down, 24) : BUI_IC(vol_up, 24);
}

/* quando lo spazio e' poco solo la linguetta attiva mostra la X di chiusura */
static int chip_closable(int i)
{
	return B.chip[i].r.w >= 120 || B.chip[i].id == bs_wm_active();
}

static void draw_chip(bui_canvas *c, int i)
{
	const bs_client *cl;
	int n = bs_wm_clients(&cl), j;
	bui_rect r = B.chip[i].r;
	const char *title = "";
	int active = B.chip[i].id == bs_wm_active();
	int p_main = B.zone_on && B.zone == Z_CHIP && B.zone_id == B.chip[i].id;
	int p_close = B.zone_on && B.zone == Z_CLOSE && B.zone_id == B.chip[i].id;

	for (j = 0; j < n; j++)
		if (cl[j].id == B.chip[i].id)
			title = cl[j].title;
	int closable = chip_closable(i);

	bui_rrect(c, r, CHIP_H / 2, p_main ? BUI_C_SURFACE3 : active ? BUI_C_ACCENT_BG : BUI_C_SURFACE2);
	if (p_close)
		bui_rrect(c, bui_r(r.x + r.w - CHIP_H, r.y, CHIP_H, CHIP_H), CHIP_H / 2, BUI_C_SURFACE3);
	bui_text_box(c, active ? BUI_FONT_SMALL_B : BUI_FONT_SMALL,
		     bui_r(r.x + 14, r.y, r.w - (closable ? 14 + CHIP_CLOSE_W : 24), r.h), title,
		     active ? BUI_C_ACCENT : BUI_C_TEXT, BUI_AL_LEFT);
	if (closable)
		bui_icon_in(c, BUI_IC(close, 18), bui_r(r.x + r.w - CHIP_H, r.y, CHIP_H, CHIP_H),
			    active ? BUI_C_ACCENT : BUI_C_TEXT2);
}

/* zona di tocco dell'orologio: fino al bordo destro */
static bui_rect clock_zone(void)
{
	return bui_r(B.r_clock.x - 6, 0, B.win->w - (B.r_clock.x - 6), BS_BAR_H);
}

static void bar_paint(bui_win *win, bui_canvas *c, bui_rect clip)
{
	int i;

	(void)win;
	bui_fill(c, clip, BUI_C_BAR);
	if (bui_rect_isect(clip, B.r_home, NULL)) {
		if (B.zone_on && B.zone == Z_HOME)
			bui_rrect(c, bui_rect_inset(B.r_home, 6, 4), 12, BUI_C_SURFACE2);
		bui_icon_in(c, BUI_IC(home, 24), B.r_home, BUI_C_TEXT);
	}
	for (i = 0; i < B.nchips; i++)
		if (bui_rect_isect(clip, B.chip[i].r, NULL))
			draw_chip(c, i);
	if (B.more > 0) {
		char more[16];

		snprintf(more, sizeof(more), "+%d", B.more > 99 ? 99 : B.more);
		bui_text_box(c, BUI_FONT_SMALL_B, B.r_more, more, BUI_C_TEXT2, BUI_AL_CENTER);
	}
	if (bui_rect_isect(clip, B.r_status, NULL)) {
		if ((B.zone_on && B.zone == Z_STATUS) || (B.qs && B.pop_kind == POP_QS))
			bui_rrect(c, bui_rect_inset(B.r_status, 2, 4), 12, BUI_C_SURFACE2);
		bui_icon_in(c, volume_icon(), B.r_vol, bs_volume() < 0 ? BUI_C_TEXT3 : BUI_C_TEXT);
		draw_battery(c, B.r_bat);
	}
	if (bui_rect_isect(clip, clock_zone(), NULL)) {
		if ((B.zone_on && B.zone == Z_CLOCK) || (B.qs && B.pop_kind == POP_CAL))
			bui_rrect(c, bui_rect_inset(clock_zone(), 1, 4), 12, BUI_C_SURFACE2);
		bui_text_box(c, BUI_FONT_BODY_B, B.r_clock, B.clock, BUI_C_TEXT, BUI_AL_CENTER);
	}
}

/* ------------------------------------------------------------------ */
/* conferme e azioni comuni                                           */
/* ------------------------------------------------------------------ */

static void qs_close(void);	/* subito */
static void qs_dismiss(void);	/* con l'animazione di chiusura */

static void rotate_done(int button, void *user)
{
	(void)user;
	if (button != 1)
		return;
	qs_close();
	bs_ctl("rotate", NULL, NULL);
	bui_toast("Rotazione dello schermo: riavvio in corso\xE2\x80\xA6", 5000);
}

void bs_ask_rotate(void)
{
	bui_dialog d = {
		.title = "Ruotare lo schermo?",
		.message = "Per cambiare orientamento il dispositivo deve riavviarsi. "
			   "Salva il lavoro aperto prima di continuare.",
		.buttons = { "Annulla", "Ruota e riavvia" },
		.styles = { BUI_BS_TONAL, BUI_BS_FILLED },
		.cancel = -1,
		.done = rotate_done,
	};

	bui_dialog_open(&d);
}

void bs_bar_close_popups(void)
{
	qs_close();
}

static void qs_open(void);

void bs_bar_quick_toggle(void)
{
	if (B.qs && B.pop_kind == POP_QS)
		qs_dismiss();
	else
		qs_open();
}

int bs_bar_quick_open(void)
{
	return B.qs != NULL;
}

void bs_ask_power(void)
{
	/* lo stesso menu della pressione lunga del tasto di accensione */
	qs_close();
	bs_power_menu();
}

static void shot_done(int status, const char *out, void *user)
{
	char msg[160];
	const char *name = strrchr(out, '/');

	(void)user;
	if (status != 0 || !*out) {
		bui_toast("Schermata non riuscita", 3000);
		return;
	}
	snprintf(msg, sizeof(msg), "Schermata salvata: %s", name ? name + 1 : out);
	bui_toast(msg, 3500);
}

static void shot_timer(void *user)
{
	(void)user;
	B.shot_timer = 0;
	bs_ctl("screenshot", shot_done, NULL);
}

void bs_screenshot(void)
{
	/* lascia il tempo a pannelli e pulsanti premuti di sparire dallo schermo */
	if (!B.shot_timer)
		B.shot_timer = bui_timer_add(400, 0, shot_timer, NULL);
}

/* ------------------------------------------------------------------ */
/* impostazioni rapide                                                */
/* ------------------------------------------------------------------ */

static int pop_slide(void *user, uint32_t now);

static void qs_close(void)
{
	bui_rect z;

	if (!B.qs)
		return;
	bui_anim_del(pop_slide, NULL);
	bui_panel_clear(&B.qsp);
	bui_popup_close(B.qs);
	B.qs = NULL;
	B.qs_bright = B.qs_vol = B.qs_batl = NULL;
	z = B.pop_kind == POP_CAL ? clock_zone() : B.r_status;
	bui_win_damage(B.win, z);
}

/*
 * Il pannello scende da sotto la barra e ci risale. A ogni fotogramma si
 * ridisegna solo la parte del popup fin dove arriva la scheda (al massimo
 * ~424x310 px, un terzo dello schermo): fondo catturato, ombra da maschera,
 * scheda e i suoi pochi widget.
 */
static int pop_slide(void *user, uint32_t now)
{
	int full = B.qs_card.y + B.qs_card.h, t, old = B.slide_off;

	(void)user;
	if (!B.qs)
		return 0;
	t = bui_tween(B.slide_start, B.slide_closing ? SLIDE_OUT_MS : SLIDE_IN_MS, now);
	B.slide_off = B.slide_closing ? full * bui_ease_in_out(t) / 1024 : full - full * bui_ease_out(t) / 1024;
	if (t >= 1024 && B.slide_closing) {
		qs_close();
		return 0;
	}
	/* righe toccate: da cima fino al fondo della scheda (con l'ombra) nella
	 * posizione piu' bassa fra la vecchia e la nuova */
	bui_win_damage(B.qs, bui_r(0, 0, B.qs->w, full - (old < B.slide_off ? old : B.slide_off) + 20));
	return t < 1024;
}

static void pop_slide_start(int closing)
{
	B.slide_closing = closing;
	B.slide_start = bui_now();
	if (!closing)
		B.slide_off = B.qs_card.y + B.qs_card.h;
	bui_anim_add(pop_slide, NULL);
}

static void qs_dismiss(void)
{
	if (!B.qs)
		return;
	if (bui_anim_level() != BUI_ANIM_FULL || B.slide_closing) {
		qs_close();
		return;
	}
	pop_slide_start(1);
}

static void qs_paint(bui_win *win, bui_canvas *c, bui_rect clip)
{
	bui_rect card = B.qs_card, in;
	int rad = bui_th.r_card + 4;

	card.y -= B.slide_off;
	B.qsp.vp.y = card.y;	/* i widget seguono la scheda */
	bui_popup_backdrop(win, c, clip);
	if (!bui_rect_isect(clip, bui_rect_inset(card, 8, 8), &in) || in.w != clip.w || in.h != clip.h)
		bui_shadow(c, card, rad, 12);
	bui_rrect(c, card, rad, BUI_C_SURFACE);
	bui_rrect_line(c, card, rad, 1, BUI_C_LINE);
	bui_panel_draw(&B.qsp, c, clip);
}

static void qs_pointer(bui_win *win, const bui_pointer *ev)
{
	(void)win;
	if (ev->type == BUI_PRESS && (B.slide_closing || !bui_rect_has(B.qs_card, ev->x, ev->y))) {
		if (B.slide_closing)
			qs_close();
		else
			qs_dismiss();
		return;
	}
	bui_panel_pointer(&B.qsp, ev);
}

static void cal_shift(int dm);

static void qs_key(bui_win *win, const bui_key *k)
{
	(void)win;
	if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE) {
		qs_dismiss();
		return;
	}
	if (B.pop_kind == POP_CAL && (k->key == BUI_K_PGUP || k->key == BUI_K_PGDN)) {
		cal_shift(k->key == BUI_K_PGDN ? 1 : -1);
		return;
	}
	bui_panel_key(&B.qsp, k);
}

/* apre il popup ancorato sotto la barra, a destra; ritorna 0 se riuscito */
static int pop_open(int kind, int cw, int ch)
{
	int sw = bui_screen_w(), m = 16;

	if (bui_dialog_active())
		return -1;
	qs_close();
	B.qs = bui_popup_open(bui_r(sw - 8 - cw - m, BS_BAR_H - 2, cw + 2 * m, ch + 6 + m + 8),
			      BUI_POPUP_MODAL);
	if (!B.qs)
		return -1;
	B.pop_kind = kind;
	B.qs_card = bui_r(B.qs->w - m - cw, 6, cw, ch);
	B.qs->paint = qs_paint;
	B.qs->pointer = qs_pointer;
	B.qs->key = qs_key;
	B.slide_off = 0;
	B.slide_closing = 0;
	bui_panel_init(&B.qsp, B.qs);
	bui_panel_set_viewport(&B.qsp, B.qs_card, ch);
	if (bui_anim_level() == BUI_ANIM_FULL)
		pop_slide_start(0);
	return 0;
}

static void qs_on_bright(bui_widget *w, int final)
{
	bs_set_brightness(w->value, final);
}

static void qs_on_vol(bui_widget *w, int final)
{
	bs_set_volume(w->value, final);
}

static void qs_on_rotate(bui_widget *w)
{
	(void)w;
	bs_ask_rotate();	/* il pannello resta sotto: "Annulla" vi ritorna */
}

static void qs_on_shot(bui_widget *w)
{
	(void)w;
	qs_close();
	bs_screenshot();
}

static void qs_on_power(bui_widget *w)
{
	(void)w;
	bs_ask_power();
}

static void qs_texts(void)
{
	bs_fmt_date(B.qs_date, sizeof(B.qs_date), BS_DATE_SHORT);
	/* "87% · 3 h 20 min" se c'e' una stima, altrimenti lo stato */
	bs_battery_text(B.qs_bat, sizeof(B.qs_bat));
}

static void qs_on_saver(bui_widget *w, int final)
{
	(void)final;
	bs_power_saver_set(w->value);
}

static void qs_open(void)
{
	int sw = bui_screen_w();
	int cw = sw - 16 < 392 ? sw - 16 : 392;
	int ch = 16 + 32 + 8 + 56 + 56 + 12 + 84 + 8 + 56 + 8;
	int bw, y;
	bui_panel *p = &B.qsp;
	bui_widget *w;

	if (pop_open(POP_QS, cw, ch) < 0)
		return;
	qs_texts();
	y = 16;
	/* il testo piu' lungo fra stato e stima */
	bw = bui_text_width(BUI_FONT_SMALL, "100% \xC2\xB7 A batteria", -1);
	if (bw < bui_text_width(BUI_FONT_SMALL, B.qs_bat, -1))
		bw = bui_text_width(BUI_FONT_SMALL, B.qs_bat, -1);
	w = bui_add_label(p, bui_r(20, y, cw - 40 - bw - 8, 32), B.qs_date, BUI_LS_BODY, BUI_AL_LEFT);
	w->font = BUI_FONT_BODY_B;
	B.qs_batl = bui_add_label(p, bui_r(cw - 20 - bw, y, bw, 32), B.qs_bat, BUI_LS_SMALL, BUI_AL_RIGHT);
	y += 32 + 8;
	w = bui_add_label(p, bui_r(18, y, 24, 56), NULL, BUI_LS_SMALL, 0);
	w->icon = BUI_IC(brightness, 24);
	B.qs_bright = bui_add_slider(p, bui_r(46, y, cw - 46 - 10, 56), 5, 100,
				     bs_brightness() < 0 ? 5 : bs_brightness(), qs_on_bright);
	if (bs_brightness() < 0)
		B.qs_bright->flags |= BUI_F_DISABLED;	/* finche' brain-ctl non risponde */
	y += 56;
	w = bui_add_label(p, bui_r(18, y, 24, 56), NULL, BUI_LS_SMALL, 0);
	w->icon = BUI_IC(vol_up, 24);
	B.qs_vol = bui_add_slider(p, bui_r(46, y, cw - 46 - 10, 56), 0, 100,
				  bs_volume() < 0 ? 0 : bs_volume(), qs_on_vol);
	if (bs_volume() < 0)
		B.qs_vol->flags |= BUI_F_DISABLED;
	y += 56 + 12;
	bw = (cw - 32 - 16) / 3;
	w = bui_add_button(p, bui_r(16, y, bw, 84), "Ruota", BUI_IC(rotate, 24), BUI_BS_TONAL, qs_on_rotate);
	w->flags |= BUI_F_ICON_TOP;
	w->font = BUI_FONT_SMALL_B;
	w = bui_add_button(p, bui_r(16 + bw + 8, y, bw, 84), "Schermata", BUI_IC(screenshot, 24),
			   BUI_BS_TONAL, qs_on_shot);
	w->flags |= BUI_F_ICON_TOP;
	w->font = BUI_FONT_SMALL_B;
	w = bui_add_button(p, bui_r(16 + 2 * (bw + 8), y, cw - 32 - 2 * (bw + 8), 84), "Spegni",
			   BUI_IC(power, 24), BUI_BS_TONAL, qs_on_power);
	w->flags |= BUI_F_ICON_TOP;
	w->font = BUI_FONT_SMALL_B;
	w->color = BUI_C_DANGER;
	y += 84 + 8;
	/* sotto i pulsanti, cosi' quelli restano dov'erano */
	w = bui_add_toggle(p, bui_r(4, y, cw - 8, 56), "Risparmio energetico", bs_pref.saver, qs_on_saver);
	w->icon = BUI_IC(battery, 24);

	bui_win_damage(B.win, B.r_status);
	bs_refresh_levels();	/* i livelli possono essere cambiati da fuori */
}

/* ------------------------------------------------------------------ */
/* calendario (tocco sull'orologio) e ultimi toast                    */
/* ------------------------------------------------------------------ */

#define CAL_CELL_W 40
#define CAL_CELL_H 30

static void note_add(const char *text)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	memmove(&B.note[1], &B.note[0], sizeof(B.note[0]) * (BS_NOTES_MAX - 1));
	snprintf(B.note[0].text, sizeof(B.note[0].text), "%s", text);
	snprintf(B.note[0].when, sizeof(B.note[0].when), "%02d:%02d", tm.tm_hour, tm.tm_min);
	if (B.nnotes < BS_NOTES_MAX)
		B.nnotes++;
}

int bs_notes_get(int i, const char **text, const char **when)
{
	if (i < 0 || i >= B.nnotes)
		return -1;
	*text = B.note[i].text;
	*when = B.note[i].when;
	return 0;
}

static void cal_title(void)
{
	static const char *const months[] = { "Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio",
					      "Giugno", "Luglio", "Agosto", "Settembre", "Ottobre",
					      "Novembre", "Dicembre" };

	snprintf(B.cal_title, sizeof(B.cal_title), "%s %d", months[B.cal_m], B.cal_y);
}

static void draw_month(bui_widget *w, bui_canvas *c, bui_rect r)
{
	static const char *const wd[7] = { "L", "M", "M", "G", "V", "S", "D" };
	static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	time_t now = time(NULL);
	struct tm today, first;
	int i, n, col0;

	(void)w;
	localtime_r(&now, &today);
	memset(&first, 0, sizeof(first));
	first.tm_year = B.cal_y - 1900;
	first.tm_mon = B.cal_m;
	first.tm_mday = 1;
	first.tm_hour = 12;
	first.tm_isdst = -1;
	mktime(&first);
	col0 = (first.tm_wday + 6) % 7;		/* la settimana inizia di lunedi' */
	n = mdays[B.cal_m];
	if (B.cal_m == 1 && ((B.cal_y % 4 == 0 && B.cal_y % 100 != 0) || B.cal_y % 400 == 0))
		n = 29;
	for (i = 0; i < 7; i++)
		bui_text_box(c, BUI_FONT_SMALL_B, bui_r(r.x + i * CAL_CELL_W, r.y, CAL_CELL_W, 24), wd[i],
			     i >= 5 ? BUI_C_ACCENT : BUI_C_TEXT3, BUI_AL_CENTER);
	for (i = 0; i < n; i++) {
		int cell = col0 + i;
		bui_rect cr = bui_r(r.x + (cell % 7) * CAL_CELL_W, r.y + 26 + (cell / 7) * CAL_CELL_H,
				    CAL_CELL_W, CAL_CELL_H);
		int is_today = today.tm_year + 1900 == B.cal_y && today.tm_mon == B.cal_m &&
			       today.tm_mday == i + 1;
		char num[16];

		snprintf(num, sizeof(num), "%d", i + 1);
		if (is_today)
			bui_rrect(c, bui_r(cr.x + (CAL_CELL_W - 34) / 2, cr.y + 1, 34, CAL_CELL_H - 2), 14,
				  BUI_C_ACCENT);
		bui_text_box(c, is_today ? BUI_FONT_BODY_B : BUI_FONT_BODY, cr, num,
			     is_today ? BUI_C_ON_ACCENT : cell % 7 >= 5 ? BUI_C_TEXT2 : BUI_C_TEXT,
			     BUI_AL_CENTER);
	}
}

static void draw_notes(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int i;

	(void)w;
	bui_fill(c, bui_r(r.x, r.y, r.w, 1), BUI_C_LINE);
	bui_text_box(c, BUI_FONT_SMALL_B, bui_r(r.x, r.y + 8, r.w, 20), "ULTIMI AVVISI", BUI_C_ACCENT,
		     BUI_AL_LEFT);
	for (i = 0; i < B.nnotes && i < 3; i++) {
		int y = r.y + 30 + i * 22;

		bui_text_box(c, BUI_FONT_SMALL_B, bui_r(r.x, y, 44, 22), B.note[i].when, BUI_C_TEXT2,
			     BUI_AL_LEFT);
		bui_text_box(c, BUI_FONT_SMALL, bui_r(r.x + 48, y, r.w - 48, 22), B.note[i].text, BUI_C_TEXT,
			     BUI_AL_LEFT);
	}
}

static void cal_shift(int dm)
{
	if (dm == 0) {
		time_t t = time(NULL);
		struct tm tm;

		localtime_r(&t, &tm);
		B.cal_y = tm.tm_year + 1900;
		B.cal_m = tm.tm_mon;
	} else {
		B.cal_m += dm;
		if (B.cal_m < 0) {
			B.cal_m = 11;
			B.cal_y--;
		} else if (B.cal_m > 11) {
			B.cal_m = 0;
			B.cal_y++;
		}
	}
	cal_title();
	if (B.qs && B.pop_kind == POP_CAL)
		bui_panel_damage(&B.qsp);
}

static void cal_prev(bui_widget *w) { (void)w; cal_shift(-1); }
static void cal_next(bui_widget *w) { (void)w; cal_shift(1); }
static void cal_today(bui_widget *w) { (void)w; cal_shift(0); }

static void cal_open(void)
{
	int gw = 7 * CAL_CELL_W, cw = gw + 32;
	int nn = B.nnotes > 3 ? 3 : B.nnotes;
	int notes_h = nn ? 34 + nn * 22 : 0;
	int ch = 12 + 48 + 4 + 26 + 6 * CAL_CELL_H + 12 + notes_h + (nn ? 8 : 0);
	bui_panel *p = &B.qsp;
	bui_widget *w;

	if (pop_open(POP_CAL, cw, ch) < 0)
		return;
	cal_shift(0);
	bui_add_button(p, bui_r(8, 12, 48, 48), NULL, BUI_IC(back, 24), BUI_BS_ROUND, cal_prev);
	w = bui_add_button(p, bui_r(60, 12, cw - 120, 48), B.cal_title, NULL, BUI_BS_GHOST, cal_today);
	w->font = BUI_FONT_BODY_B;
	bui_add_button(p, bui_r(cw - 56, 12, 48, 48), NULL, BUI_IC(chevron, 24), BUI_BS_ROUND, cal_next);
	bui_add_custom(p, bui_r(16, 64, gw, 26 + 6 * CAL_CELL_H), draw_month);
	if (nn)
		bui_add_custom(p, bui_r(16, 64 + 26 + 6 * CAL_CELL_H + 10, gw, notes_h), draw_notes);
	bui_win_damage(B.win, clock_zone());
}

/* ------------------------------------------------------------------ */
/* battito della batteria in carica                                   */
/* ------------------------------------------------------------------ */

/* budget: 44x16 px (una fascia di 16 righe) 12 volte in un secondo, una
 * volta per lettura della batteria (ogni 30 s) e solo mentre e' in carica */
static void pulse_tick(void *user)
{
	int t = bui_tween(B.pulse_start, PULSE_MS, bui_now());
	int k = t >= 1024 ? 0 : (t < 512 ? t : 1024 - t) * 200 / 512;

	(void)user;
	if (t >= 1024 || bs_screen_state() != BS_SCREEN_ON) {
		bui_timer_del(B.pulse_timer);
		B.pulse_timer = 0;
		k = 0;
	}
	if (k != B.pulse_k) {
		B.pulse_k = k;
		bui_win_damage(B.win, bui_r(B.r_bat.x + 14, (BS_BAR_H - 16) / 2, 28, 16));
	}
}

static void pulse_start(void)
{
	if (B.pulse_timer || bui_anim_level() != BUI_ANIM_FULL || bs_screen_state() != BS_SCREEN_ON ||
	    bs_battery_get()->status != BS_BAT_CHARGING)
		return;
	B.pulse_start = bui_now();
	B.pulse_timer = bui_timer_add(80, 80, pulse_tick, NULL);
}

/* ------------------------------------------------------------------ */
/* tocco                                                              */
/* ------------------------------------------------------------------ */

static int zone_at(int x, int y, unsigned long *id)
{
	int i;

	(void)y;
	if (x < B.r_home.x + B.r_home.w)
		return Z_HOME;
	if (x >= B.r_clock.x - 6)
		return Z_CLOCK;
	if (x >= B.r_status.x)
		return Z_STATUS;
	for (i = 0; i < B.nchips; i++) {
		bui_rect r = B.chip[i].r;

		if (x >= r.x - 3 && x < r.x + r.w + 3) {
			*id = B.chip[i].id;
			return chip_closable(i) && x >= r.x + r.w - CHIP_CLOSE_W ? Z_CLOSE : Z_CHIP;
		}
	}
	return Z_NONE;
}

static bui_rect zone_rect(int zone, unsigned long id)
{
	int i;

	switch (zone) {
	case Z_HOME:
		return B.r_home;
	case Z_STATUS:
		return B.r_status;
	case Z_CLOCK:
		return clock_zone();
	case Z_CHIP:
	case Z_CLOSE:
		for (i = 0; i < B.nchips; i++)
			if (B.chip[i].id == id)
				return B.chip[i].r;
		break;
	}
	return bui_r(0, 0, 0, 0);
}

static void bar_pointer(bui_win *win, const bui_pointer *ev)
{
	unsigned long id = 0;
	int z, on;

	(void)win;
	switch (ev->type) {
	case BUI_PRESS:
		B.zone = zone_at(ev->x, ev->y, &id);
		B.zone_id = id;
		B.zone_on = B.zone != Z_NONE;
		bui_win_damage(B.win, zone_rect(B.zone, id));
		break;
	case BUI_MOVE:
		if (B.zone == Z_NONE)
			break;
		z = zone_at(ev->x, ev->y, &id);
		on = z == B.zone && id == B.zone_id && ev->y > -24 && ev->y < BS_BAR_H + 24;
		if (on != B.zone_on) {
			B.zone_on = on;
			bui_win_damage(B.win, zone_rect(B.zone, B.zone_id));
		}
		break;
	case BUI_RELEASE:
	case BUI_CANCEL:
		z = B.zone;
		id = B.zone_id;
		on = B.zone_on && ev->type == BUI_RELEASE;
		B.zone = Z_NONE;
		B.zone_on = 0;
		bui_win_damage(B.win, zone_rect(z, id));
		if (!on)
			break;
		switch (z) {
		case Z_HOME:
			bs_home_go_grid();
			bs_wm_show_home();
			break;
		case Z_STATUS:
			/* di norma il pannello aperto ha il grab e si chiude da solo;
			 * se il grab non e' riuscito il tocco arriva qui */
			if (B.qs)
				qs_close();
			else
				qs_open();
			break;
		case Z_CLOCK:
			if (B.qs)
				qs_close();
			else
				cal_open();
			break;
		case Z_CHIP:
			bs_wm_activate(id);
			break;
		case Z_CLOSE:
			bs_wm_close(id);
			break;
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* eventi di sistema                                                  */
/* ------------------------------------------------------------------ */

static void minute_tick(void *user);

static void arm_minute(void)
{
	struct timespec ts;

	/* scatta poco dopo l'inizio del prossimo minuto dell'orologio di sistema */
	clock_gettime(CLOCK_REALTIME, &ts);
	B.minute_timer = bui_timer_add((int)((60 - ts.tv_sec % 60) * 1000 - ts.tv_nsec / 1000000) + 20, 0,
				       minute_tick, NULL);
}

void bs_bar_clock_changed(void)
{
	if (B.minute_timer)
		bui_timer_del(B.minute_timer);
	B.minute_timer = 0;
	if (bs_screen_state() != BS_SCREEN_OFF)
		arm_minute();
	bs_notify(BS_EV_MINUTE);
}

static void minute_tick(void *user)
{
	(void)user;
	B.minute_timer = 0;
	if (bs_screen_state() == BS_SCREEN_OFF)
		return;		/* a schermo spento l'orologio non serve: riparte al risveglio */
	arm_minute();
	bs_notify(BS_EV_MINUTE);
}

/* impronta di cio' che le linguette mostrano: finestre, titoli, quella attiva */
static unsigned chips_signature(void)
{
	const bs_client *cl;
	int n = bs_wm_clients(&cl), i;
	unsigned h = 2166136261u;
	const char *p;

	for (i = 0; i < B.nchips; i++) {
		int j;

		h = (h ^ (unsigned)B.chip[i].id) * 16777619u;
		h = (h ^ (unsigned)B.chip[i].r.w) * 16777619u;
		h = (h ^ (unsigned)(B.chip[i].id == bs_wm_active())) * 16777619u;
		for (j = 0; j < n; j++)
			if (cl[j].id == B.chip[i].id)
				for (p = cl[j].title; *p; p++)
					h = (h ^ (unsigned char)*p) * 16777619u;
	}
	return h ^ (unsigned)B.nchips ^ ((unsigned)B.more << 8);
}

void bs_bar_notify(int ev)
{
	char old[sizeof(B.clock)];
	unsigned sig;

	if (!B.win)
		return;
	switch (ev) {
	case BS_EV_MINUTE:
		memcpy(old, B.clock, sizeof(old));
		update_clock();
		if (strcmp(old, B.clock))
			bui_win_damage(B.win, B.r_clock);
		break;
	case BS_EV_SCREEN:
		if (bs_screen_state() != BS_SCREEN_OFF && !B.minute_timer) {
			arm_minute();
			bs_notify(BS_EV_MINUTE);
		}
		break;
	case BS_EV_THEME:
		qs_close();
		bui_win_damage_all(B.win);
		break;
	case BS_EV_BATTERY:
		bui_win_damage(B.win, B.r_bat);
		pulse_start();
		if (B.qs && B.qs_batl) {
			qs_texts();
			bui_widget_damage(B.qs_batl);
		}
		break;
	case BS_EV_VOLUME:
		bui_win_damage(B.win, B.r_vol);
		if (B.qs_vol && !(B.qs_vol->flags & BUI_F_PRESSED) && bs_volume() >= 0) {
			bui_widget_set_flag(B.qs_vol, BUI_F_DISABLED, 0);
			bui_widget_set_value(B.qs_vol, bs_volume());
		}
		break;
	case BS_EV_BRIGHTNESS:
		if (B.qs_bright && !(B.qs_bright->flags & BUI_F_PRESSED) && bs_brightness() >= 0) {
			bui_widget_set_flag(B.qs_bright, BUI_F_DISABLED, 0);
			bui_widget_set_value(B.qs_bright, bs_brightness());
		}
		break;
	case BS_EV_WINDOWS:
		layout_chips();
		/* un tocco in corso su una linguetta appena sparita non vale piu' */
		if ((B.zone == Z_CHIP || B.zone == Z_CLOSE) && zone_rect(B.zone, B.zone_id).w == 0) {
			B.zone = Z_NONE;
			B.zone_on = 0;
		}
		/* ridisegna solo se cio' che si vede e' cambiato davvero */
		sig = chips_signature();
		if (sig != B.chips_sig) {
			B.chips_sig = sig;
			bui_win_damage(B.win, B.r_chips);
		}
		break;
	case BS_EV_PREFS:
		layout();
		B.chips_sig = chips_signature();
		bui_win_damage_all(B.win);
		break;
	}
}

static void bar_resized(bui_win *win)
{
	bui_win_set_strut_top(win, BS_BAR_H);
	layout();
	B.chips_sig = chips_signature();
}

/* lo schermo ha cambiato dimensione (RandR): la barra segue la larghezza */
void bs_bar_screen_changed(void)
{
	if (!B.win)
		return;
	qs_close();
	B.zone = Z_NONE;
	B.zone_on = 0;
	bui_win_move_resize(B.win, bui_r(0, 0, bui_screen_w(), BS_BAR_H));
}

bui_win *bs_bar_init(void)
{
	B.win = bui_win_create(BUI_WIN_DOCK, bui_r(0, 0, bui_screen_w(), BS_BAR_H), "brainshell-bar");
	if (!B.win)
		return NULL;
	B.win->paint = bar_paint;
	B.win->pointer = bar_pointer;
	B.win->resized = bar_resized;
	bui_win_set_strut_top(B.win, BS_BAR_H);
	bs_wm_own(B.win);
	update_clock();
	layout();
	B.chips_sig = chips_signature();
	bui_toast_set_hook(note_add);
	arm_minute();
	bui_win_show(B.win);
	return B.win;
}
