// SPDX-License-Identifier: MIT
/*
 * view_time.c - Impostazioni -> Data e ora: ora e data a mano, fuso orario.
 *
 * Quattro pagine nella stessa vista (BS_VIEW_NO_HEADER, come view_keys.c):
 * riepilogo, ora, data, fuso. Le modifiche passano da brain-ctl
 * ("time AAAA-MM-GG HH:MM", "timezone Zona"), che usa timedatectl: cosi'
 * si aggiorna anche l'orologio hardware (RTC) e il fuso resta dopo il riavvio.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

enum { PG_MAIN, PG_TIME, PG_DATE, PG_TZ };

/* fusi proposti: nome mostrato, zona tz */
static const char *const zones[][2] = {
	{ "Italia (Roma)", "Europe/Rome" },
	{ "Regno Unito (Londra)", "Europe/London" },
	{ "Portogallo (Lisbona)", "Europe/Lisbon" },
	{ "Spagna (Madrid)", "Europe/Madrid" },
	{ "Francia (Parigi)", "Europe/Paris" },
	{ "Germania (Berlino)", "Europe/Berlin" },
	{ "Svizzera (Zurigo)", "Europe/Zurich" },
	{ "Grecia (Atene)", "Europe/Athens" },
	{ "Romania (Bucarest)", "Europe/Bucharest" },
	{ "Turchia (Istanbul)", "Europe/Istanbul" },
	{ "Russia (Mosca)", "Europe/Moscow" },
	{ "Emirati (Dubai)", "Asia/Dubai" },
	{ "India (Calcutta)", "Asia/Kolkata" },
	{ "Thailandia (Bangkok)", "Asia/Bangkok" },
	{ "Cina (Shanghai)", "Asia/Shanghai" },
	{ "Giappone (Tokyo)", "Asia/Tokyo" },
	{ "Corea (Seul)", "Asia/Seoul" },
	{ "Australia (Sydney)", "Australia/Sydney" },
	{ "Nuova Zelanda (Auckland)", "Pacific/Auckland" },
	{ "Brasile (San Paolo)", "America/Sao_Paulo" },
	{ "Argentina (Buenos Aires)", "America/Argentina/Buenos_Aires" },
	{ "USA (New York)", "America/New_York" },
	{ "USA (Chicago)", "America/Chicago" },
	{ "USA (Denver)", "America/Denver" },
	{ "USA (Los Angeles)", "America/Los_Angeles" },
	{ "Messico (Citt\xC3\xA0 del Messico)", "America/Mexico_City" },
	{ "UTC", "Etc/UTC" },
};
#define NZONES ((int)(sizeof(zones) / sizeof(zones[0])))

static struct {
	bui_panel p;
	bui_rect area;
	int page;
	int busy;		/* una richiesta a brain-ctl in corso */
	char zone[64];		/* zona attuale (es. Europe/Rome) */
	int hh, mm;		/* pagina ora */
	int yy, mo, dd;		/* pagina data */
	char clock[16], date[64], zone_label[80];
	char num[5][8];
} V;

static void build(void);

static void go(int page)
{
	V.page = page;
	build();
	bui_panel_damage(&V.p);
}

static const char *zone_name(const char *zone)
{
	int i;

	for (i = 0; i < NZONES; i++)
		if (!strcmp(zones[i][1], zone))
			return zones[i][0];
	return zone;
}

/* la zona in uso: il collegamento /etc/localtime -> .../zoneinfo/Europe/Rome */
static void read_zone(void)
{
	const char *tz = getenv("TZ");
	char link[256];
	ssize_t n;

	if (tz && *tz && *tz != ':') {
		snprintf(V.zone, sizeof(V.zone), "%s", tz);
		return;
	}
	n = readlink("/etc/localtime", link, sizeof(link) - 1);
	if (n > 0) {
		char *z;

		link[n] = 0;
		z = strstr(link, "zoneinfo/");
		snprintf(V.zone, sizeof(V.zone), "%s", z ? z + 9 : "Etc/UTC");
	} else {
		snprintf(V.zone, sizeof(V.zone), "Etc/UTC");
	}
}

static int month_days(int y, int m)
{
	static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
		return 29;
	return d[m - 1];
}

static void on_back(bui_widget *w)
{
	(void)w;
	if (V.page == PG_MAIN)
		bs_open("settings");
	else
		go(PG_MAIN);
}

static void load_now(void)
{
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	V.hh = tm.tm_hour;
	V.mm = tm.tm_min;
	V.yy = tm.tm_year + 1900;
	V.mo = tm.tm_mon + 1;
	V.dd = tm.tm_mday;
}

static void on_page(bui_widget *w)
{
	load_now();
	go(w->id);
}

/* +/- delle colonne: id = colonna * 2 + (0 meno, 1 piu') */
static void on_step(bui_widget *w)
{
	int col = w->id / 2, d = (w->id & 1) ? 1 : -1;

	if (V.page == PG_TIME) {
		if (col == 0)
			V.hh = (V.hh + d + 24) % 24;
		else
			V.mm = (V.mm + d + 60) % 60;
	} else {
		if (col == 0)
			V.dd += d;
		else if (col == 1)
			V.mo = (V.mo - 1 + d + 12) % 12 + 1;
		else
			V.yy += d;
		if (V.yy < 2024)
			V.yy = 2024;
		if (V.yy > 2099)
			V.yy = 2099;
		if (V.dd < 1)
			V.dd = month_days(V.yy, V.mo);
		if (V.dd > month_days(V.yy, V.mo))
			V.dd = col == 0 ? 1 : month_days(V.yy, V.mo);
	}
	build();
	bui_panel_damage(&V.p);
}

static void set_done(int status, const char *out, void *user)
{
	(void)out;
	(void)user;
	V.busy = 0;
	if (status != 0) {
		bui_toast("Impossibile cambiare l'ora", 2200);
		return;
	}
	bs_bar_clock_changed();
	bui_toast("Data e ora impostate", 1600);
	if (bs_home_view_id() && !strcmp(bs_home_view_id(), "datetime"))
		go(PG_MAIN);
}

static void on_apply(bui_widget *w)
{
	time_t t = time(NULL);
	struct tm tm;
	static char arg[32];

	(void)w;
	if (V.busy)
		return;
	localtime_r(&t, &tm);
	/* la pagina cambia solo la sua parte: l'altra resta quella di adesso */
	if (V.page == PG_TIME)
		snprintf(arg, sizeof(arg), "%04d-%02d-%02d %02d:%02d:00", tm.tm_year + 1900, tm.tm_mon + 1,
			 tm.tm_mday, V.hh, V.mm);
	else
		snprintf(arg, sizeof(arg), "%04d-%02d-%02d %02d:%02d:%02d", V.yy, V.mo, V.dd, tm.tm_hour,
			 tm.tm_min, tm.tm_sec);
	V.busy = 1;
	bs_ctl_arg("time", arg, set_done, NULL);
}

static void tz_done(int status, const char *out, void *user)
{
	(void)out;
	V.busy = 0;
	if (status != 0) {
		bui_toast("Impossibile cambiare il fuso orario", 2200);
		return;
	}
	bs_set_timezone((const char *)user);
	read_zone();
	bui_toast("Fuso orario cambiato", 1600);
	if (bs_home_view_id() && !strcmp(bs_home_view_id(), "datetime"))
		go(PG_MAIN);
}

static void on_zone(bui_widget *w)
{
	if (V.busy)
		return;
	V.busy = 1;
	bs_ctl_arg("timezone", zones[w->id][1], tz_done, (void *)zones[w->id][1]);
}

static int header(bui_panel *p, const char *title)
{
	bui_widget *w = bui_add_button(p, bui_r(0, 0, 64, BUI_HEADER_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND,
				       on_back);

	w->flags |= BUI_F_NOFOCUS;
	bui_add_label(p, bui_r(68, 0, V.area.w - 68 - 16, BUI_HEADER_H), title, BUI_LS_TITLE, BUI_AL_LEFT);
	return BUI_HEADER_H + 8;
}

/* una colonna "+ / valore / -" centrata in x */
static void stepper(bui_panel *p, int x, int y, int col, const char *caption, const char *value, int digits)
{
	bui_widget *w;

	bui_add_label(p, bui_r(x - 60, y, 120, 24), caption, BUI_LS_SECTION, BUI_AL_CENTER);
	w = bui_add_button(p, bui_r(x - 36, y + 28, 72, BUI_TOUCH), NULL, BUI_IC(add, 24), BUI_BS_TONAL, on_step);
	w->id = col * 2 + 1;
	w = bui_add_label(p, bui_r(x - 60, y + 28 + BUI_TOUCH + 4, 120, 56), value, BUI_LS_TITLE, BUI_AL_CENTER);
	w->font = digits ? BUI_FONT_NUM : BUI_FONT_TITLE;
	w = bui_add_button(p, bui_r(x - 36, y + 28 + BUI_TOUCH + 64, 72, BUI_TOUCH), NULL, BUI_IC(remove, 24),
			   BUI_BS_TONAL, on_step);
	w->id = col * 2;
}

static void build(void)
{
	static const char *const months[] = { "gen", "feb", "mar", "apr", "mag", "giu",
					      "lug", "ago", "set", "ott", "nov", "dic" };
	bui_panel *p = &V.p;
	bui_rect a = V.area;
	int y, i, w = a.w - 32, cx = a.w / 2, sw = a.w >= 640 ? 170 : 120;
	bui_widget *wd;
	time_t t = time(NULL);
	struct tm tm;

	bui_panel_clear(p);
	localtime_r(&t, &tm);
	switch (V.page) {
	case PG_MAIN:
		y = header(p, "Data e ora");
		strftime(V.clock, sizeof(V.clock), "%H:%M", &tm);
		wd = bui_add_label(p, bui_r(16, y, w, 64), V.clock, BUI_LS_TITLE, BUI_AL_CENTER);
		wd->font = BUI_FONT_NUM;
		y += 64;
		bs_fmt_date(V.date, sizeof(V.date), 1);
		bui_add_label(p, bui_r(16, y, w, 28), V.date, BUI_LS_BODY, BUI_AL_CENTER);
		y += 44;
		wd = bui_add_row(p, bui_r(16, y, w, 56), BUI_IC(clock, 24), "Ora", V.clock, on_page);
		wd->id = PG_TIME;
		wd->flags |= BUI_F_DIVIDER;
		y += 56;
		wd = bui_add_row(p, bui_r(16, y, w, 56), BUI_IC(timer, 24), "Data", V.date, on_page);
		wd->id = PG_DATE;
		wd->flags |= BUI_F_DIVIDER;
		y += 56;
		snprintf(V.zone_label, sizeof(V.zone_label), "%s", zone_name(V.zone));
		wd = bui_add_row(p, bui_r(16, y, w, 56), BUI_IC(flag, 24), "Fuso orario", V.zone_label, on_page);
		wd->id = PG_TZ;
		y += 64;
		break;
	case PG_TIME:
		y = header(p, "Imposta l'ora");
		snprintf(V.num[0], sizeof(V.num[0]), "%02d", V.hh);
		snprintf(V.num[1], sizeof(V.num[1]), "%02d", V.mm);
		stepper(p, cx - sw / 2 - 20, y, 0, "ORE", V.num[0], 1);
		bui_add_label(p, bui_r(cx - 30, y + 28 + BUI_TOUCH + 4, 20, 56), ":", BUI_LS_TITLE, BUI_AL_CENTER)
			->font = BUI_FONT_NUM;
		stepper(p, cx + sw / 2 - 20, y, 1, "MINUTI", V.num[1], 1);
		y += 28 + BUI_TOUCH * 2 + 72;
		bui_add_button(p, bui_r(cx - 110, y, 220, BUI_TOUCH), "Imposta", BUI_IC(check, 24), BUI_BS_FILLED,
			       on_apply);
		y += BUI_TOUCH + 16;
		break;
	case PG_DATE:
		y = header(p, "Imposta la data");
		snprintf(V.num[2], sizeof(V.num[2]), "%d", V.dd);
		snprintf(V.num[3], sizeof(V.num[3]), "%s", months[V.mo - 1]);
		snprintf(V.num[4], sizeof(V.num[4]), "%d", V.yy);
		stepper(p, cx - sw, y, 0, "GIORNO", V.num[2], 1);
		stepper(p, cx, y, 1, "MESE", V.num[3], 0);
		stepper(p, cx + sw, y, 2, "ANNO", V.num[4], 1);
		y += 28 + BUI_TOUCH * 2 + 72;
		bui_add_button(p, bui_r(cx - 110, y, 220, BUI_TOUCH), "Imposta", BUI_IC(check, 24), BUI_BS_FILLED,
			       on_apply);
		y += BUI_TOUCH + 16;
		break;
	default:
		y = header(p, "Fuso orario");
		for (i = 0; i < NZONES; i++) {
			int wide = a.w >= 640, half = (NZONES + 1) / 2, cw = (a.w - 48) / 2;
			bui_rect r = wide ? bui_r(i < half ? 16 : 32 + cw, y + (i % half) * 56, cw, 56)
					  : bui_r(16, y + i * 56, w, 56);

			wd = bui_add_row(p, r, !strcmp(zones[i][1], V.zone) ? BUI_IC(check, 24) : NULL, zones[i][0],
					 NULL, on_zone);
			wd->id = i;
			wd->flags |= BUI_F_DIVIDER;
			if (!strcmp(zones[i][1], V.zone))
				wd->flags |= BUI_F_SELECTED;
		}
		y += (a.w >= 640 ? (NZONES + 1) / 2 : NZONES) * 56 + 8;
		break;
	}
	bui_panel_set_viewport(p, a, y > a.h ? y : a.h);
}

static void tm_enter(bs_view *v)
{
	bui_panel_init(&V.p, v->win);
	V.page = PG_MAIN;
	V.busy = 0;
	read_zone();
	load_now();
}

static void tm_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&V.p);
}

static void tm_layout(bs_view *v, bui_rect a)
{
	(void)v;
	V.area = a;
	build();
}

static void tm_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&V.p, c, clip);
}

static int tm_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&V.p, ev);
}

static int tm_key(bs_view *v, const bui_key *k)
{
	(void)v;
	if (bui_panel_key(&V.p, k))
		return 1;
	if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE) {
		on_back(NULL);
		return 1;
	}
	return 0;
}

static void tm_notify(bs_view *v, int what)
{
	(void)v;
	/* il riepilogo segue l'orologio; le pagine di modifica no (si sta scegliendo) */
	if (what == BS_EV_MINUTE && V.page == PG_MAIN) {
		build();
		bui_panel_damage(&V.p);
	}
}

bs_view bs_view_time = {
	.title = "Data e ora",
	.flags = BS_VIEW_NO_HEADER,
	.enter = tm_enter,
	.leave = tm_leave,
	.layout = tm_layout,
	.draw = tm_draw,
	.pointer = tm_pointer,
	.key = tm_key,
	.notify = tm_notify,
};
