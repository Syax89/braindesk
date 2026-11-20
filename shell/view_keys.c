// SPDX-License-Identifier: MIT
/*
 * view_keys.c - Impostazioni -> Tasti: l'azione di ogni tasto del
 * dizionario (keys.c) e la "Guida ai tasti".
 *
 * Quattro pagine nella stessa vista (BS_VIEW_NO_HEADER: la freccia torna
 * alla pagina precedente, dalla prima alle Impostazioni): elenco dei tasti,
 * scelta dell'azione, scelta dell'app, guida.
 */
#include <stdio.h>
#include <string.h>

#include "shell.h"

enum { PG_MAIN, PG_ACTION, PG_APP, PG_GUIDE };

static struct {
	bui_panel p;
	bui_rect area;
	int page, key;
	char val[BS_NKEYS][48];
	char title[64];
} V;

static void build(void);

static void go(int page)
{
	V.page = page;
	build();
	bui_panel_damage(&V.p);
}

static void on_back(bui_widget *w)
{
	(void)w;
	switch (V.page) {
	case PG_MAIN:
		bs_open("settings");
		break;
	case PG_APP:
		go(PG_ACTION);
		break;
	default:
		go(PG_MAIN);
		break;
	}
}

static void on_key(bui_widget *w)
{
	V.key = w->id;
	go(PG_ACTION);
}

static void on_guide(bui_widget *w)
{
	(void)w;
	go(PG_GUIDE);
}

static void on_defaults(bui_widget *w)
{
	(void)w;
	bs_keys_defaults();
	bui_toast("Tasti riportati alle scelte iniziali", 1800);
	go(PG_MAIN);
}

static void on_action(bui_widget *w)
{
	if (w->id == BS_KA_APP) {
		go(PG_APP);
		return;
	}
	bs_key_set(V.key, w->id, NULL);
	go(PG_MAIN);
}

static void on_app(bui_widget *w)
{
	bs_key_set(V.key, BS_KA_APP, bs_apps[w->id].id);
	go(PG_MAIN);
}

static int header(bui_panel *p, const char *title)
{
	bui_widget *w = bui_add_button(p, bui_r(0, 0, 64, BUI_HEADER_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND,
				       on_back);

	w->flags |= BUI_F_NOFOCUS;
	bui_add_label(p, bui_r(68, 0, V.area.w - 68 - 16, BUI_HEADER_H), title, BUI_LS_TITLE, BUI_AL_LEFT);
	return BUI_HEADER_H + 8;
}

/* testo della guida: una voce per riga, "titolo\tspiegazione" */
static const char *const guide[] = {
	"Kokugo\tApp recenti: ogni pressione passa alla successiva; Invio, un tocco o il rilascio "
	"dopo averlo tenuto aprono quella scelta, Esc annulla.",
	"Eiwa \xC2\xB7 Waei\tImpostazioni rapide (di nuovo per chiuderle).",
	"Brain English\tMusica: pausa e ripresa del brano caricato; senza brano apre Musica.",
	"My Dictionary\tNote; tenuto premuto mezzo secondo apre una nota nuova.",
	"Rireki\tSchermata (salvata da brain-ctl, con un avviso).",
	"Onsei + Kokugo / Eiwa\tLuminosit\xC3\xA0 \xE2\x88\x92 / +.",
	"Onsei + My Dictionary / Rireki\tVolume \xE2\x88\x92 / +.",
	"Onsei + Brain English\tAudio disattivato / riattivato.",
	"Mentre si scrive\tNel Terminale, nell'editor delle note e nei campi di testo i cinque tasti "
	"restano PagSu, PagGi\xC3\xB9, Tab, Ins e Canc; anche nei dialoghi e nei pannelli. Con Maiusc o "
	"Ctrl restano sempre tasti normali.",
	"Home\tAlla griglia da ovunque (anche dal Terminale; la sessione resta aperta).",
	"Go Back\tIndietro; nel Terminale \xC3\xA8 Esc per il programma.",
	"Page \xE2\x86\x91\tCtrl (Ctrl+C, Ctrl+D... nel Terminale).",
	"Onsei + lettera\tAccentate: a \xC3\xA0, e \xC3\xA8, r \xC3\xA9, i \xC3\xAC, o \xC3\xB2, "
	"u \xC3\xB9, 4 \xE2\x82\xAC.",
};

/* posizione della riga i di un elenco: in orizzontale due colonne da `half` righe */
static bui_rect grid_row(int i, int n, int y0)
{
	int wide = V.area.w >= 640, half = (n + 1) / 2, cw = (V.area.w - 32 - 16) / 2;

	if (!wide)
		return bui_r(16, y0 + i * 56, V.area.w - 32, 56);
	return bui_r(i < half ? 16 : 16 + cw + 16, y0 + (i % half) * 56, cw, 56);
}

static int grid_end(int n, int y0)
{
	return y0 + (V.area.w >= 640 ? (n + 1) / 2 : n) * 56;
}

static void build(void)
{
	bui_panel *p = &V.p;
	bui_rect a = V.area;
	int y, i, w = a.w - 32, n, y0;
	bui_widget *wd;

	bui_panel_clear(p);
	switch (V.page) {
	case PG_MAIN:
		y = header(p, "Tasti");
		bui_add_label(p, bui_r(24, y, w - 16, 28), "TASTI DEL DIZIONARIO", BUI_LS_SECTION, BUI_AL_LEFT);
		y += 32;
		for (i = 0; i < BS_NKEYS; i++) {
			bs_key_describe(i, V.val[i], sizeof(V.val[i]));
			wd = bui_add_row(p, bui_r(16, y, w, 56), NULL, bs_key_name(i), V.val[i], on_key);
			wd->id = i;
			wd->flags |= BUI_F_DIVIDER;
			y += 56;
		}
		y += 8;
		wd = bui_add_row(p, bui_r(16, y, w, 56), BUI_IC(info, 24), "Guida ai tasti", NULL, on_guide);
		y += 64;
		bui_add_button(p, bui_r(16, y, 260, BUI_TOUCH), "Scelte iniziali", BUI_IC(replay, 24), BUI_BS_TONAL,
			       on_defaults);
		y += BUI_TOUCH + 16;
		break;
	case PG_ACTION: {
		const char *app;
		int cur = bs_key_action(V.key, &app);

		snprintf(V.title, sizeof(V.title), "%s", bs_key_name(V.key));
		y0 = header(p, V.title);
		for (i = 0; i < BS_KA_COUNT; i++) {
			const char *label = bs_key_action_label(i);

			if (i == BS_KA_STANDARD) {
				snprintf(V.val[0], sizeof(V.val[0]), "Tasto normale (%s)", bs_key_std_name(V.key));
				label = V.val[0];
			}
			wd = bui_add_row(p, grid_row(i, BS_KA_COUNT, y0), i == cur ? BUI_IC(check, 24) : NULL, label,
					 NULL, on_action);
			wd->id = i;
			wd->flags |= BUI_F_DIVIDER;
			if (i == cur)
				wd->flags |= BUI_F_SELECTED;
		}
		y = grid_end(BS_KA_COUNT, y0) + 8;
		break;
	}
	case PG_APP: {
		const char *app;
		int act = bs_key_action(V.key, &app);

		snprintf(V.title, sizeof(V.title), "%s: apri", bs_key_name(V.key));
		y0 = header(p, V.title);
		for (i = 0, n = 0; i < bs_napps; i++)
			n += !(bs_apps[i].flags & BS_APP_HIDDEN);
		if (n > 40)
			n = 40;
		for (i = 0, y = 0; i < bs_napps && y < n; i++) {
			if (bs_apps[i].flags & BS_APP_HIDDEN)
				continue;
			wd = bui_add_row(p, grid_row(y, n, y0),
					 act == BS_KA_APP && !strcmp(app, bs_apps[i].id) ? BUI_IC(check, 24) : NULL,
					 bs_apps[i].label, NULL, on_app);
			wd->id = i;
			wd->flags |= BUI_F_DIVIDER;
			y++;
		}
		y = grid_end(n, y0) + 8;
		break;
	}
	default:
		y = header(p, "Guida ai tasti");
		for (i = 0; i < (int)(sizeof(guide) / sizeof(guide[0])); i++) {
			const char *tab = strchr(guide[i], '\t');
			static char head[sizeof(guide) / sizeof(guide[0])][48];
			int h;

			/* il tasto in grassetto, sotto la spiegazione */
			snprintf(head[i], sizeof(head[i]), "%.*s", (int)(tab - guide[i]), guide[i]);
			wd = bui_add_label(p, bui_r(24, y, w - 16, 26), head[i], BUI_LS_BODY, BUI_AL_LEFT);
			wd->font = BUI_FONT_BODY_B;
			y += 26;
			h = bui_text_wrap(NULL, BUI_FONT_BODY, bui_r(0, 0, w - 16, 1000), tab + 1, 0, 26, BUI_AL_LEFT);
			bui_add_label(p, bui_r(24, y, w - 16, h), tab + 1, BUI_LS_WRAP, BUI_AL_LEFT);
			y += h + 14;
		}
		break;
	}
	bui_panel_set_viewport(p, a, y > a.h ? y : a.h);
}

static void ky_enter(bs_view *v)
{
	bui_panel_init(&V.p, v->win);
	V.page = PG_MAIN;
}

static void ky_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&V.p);
}

static void ky_layout(bs_view *v, bui_rect a)
{
	(void)v;
	V.area = a;
	build();
}

static void ky_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&V.p, c, clip);
}

static int ky_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&V.p, ev);
}

static int ky_key(bs_view *v, const bui_key *k)
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

bs_view bs_view_keys = {
	.title = "Tasti",
	.flags = BS_VIEW_NO_HEADER,
	.enter = ky_enter,
	.leave = ky_leave,
	.layout = ky_layout,
	.draw = ky_draw,
	.pointer = ky_pointer,
	.key = ky_key,
};
