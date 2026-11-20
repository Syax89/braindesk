// SPDX-License-Identifier: MIT
/*
 * power.c - gestione dell'energia: tasto di accensione, menu di
 * spegnimento, sospensione e risveglio, batteria scarica, stima
 * dell'autonomia.
 *
 * Tasto di accensione (evdev "Brain Power Button", KEY_POWER: arriva come
 * BUI_K_POWER / BUI_K_POWER_UP a un filtro d'ingresso, prima delle finestre):
 *
 *   schermo spento o attenuato  -> lo riaccende; pressione e rilascio scartati
 *   breve (< 1 s)               -> a batteria sospende; col cavo USB spegne
 *                                  solo lo schermo (la ricarica e' una macchina
 *                                  a stati del kernel che deve continuare)
 *   lungo (>= 1,5 s, mentre e' ancora premuto) -> menu a tutto schermo:
 *                                  Spegni / Riavvia / Sospendi / Annulla
 *                                  (durante un'app X: chiude l'app)
 *
 * Sospensione: "brain-ctl suspend [N]" in un processo figlio. Il comando
 * ritorna solo a sistema risvegliato (N = secondi per la sveglia RTC: il
 * timer dell'Orologio che scade durante il sonno). Al ritorno: eventi
 * accodati prima del risveglio scartati, animazioni portate a termine,
 * inattivita' azzerata, schermo acceso, batteria, ora e data rilette, tutto
 * ridisegnato. bui_now() conta anche il tempo sospeso (CLOCK_BOOTTIME):
 * timer e scadenze superate durante il sonno scattano subito.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "batt_est.h"
#include "media.h"
#include "shell.h"

#define SHORT_MS 1000
#define LONG_MS 1500

static struct {
	int filter;
	/* tasto */
	int down, swallow, long_fired, long_timer;
	uint32_t t_down;
	/* sospensione */
	int sleeping;
	uint32_t t_sleep;
	/* menu */
	bui_win *menu;
	bui_panel mp;
	bui_rect title_r;
	int menu_outside;
	char menu_sub[64];
	/* batteria */
	bs_est est;
	int poweroff;
} P;

int bs_power_sleeping(void)
{
	return P.sleeping || P.poweroff;	/* anche durante lo spegnimento */
}

int bs_power_menu_open(void)
{
	return P.menu != NULL;
}

/* ------------------------------------------------------------------ */
/* sospensione                                                        */
/* ------------------------------------------------------------------ */

static void resumed(int status)
{
	uint32_t now = bui_now();
	char msg[64];

	/* il tasto che ha svegliato il sistema (e tutto cio' che e' arrivato
	 * mentre dormiva) e' gia' in coda col suo tempo: non deve valere */
	bui_input_ignore_before(now);
	bui_anim_finish_all();
	bui_timer_del(P.long_timer);
	P.long_timer = 0;
	P.down = 0;
	bs_idlev_resumed();
	bs_battery_refresh();
	bs_notify(BS_EV_MINUTE);	/* l'orologio di sistema e' andato avanti */
	bui_redraw_all();		/* il pannello puo' aver perso l'immagine */
	if (getenv("BRAINSHELL_TIMING"))
		fprintf(stderr, "brainshell: risveglio dopo %u ms (brain-ctl suspend: %d)\n",
			(unsigned)(now - P.t_sleep), status);
	if (status != 0 && (int32_t)(now - P.t_sleep) < 2000) {
		snprintf(msg, sizeof(msg), "Sospensione non riuscita");
		bui_toast(msg, 3000);
	}
}

static void suspend_done(int status, const char *out, void *user)
{
	(void)out;
	(void)user;
	P.sleeping = 0;
	resumed(status);
}

static void menu_close(void);

static int suspend_now(void)
{
	char secs[16];
	int ms = bs_clock_timer_ms();

	if (P.sleeping)
		return -1;
	menu_close();
	P.sleeping = 1;
	P.t_sleep = bui_now();
	bs_conf_flush();	/* la batteria puo' finire nel sonno */
	/* un timer dell'Orologio in corso: la sveglia RTC lo fa suonare in tempo */
	if (ms > 0)
		snprintf(secs, sizeof(secs), "%d", (ms + 999) / 1000);
	bs_ctl_arg("suspend", ms > 0 ? secs : NULL, suspend_done, NULL);
	return 0;
}

int bs_power_sleep(int explicit_request)
{
	const bs_battery *b = bs_battery_get();

	if (P.sleeping)
		return -1;
	if (b->usb == 1) {
		/* col cavo niente sospensione profonda: solo lo schermo */
		menu_close();
		if (explicit_request)
			bui_toast("In carica: si spegne solo lo schermo", 2000);
		bs_idlev_screen_off();
		return 0;
	}
	return suspend_now();
}

int bs_power_can_autosleep(void)
{
	const bs_battery *b = bs_battery_get();

	return !P.sleeping && !P.menu && b->usb != 1 && mp_get()->state != MP_PLAYING &&
	       bs_xapp_state() == BS_XAPP_IDLE && !bui_is_suspended() && !bs_term_keep_awake();
}

/* ------------------------------------------------------------------ */
/* menu di spegnimento                                                */
/* ------------------------------------------------------------------ */

static void menu_close(void)
{
	if (!P.menu)
		return;
	bui_panel_clear(&P.mp);
	bui_popup_close(P.menu);
	P.menu = NULL;
}

/* lo spegnimento ordinato di systemd dura ~12 s su questa CPU: chi lo ha
 * chiesto vede l'avviso, poi lo schermo si spegne subito e resta spento
 * (i tocchi non lo riaccendono) mentre il sistema finisce da solo */
static void blank_for_poweroff(void *user)
{
	(void)user;
	bs_idlev_screen_off();
}

static void power_down(void)
{
	P.poweroff = 1;
	bs_idlev_wake();
	bs_conf_flush();
	bs_ctl("poweroff", NULL, NULL);
	bui_timer_add(1500, 0, blank_for_poweroff, NULL);
}

static void act_poweroff(bui_widget *w)
{
	(void)w;
	menu_close();
	bui_toast("Spegnimento in corso\xE2\x80\xA6", 5000);
	power_down();
}

static void act_reboot(bui_widget *w)
{
	(void)w;
	menu_close();
	bs_conf_flush();
	bs_ctl("reboot", NULL, NULL);
	bui_toast("Riavvio in corso\xE2\x80\xA6", 5000);
}

static void act_sleep(bui_widget *w)
{
	(void)w;
	menu_close();
	bs_power_sleep(1);
}

static void act_cancel(bui_widget *w)
{
	(void)w;
	menu_close();
}

static void menu_paint(bui_win *win, bui_canvas *c, bui_rect clip)
{
	bui_popup_backdrop(win, c, clip);
	bui_text_box(c, BUI_FONT_TITLE, P.title_r, "Spegnimento", BUI_C_TEXT, BUI_AL_CENTER);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(P.title_r.x, P.title_r.y + 34, P.title_r.w, 22), P.menu_sub,
		     BUI_C_TEXT2, BUI_AL_CENTER);
	bui_panel_draw(&P.mp, c, clip);
}

static void menu_pointer(bui_win *win, const bui_pointer *ev)
{
	int i, on = 0;

	(void)win;
	for (i = 0; i < P.mp.n; i++)
		if (bui_rect_has(bui_widget_rect(&P.mp.w[i]), ev->x, ev->y))
			on = 1;
	if (ev->type == BUI_PRESS)
		P.menu_outside = !on;
	if (P.menu_outside) {
		/* un tocco sul fondo annulla */
		if (ev->type == BUI_RELEASE && !on)
			menu_close();
		return;
	}
	bui_panel_pointer(&P.mp, ev);
}

static void menu_key(bui_win *win, const bui_key *k)
{
	(void)win;
	if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE) {
		menu_close();
		return;
	}
	bui_panel_key(&P.mp, k);
}

void bs_power_menu(void)
{
	static const struct {
		const char *label;
		int style;
		void (*cb)(bui_widget *);
	} item[4] = {
		{ "Spegni", BUI_BS_DANGER, act_poweroff },
		{ "Riavvia", BUI_BS_TONAL, act_reboot },
		{ "Sospendi", BUI_BS_TONAL, act_sleep },
		{ "Annulla", BUI_BS_TONAL, act_cancel },
	};
	const bui_icon *icon[4] = { BUI_IC(power, 24), BUI_IC(restart, 24), BUI_IC(pause, 24),
				    BUI_IC(close, 24) };
	int sw = bui_screen_w(), sh = bui_screen_h();
	int cols = sw >= 640 ? 4 : 2, rows = 4 / cols, gap = 16;
	int bw = (sw - 48 - (cols - 1) * gap) / cols, bh = 120, i, gx, gy;
	bui_widget *w, *cancel = NULL;

	if (bw > 176)
		bw = 176;
	menu_close();
	P.menu = bui_popup_open(bui_r(0, 0, sw, sh), BUI_POPUP_MODAL | BUI_POPUP_DIM);
	if (!P.menu)
		return;
	P.menu->paint = menu_paint;
	P.menu->pointer = menu_pointer;
	P.menu->key = menu_key;
	P.menu_outside = 0;
	bs_battery_text(P.menu_sub, sizeof(P.menu_sub));
	gx = (sw - cols * bw - (cols - 1) * gap) / 2;
	gy = (sh - rows * bh - (rows - 1) * gap) / 2 + 30;
	P.title_r = bui_r(16, gy - 92, sw - 32, 32);
	bui_panel_init(&P.mp, P.menu);
	bui_panel_set_viewport(&P.mp, bui_r(0, 0, sw, sh), sh);
	for (i = 0; i < 4; i++) {
		int x = gx + (i % cols) * (bw + gap), y = gy + (i / cols) * (bh + gap);

		w = bui_add_button(&P.mp, bui_r(x, y, bw, bh), item[i].label, icon[i], item[i].style, item[i].cb);
		w->flags |= BUI_F_ICON_TOP;
		if (i == 3)
			cancel = w;
	}
	/* Invio senza guardare non spegne nulla */
	bui_panel_focus(&P.mp, cancel, 1);
}

/* ------------------------------------------------------------------ */
/* tasto di accensione                                                */
/* ------------------------------------------------------------------ */

static void long_fire(void *user)
{
	(void)user;
	P.long_timer = 0;
	if (!P.down)
		return;
	P.long_fired = 1;
	if (bs_xapp_state() != BS_XAPP_IDLE) {
		bs_xapp_stop();		/* un'app X ha lo schermo: si torna alla shell */
		return;
	}
	bs_power_menu();
}

/*
 * Tasto Home (KEY_HOMEPAGE): alla griglia da ovunque. Schermo spento o
 * attenuato: lo riaccende soltanto. Un'app X aperta: la chiude e si torna
 * alla shell (come Ctrl+Alt+Esc: i dispositivi restano aperti anche durante
 * X, quindi il tasto arriva qui con suspended = 1).
 */
static void go_home(void)
{
	menu_close();
	bs_switcher_close();
	bui_dialog_close();	/* il dialogo annullato: chi l'ha aperto lo sa */
	bs_bar_close_popups();
	bs_home_go_grid();	/* chiude la vista (leave: le note si salvano), toglie la ricerca */
	bs_wm_show_home();
}

static int filter(int kind, const bui_key *k, int suspended)
{
	int st;

	if (!P.sleeping && !P.poweroff && kind == BUI_IN_KEY && k && k->key == BUI_K_HOMEPAGE) {
		if (bs_xapp_state() != BS_XAPP_IDLE)
			bs_xapp_stop();
		else if (suspended)
			;	/* schermo ceduto senza app (non succede) */
		else if (bs_screen_state() != BS_SCREEN_ON)
			bs_idlev_wake();	/* sveglia soltanto */
		else
			go_home();
		return 1;
	}

	(void)suspended;
	if (P.sleeping || P.poweroff)
		return 1;	/* niente arriva a nessuno mentre il sistema si sospende o si spegne */
	if (kind != BUI_IN_KEY || !k || (k->key != BUI_K_POWER && k->key != BUI_K_POWER_UP))
		return 0;
	if (k->key == BUI_K_POWER) {
		if (P.down)
			return 1;
		st = bs_screen_state();
		P.down = 1;
		P.t_down = bui_now();
		P.long_fired = 0;
		P.swallow = st != BS_SCREEN_ON;
		if (P.swallow)
			bs_idlev_wake();	/* sveglia soltanto */
		bui_timer_del(P.long_timer);
		P.long_timer = bui_timer_add(LONG_MS, 0, long_fire, NULL);
		return 1;
	}
	if (!P.down)
		return 1;
	P.down = 0;
	bui_timer_del(P.long_timer);
	P.long_timer = 0;
	if (P.swallow || P.long_fired)
		return 1;
	if ((int32_t)(bui_now() - P.t_down) < SHORT_MS) {
		if (P.menu)
			menu_close();
		else
			bs_power_sleep(0);
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* batteria                                                           */
/* ------------------------------------------------------------------ */

static int discharging(const bs_battery *b)
{
	return b->status == BS_BAT_DISCHARGING && b->usb != 1;
}

int bs_power_minutes(void)
{
	return discharging(bs_battery_get()) ? bs_est_minutes(&P.est, bui_now()) : -1;
}

void bs_battery_text(char *buf, int len)
{
	const bs_battery *b = bs_battery_get();
	char est[32];
	int m = bs_power_minutes();

	if (b->capacity < 0) {
		snprintf(buf, (size_t)len, "Nessuna batteria");
		return;
	}
	if (m >= 0) {
		bs_est_format(est, sizeof(est), m);
		snprintf(buf, (size_t)len, "%d%% \xC2\xB7 %s", b->capacity, est);
	} else {
		snprintf(buf, (size_t)len, "%d%% \xC2\xB7 %s", b->capacity, bs_battery_status_text(b));
	}
}

void bs_power_battery(const bs_battery *old, const bs_battery *n)
{
	char msg[96];

	if (!discharging(n) || n->capacity < 0) {
		bs_est_reset(&P.est);
		return;
	}
	bs_est_add(&P.est, bui_now(), n->capacity);
	if (P.poweroff)
		return;
	/* a 3% (o se il kernel dice 0) uno spegnimento ordinato: sospendere
	 * vorrebbe dire perdere tutto quando la batteria finisce nel sonno */
	if (n->capacity <= 3) {
		bui_toast("Batteria esaurita: spegnimento\xE2\x80\xA6", 5000);
		power_down();
		return;
	}
	if (old->capacity < 0 || !discharging(old))
		return;		/* solo quando la carica scende sotto la soglia */
	if (n->capacity <= 5 && old->capacity > 5) {
		bui_dialog d = {
			.title = "Batteria quasi scarica",
			.message = msg,
			.buttons = { "OK" },
			.styles = { BUI_BS_FILLED },
			.cancel = 0,
		};

		snprintf(msg, sizeof(msg), "Collega il cavo: carica al %d%%. Al 3%% il dispositivo si "
			 "spegne da solo.", n->capacity);
		bs_idlev_wake();
		bui_dialog_open(&d);
	} else if (n->capacity <= 15 && old->capacity > 15) {
		snprintf(msg, sizeof(msg), "Batteria al %d%%: collega l'alimentazione", n->capacity);
		bui_toast(msg, 5000);
	}
}

void bs_power_saver_set(int on)
{
	on = !!on;
	if (on == bs_pref.saver)
		return;
	bs_pref.saver = on;
	bs_prefs_apply();		/* animazioni */
	bs_idle_configure();		/* 15 s / 30 s / 1 min */
	if (on && bs_brightness() > BS_SAVER_BRIGHTNESS)
		bs_set_brightness(BS_SAVER_BRIGHTNESS, 1);
	bs_conf_save();
	bs_notify(BS_EV_PREFS);
}

void bs_power_init(void)
{
	if (P.filter)
		return;
	P.filter = 1;
	bs_est_reset(&P.est);
	bui_add_input_filter(filter);
}

void bs_power_shutdown(void)
{
	menu_close();
	bui_timer_del(P.long_timer);
	P.long_timer = 0;
}
