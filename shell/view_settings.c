// SPDX-License-Identifier: MIT
/*
 * view_settings.c - Impostazioni: luminosita', volume, rotazione,
 * informazioni su batteria e sistema.
 */
#include <stdio.h>
#include <string.h>

#include "batt_est.h"
#include "shell.h"

static struct {
	bui_panel p;
	bui_widget *bright, *bright_val, *vol, *vol_val;
	bui_widget *bat[5], *sys[3], *saver;
	char s_bright[16], s_vol[16], s_est[32];
	char s_cap[16], s_volt[16], s_kernel[72], s_mem[64], s_disk[64];
	char s_theme[48];
} S;

static const int idle_secs[5] = { 0, 30, 60, 120, 300 };
#define IDLE_LABELS "Mai\n30 s\n1 min\n2 min\n5 min"

static const int sleep_secs[5] = { 0, 60, 300, 900, 1800 };
#define SLEEP_LABELS "Mai\n1 min\n5 min\n15 min\n30 min"

static int sleep_index(int s)
{
	int i, best = 0;

	for (i = 0; i < 5; i++)
		if (sleep_secs[i] <= s)
			best = i;
	return best;
}

static int idle_index(int s)
{
	int i, best = 0;

	for (i = 0; i < 5; i++)
		if (idle_secs[i] <= s)
			best = i;
	return best;
}

static void fmt_level(char *buf, size_t len, int v)
{
	if (v < 0)
		snprintf(buf, len, "\xE2\x80\x94");
	else
		snprintf(buf, len, "%d%%", v);
}

static void read_info(void)
{
	const bs_battery *b = bs_battery_get();
	long total = 0, avail = 0;
	long long dt = 0, df = 0;

	if (b->capacity >= 0)
		snprintf(S.s_cap, sizeof(S.s_cap), "%d%%", b->capacity);
	else
		snprintf(S.s_cap, sizeof(S.s_cap), "\xE2\x80\x94");
	{
		int m = bs_power_minutes();

		if (m >= 0)
			bs_est_format(S.s_est, sizeof(S.s_est), m);
		else
			snprintf(S.s_est, sizeof(S.s_est), "%s",
				 b->status == BS_BAT_DISCHARGING ? "Calcolo in corso\xE2\x80\xA6" : "\xE2\x80\x94");
	}
	if (b->voltage_mv >= 0)
		snprintf(S.s_volt, sizeof(S.s_volt), "%d,%02d V", b->voltage_mv / 1000,
			 (b->voltage_mv % 1000) / 10);
	else
		snprintf(S.s_volt, sizeof(S.s_volt), "\xE2\x80\x94");
	bs_sys_kernel(S.s_kernel, sizeof(S.s_kernel));
	if (bs_sys_meminfo(&total, &avail) == 0)
		snprintf(S.s_mem, sizeof(S.s_mem), "%ld / %ld MB", avail / 1024, total / 1024);
	else
		snprintf(S.s_mem, sizeof(S.s_mem), "\xE2\x80\x94");
	if (bs_sys_storage(&dt, &df) == 0) {
		long long t10 = dt * 10 / (1024LL * 1024 * 1024), f10 = df * 10 / (1024LL * 1024 * 1024);

		snprintf(S.s_disk, sizeof(S.s_disk), "%lld,%lld / %lld,%lld GB", f10 / 10,
			 f10 % 10, t10 / 10, t10 % 10);
	} else {
		snprintf(S.s_disk, sizeof(S.s_disk), "\xE2\x80\x94");
	}
}

static const char *usb_text(void)
{
	const bs_battery *b = bs_battery_get();

	return b->usb < 0 ? "\xE2\x80\x94" : b->usb ? "Collegata" : "Non collegata";
}

static void on_bright(bui_widget *w, int final)
{
	bs_set_brightness(w->value, final);
}

static void on_vol(bui_widget *w, int final)
{
	bs_set_volume(w->value, final);
}

static void on_time(bui_widget *w)
{
	(void)w;
	bs_open("datetime");	/* Impostazioni -> Data e ora (view_time.c) */
}

static void on_keys(bui_widget *w)
{
	(void)w;
	bs_open("keys");	/* Impostazioni -> Tasti (view_keys.c) */
}

static void on_rotate(bui_widget *w)
{
	(void)w;
	bs_ask_rotate();
}

static void on_percent(bui_widget *w, int final)
{
	(void)final;
	bs_pref_battery_percent = w->value;
	bs_conf_save();
	bs_notify(BS_EV_PREFS);
}

static void on_dim(bui_widget *w, int final)
{
	(void)final;
	bs_pref.dim_s = idle_secs[w->value];
	bs_idle_configure();
	bs_conf_save();
}

static void on_sleep(bui_widget *w, int final)
{
	(void)final;
	bs_pref.sleep_s = sleep_secs[w->value];
	bs_idle_configure();
	bs_conf_save();
}

static void on_saver(bui_widget *w, int final)
{
	(void)final;
	bs_power_saver_set(w->value);
}

static void on_off(bui_widget *w, int final)
{
	(void)final;
	bs_pref.off_s = idle_secs[w->value];
	bs_idle_configure();
	bs_conf_save();
}

/* il tema cambia a caldo: la shell ridisegna tutto e richiama st_layout() */
static void on_theme(bui_widget *w)
{
	if (w->id == bs_pref.theme)
		return;
	bs_pref.theme = w->id;
	bs_prefs_apply();
	bs_conf_save();
	bs_notify(BS_EV_THEME);
}

static void on_anim(bui_widget *w, int final)
{
	(void)final;
	bs_pref.anim = BUI_ANIM_FULL - w->value;
	bs_prefs_apply();
	bs_conf_save();
}

static void on_tiles(bui_widget *w, int final)
{
	(void)final;
	bs_pref.tile_style = w->value - 1;	/* 0 = quello del tema */
	bs_prefs_apply();
	bs_conf_save();
	bs_notify(BS_EV_THEME);
}

static void restart_ui(void *user)
{
	(void)user;
	bs_restart();
}

/* la lingua: i testi si ricostruiscono tutti, si riavvia l'interfaccia (~0,2 s) */
static void on_lang(bui_widget *w, int final)
{
	if (!final || w->value == bs_pref.lang)
		return;
	bs_pref.lang = w->value;
	bs_conf_save();
	bui_toast("Cambio della lingua\xE2\x80\xA6", 1500);
	bui_timer_add(350, 0, restart_ui, NULL);
}

/* anteprima di un tema: fondo, una scheda, l'accento e una riga di testo */
static void draw_swatch(bui_widget *w, bui_canvas *c, bui_rect r)
{
	const bui_theme *t = bui_theme_get(w->id);
	int sel = w->id == bs_pref.theme;

	bui_rrect(c, r, 12, t->bg);
	bui_rrect(c, bui_r(r.x + 7, r.y + 8, r.w - 14, 20), 6, t->surface2);
	bui_rrect(c, bui_r(r.x + 11, r.y + 12, r.w - 30, 4), 2, t->text);
	bui_rrect(c, bui_r(r.x + 11, r.y + 20, r.w - 38, 4), 2, t->text2);
	bui_rrect(c, bui_r(r.x + 7, r.y + 34, 20, 20), 10, t->accent);
	bui_rrect(c, bui_r(r.x + 31, r.y + 38, r.w - 38, 12), 6, t->surface3);
	if (sel)
		bui_rrect_line(c, r, 12, 3, BUI_C_ACCENT);
	else
		bui_rrect_line(c, r, 12, 1, (w->flags & BUI_F_PRESSED) ? BUI_C_TEXT : BUI_C_LINE);
	if (w->flags & BUI_F_FOCUS)
		bui_rrect_line(c, bui_rect_inset(r, -3, -3), 14, 2, BUI_C_TEXT);
}

/* etichetta + selettore a segmenti dentro una scheda; ritorna la nuova y */
static int segs_block(bui_panel *p, int x, int y, int w, const char *label, const char *opts, int v,
		      void (*cb)(bui_widget *, int))
{
	bui_add_label(p, bui_r(x + 16, y, w - 32, 30), label, BUI_LS_BODY, BUI_AL_LEFT);
	bui_add_segs(p, bui_r(x + 12, y + 30, w - 24, 44), opts, v, cb);
	return y + 84;
}

static int section(bui_panel *p, int x, int y, int w, const char *title)
{
	bui_add_label(p, bui_r(x + 8, y, w - 16, 28), title, BUI_LS_SECTION, BUI_AL_LEFT);
	return y + 32;
}

/* blocco "etichetta + valore" sopra un cursore; ritorna la nuova y */
static int slider_block(bui_panel *p, int x, int y, int w, const char *label, const bui_icon *icon,
			char *valbuf, bui_widget **val, bui_widget **sl, int vmin, int v,
			void (*cb)(bui_widget *, int))
{
	bui_widget *ic = bui_add_label(p, bui_r(x + 16, y + 8, 24, 32), NULL, BUI_LS_SMALL, 0);

	ic->icon = icon;
	bui_add_label(p, bui_r(x + 52, y + 8, w - 52 - 80, 32), label, BUI_LS_BODY, BUI_AL_LEFT);
	*val = bui_add_label(p, bui_r(x + w - 16 - 64, y + 8, 64, 32), valbuf, BUI_LS_BODY, BUI_AL_RIGHT);
	(*val)->color = BUI_C_TEXT2;
	*sl = bui_add_slider(p, bui_r(x + 6, y + 36, w - 12, 56), vmin, 100, v < vmin ? vmin : v, cb);
	if (v < 0)
		(*sl)->flags |= BUI_F_DISABLED;	/* finche' brain-ctl non risponde */
	return y + 96;
}

static void st_layout(bs_view *v, bui_rect a)
{
	bui_panel *p = &S.p;
	const bs_battery *b = bs_battery_get();
	int two = a.w >= 760;
	int pad = two ? 24 : 16, gap = 16;
	int cw = two ? (a.w - 2 * pad - gap) / 2 : a.w - 2 * pad;
	int x = pad, y = 12, y1, i, keep = p->sc.pos;
	bui_widget *card, *w;

	(void)v;
	bui_panel_clear(p);
	fmt_level(S.s_bright, sizeof(S.s_bright), bs_brightness());
	fmt_level(S.s_vol, sizeof(S.s_vol), bs_volume());

	/* --- colonna 1: schermo e audio --- */
	y = section(p, x, y, cw, "SCHERMO");
	card = bui_add_card(p, bui_r(x, y, cw, 0));
	y = slider_block(p, x, y, cw, "Luminosit\xC3\xA0", BUI_IC(brightness, 24), S.s_bright,
			 &S.bright_val, &S.bright, 5, bs_brightness(), on_bright);
	bui_add_row(p, bui_r(x, y, cw - 140, 64), BUI_IC(rotate, 24), "Orientamento", NULL, NULL);
	bui_add_button(p, bui_r(x + cw - 12 - 124, y + 4, 124, 56), "Ruota", NULL, BUI_BS_TONAL, on_rotate);
	y += 64;
	w = bui_add_toggle(p, bui_r(x, y, cw, 60), "Mostra percentuale batteria",
			   bs_pref_battery_percent, on_percent);
	w->flags |= BUI_F_DIVIDER;
	y += 68;
	y = segs_block(p, x, y, cw, "Attenua lo schermo dopo", IDLE_LABELS, idle_index(bs_pref.dim_s),
		       on_dim);
	y = segs_block(p, x, y, cw, "Spegni lo schermo dopo", IDLE_LABELS, idle_index(bs_pref.off_s),
		       on_off);
	y = segs_block(p, x, y, cw, "Sospendi dopo (a batteria)", SLEEP_LABELS,
		       sleep_index(bs_pref.sleep_s), on_sleep);
	S.saver = bui_add_toggle(p, bui_r(x, y, cw, 60), "Risparmio energetico", bs_pref.saver, on_saver);
	y += 64;
	card->r.h = y - card->r.y;
	y += 12;

	y = section(p, x, y, cw, "AUDIO");
	card = bui_add_card(p, bui_r(x, y, cw, 0));
	y = slider_block(p, x, y, cw, "Volume", BUI_IC(vol_up, 24), S.s_vol, &S.vol_val, &S.vol, 0,
			 bs_volume(), on_vol);
	y += 4;
	card->r.h = y - card->r.y;
	y += 12;

	/* tasti del dizionario: una pagina a parte */
	y = section(p, x, y, cw, "DISPOSITIVO");
	card = bui_add_card(p, bui_r(x, y, cw, 0));
	bui_add_row(p, bui_r(x, y, cw, 56), BUI_IC(apps, 24), "Tasti del dizionario", NULL,
		    on_keys);
	y += 56;
	bui_add_row(p, bui_r(x, y, cw, 56), BUI_IC(clock, 24), "Data e ora", NULL,
		    on_time);
	y += 56;
	card->r.h = y - card->r.y;
	y1 = y + 12;

	/* --- colonna 2 (o di seguito): batteria e sistema --- */
	if (two) {
		x = pad + cw + gap;
		y = 12;
	} else {
		y = y1;
	}
	y = section(p, x, y, cw, "ASPETTO");
	card = bui_add_card(p, bui_r(x, y, cw, 0));
	snprintf(S.s_theme, sizeof(S.s_theme), "%s", bui_theme_get(bs_pref.theme)->name);
	bui_add_label(p, bui_r(x + 16, y + 6, cw - 32, 30), "Tema", BUI_LS_BODY, BUI_AL_LEFT);
	w = bui_add_label(p, bui_r(x + cw - 16 - 120, y + 6, 120, 30), S.s_theme, BUI_LS_BODY, BUI_AL_RIGHT);
	w->color = BUI_C_ACCENT;
	{
		int n = bui_theme_count(), sw = (cw - 24 - (n - 1) * 8) / n;

		for (i = 0; i < n; i++) {
			w = bui_add_custom(p, bui_r(x + 12 + i * (sw + 8), y + 38, sw, 62), draw_swatch);
			w->on_click = on_theme;
			w->id = i;
		}
	}
	y += 112;
	y = segs_block(p, x, y, cw, "Animazioni", "Complete\nRidotte\nNessuna",
		       BUI_ANIM_FULL - bs_pref.anim, on_anim);
	y = segs_block(p, x, y, cw, "Riquadri della home", "Tema\nPieni\nVetro\nContorno",
		       bs_pref.tile_style + 1, on_tiles);
	y = segs_block(p, x, y, cw, "Lingua", "Italiano\nEnglish\n\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",
		       bs_pref.lang, on_lang);
	card->r.h = y - card->r.y;
	y += 12;

	y = section(p, x, y, cw, "BATTERIA");
	card = bui_add_card(p, bui_r(x, y, cw, 0));
	S.bat[0] = bui_add_row(p, bui_r(x, y, cw, 48), BUI_IC(battery, 24), "Livello", S.s_cap, NULL);
	S.bat[1] = bui_add_row(p, bui_r(x, y + 48, cw, 48), BUI_IC(bolt, 24), "Stato",
			       bs_battery_status_text(b), NULL);
	S.bat[2] = bui_add_row(p, bui_r(x, y + 96, cw, 48), BUI_IC(info, 24), "Tensione", S.s_volt, NULL);
	S.bat[3] = bui_add_row(p, bui_r(x, y + 144, cw, 48), BUI_IC(usb, 24), "Alimentazione USB",
			       usb_text(), NULL);
	S.bat[4] = bui_add_row(p, bui_r(x, y + 192, cw, 48), BUI_IC(timer, 24), "Autonomia stimata",
			       S.s_est, NULL);
	for (i = 0; i < 4; i++)
		S.bat[i]->flags |= BUI_F_DIVIDER;
	y += 240;
	card->r.h = y - card->r.y;
	y += 12;

	y = section(p, x, y, cw, "SISTEMA");
	card = bui_add_card(p, bui_r(x, y, cw, 0));
	S.sys[0] = bui_add_row(p, bui_r(x, y, cw, 48), BUI_IC(memory, 24), "Memoria libera", S.s_mem, NULL);
	S.sys[1] = bui_add_row(p, bui_r(x, y + 48, cw, 48), BUI_IC(storage, 24), "Spazio libero",
			       S.s_disk, NULL);
	S.sys[2] = bui_add_row(p, bui_r(x, y + 96, cw, 48), BUI_IC(terminal, 24), "Kernel", S.s_kernel,
			       NULL);
	w = bui_add_row(p, bui_r(x, y + 144, cw, 48), BUI_IC(info, 24), "BrainDesk",
			"shell " BS_VERSION, NULL);
	(void)w;
	for (i = 0; i < 3; i++)
		S.sys[i]->flags |= BUI_F_DIVIDER;
	y += 192;
	card->r.h = y - card->r.y;
	y += 12;

	bui_panel_set_viewport(p, a, (y > y1 ? y : y1) + 4);
	p->sc.pos = keep > p->sc.max ? p->sc.max : keep;
}

static void st_enter(bs_view *v)
{
	bui_panel_init(&S.p, v->win);
	read_info();
	bs_refresh_levels();
}

static void st_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&S.p);
}

static void st_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&S.p, c, clip);
}

static int st_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&S.p, ev);
}

static int st_key(bs_view *v, const bui_key *k)
{
	(void)v;
	return bui_panel_key(&S.p, k);
}

static void st_notify(bs_view *v, int what)
{
	const bs_battery *b = bs_battery_get();
	int i;

	(void)v;
	switch (what) {
	case BS_EV_BRIGHTNESS:
		fmt_level(S.s_bright, sizeof(S.s_bright), bs_brightness());
		bui_widget_damage(S.bright_val);
		if (!(S.bright->flags & BUI_F_PRESSED) && bs_brightness() >= 0) {
			bui_widget_set_flag(S.bright, BUI_F_DISABLED, 0);
			bui_widget_set_value(S.bright, bs_brightness());
		}
		break;
	case BS_EV_VOLUME:
		fmt_level(S.s_vol, sizeof(S.s_vol), bs_volume());
		bui_widget_damage(S.vol_val);
		if (!(S.vol->flags & BUI_F_PRESSED) && bs_volume() >= 0) {
			bui_widget_set_flag(S.vol, BUI_F_DISABLED, 0);
			bui_widget_set_value(S.vol, bs_volume());
		}
		break;
	case BS_EV_PREFS:
		if (S.saver && S.saver->value != bs_pref.saver)
			bui_widget_set_value(S.saver, bs_pref.saver);
		break;
	case BS_EV_BATTERY:
		read_info();
		S.bat[1]->text2 = bs_battery_status_text(b);
		S.bat[3]->text2 = usb_text();
		for (i = 0; i < 5; i++)
			bui_widget_damage(S.bat[i]);
		for (i = 0; i < 3; i++)
			bui_widget_damage(S.sys[i]);
		break;
	}
}

bs_view bs_view_settings = {
	.enter = st_enter,
	.leave = st_leave,
	.layout = st_layout,
	.draw = st_draw,
	.pointer = st_pointer,
	.key = st_key,
	.notify = st_notify,
};
