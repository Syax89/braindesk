// SPDX-License-Identifier: MIT
/*
 * view_music.c - Musica: libreria di ~/Musica e pannello "in riproduzione".
 *
 * L'elenco (cartella/nome) arriva da media_scan.c, la riproduzione e' di
 * brainplay (view_music_player.c). La vista non ha un proprio orologio: si
 * aggiorna quando il lettore scrive (posizione due volte al secondo mentre
 * suona, nulla in pausa). Uscendo dalla vista la musica continua e il
 * passaggio al brano successivo resta attivo; rientrando si ritrova lo
 * stato corrente.
 *
 * Tastiera: frecce/Invio come ovunque, Spazio = pausa/riprendi,
 * N / P = brano successivo / precedente.
 */
#include <stdio.h>
#include <string.h>

#include "media.h"

#define ROW_H 64
#define CARD_H 276	/* altezza del contenuto del pannello */

static struct {
	bui_panel p;
	bui_list list;
	int active;		/* la vista e' sullo schermo: i widget esistono */
	int cur;		/* indice in lib del brano caricato (-1 nessuno) */
	bui_widget *w_title, *w_sub, *w_seek, *w_pos, *w_dur, *w_play, *w_vol;
	char s_title[MS_TEXT], s_sub[MS_TEXT + 160], s_pos[16], s_dur[16];
} M = { .cur = -1 };

static void lib_changed(ms_lib *l);

static ms_lib lib = {
	.subdir = "Musica",
	.exts = "mp3 flac ogg opus wav m4a aac",
	.recursive = 1,
	.probe = mp_probe_tags,
	.cmp = ms_cmp_path,
	.changed = lib_changed,
};

/* ------------------------------------------------------------------ */
/* testi                                                              */
/* ------------------------------------------------------------------ */

static void fmt_time(char *buf, int len, int ds)
{
	int s = ds / 10;

	if (s >= 3600)
		snprintf(buf, (size_t)len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, (size_t)len, "%d:%02d", s / 60, s % 60);
}

static void item_texts(const ms_item *it, char *title, int tcap, char *sub, int scap)
{
	char folder[128];

	if (it->s1[0])
		snprintf(title, (size_t)tcap, "%s", it->s1);
	else
		ms_title(it, title, tcap);
	ms_folder(it, folder, sizeof(folder));
	if (it->s2[0] && folder[0])
		snprintf(sub, (size_t)scap, "%s \xC2\xB7 %s", it->s2, folder);
	else
		snprintf(sub, (size_t)scap, "%s", it->s2[0] ? it->s2 : folder[0] ? folder : "Musica");
}

/* ------------------------------------------------------------------ */
/* aggiornamento dei widget                                           */
/* ------------------------------------------------------------------ */

static void refresh_now(void)
{
	const mp_status *st = mp_get();
	char title[MS_TEXT], sub[sizeof(M.s_sub)];

	if (M.cur >= 0) {
		item_texts(&lib.it[M.cur], title, sizeof(title), sub, sizeof(sub));
	} else if (st->path[0]) {
		/* brano caricato ma non (ancora) in elenco */
		const char *sl = strrchr(st->path, '/');

		ms_clean(title, sizeof(title), sl ? sl + 1 : st->path, -1);
		sub[0] = 0;
	} else {
		snprintf(title, sizeof(title), "Nessun brano");
		snprintf(sub, sizeof(sub), "%s", lib.n ? "Tocca un brano per ascoltarlo" : "");
	}
	/* cio' che dice il lettore vale piu' del nome del file */
	if (st->path[0] && st->title[0])
		snprintf(title, sizeof(title), "%s", st->title);
	if (st->path[0] && st->artist[0] && (M.cur < 0 || !lib.it[M.cur].s2[0])) {
		char folder[128] = "";

		if (M.cur >= 0)
			ms_folder(&lib.it[M.cur], folder, sizeof(folder));
		if (folder[0])
			snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", st->artist, folder);
		else
			snprintf(sub, sizeof(sub), "%s", st->artist);
	}
	if (strcmp(title, M.s_title)) {
		snprintf(M.s_title, sizeof(M.s_title), "%s", title);
		if (M.active)
			bui_widget_damage(M.w_title);
	}
	if (strcmp(sub, M.s_sub)) {
		snprintf(M.s_sub, sizeof(M.s_sub), "%s", sub);
		if (M.active)
			bui_widget_damage(M.w_sub);
	}
}

static void refresh_pos(void)
{
	const mp_status *st = mp_get();
	int dur = st->dur_ds / 10, pos = st->pos_ds / 10;
	char buf[16];

	if (!M.active)
		return;
	if (st->dur_ds > 0)
		fmt_time(buf, sizeof(buf), st->dur_ds);
	else
		snprintf(buf, sizeof(buf), "%s", st->path[0] ? "--:--" : "");
	if (strcmp(buf, M.s_dur)) {
		memcpy(M.s_dur, buf, sizeof(buf));
		bui_widget_damage(M.w_dur);
	}
	/* mentre il dito tiene il cursore comanda lui */
	if (M.w_seek->flags & BUI_F_PRESSED)
		return;
	if (st->path[0])
		fmt_time(buf, sizeof(buf), st->pos_ds);
	else
		buf[0] = 0;
	if (strcmp(buf, M.s_pos)) {
		memcpy(M.s_pos, buf, sizeof(buf));
		bui_widget_damage(M.w_pos);
	}
	bui_widget_set_flag(M.w_seek, BUI_F_DISABLED, dur <= 0 || st->state == MP_STOPPED);
	if (M.w_seek->vmax != (dur > 0 ? dur : 1)) {
		M.w_seek->vmax = dur > 0 ? dur : 1;
		bui_widget_damage(M.w_seek);
	}
	bui_widget_set_value(M.w_seek, pos > dur ? dur : pos);
}

static void refresh_state(void)
{
	const bui_icon *ic = mp_get()->state == MP_PLAYING ? &bs_ic_pause_32 : &bs_ic_play_32;

	if (!M.active)
		return;
	if (M.w_play->icon != ic) {
		M.w_play->icon = ic;
		bui_widget_damage(M.w_play);
	}
}

/* ridisegna una sola riga dell'elenco (se e' visibile) */
static void row_damage(int i)
{
	bui_rect r, k;

	if (!M.active || i < 0 || !M.list.w)
		return;
	r = bui_widget_rect(M.list.w);
	if (bui_rect_isect(bui_r(r.x, r.y + i * ROW_H - M.list.sc.pos, r.w, ROW_H), r, &k))
		bs_view_damage(k);
}

static void set_current(int i)
{
	if (i == M.cur)
		return;
	row_damage(M.cur);
	M.cur = i;
	row_damage(i);
}

/* ------------------------------------------------------------------ */
/* comandi                                                            */
/* ------------------------------------------------------------------ */

static void play_index(int i)
{
	if (i < 0 || i >= lib.n)
		return;
	set_current(i);
	mp_play(lib.it[i].path);
	if (M.active)
		bui_list_reveal(&M.list, i);
}

/* la striscia della home mostra cosa suona (anche a vista chiusa) */
static void home_now_playing(void)
{
	const mp_status *st = mp_get();
	char title[MS_TEXT];
	int i = ms_find(&lib, st->path);

	if (st->state == MP_STOPPED || !st->path[0]) {
		bs_home_set_now_playing(NULL, BS_NP_NONE);
		return;
	}
	if (st->title[0]) {
		snprintf(title, sizeof(title), "%s", st->title);
	} else if (i >= 0) {
		ms_title(&lib.it[i], title, sizeof(title));
	} else {
		const char *sl = strrchr(st->path, '/');

		ms_clean(title, sizeof(title), sl ? sl + 1 : st->path, -1);
	}
	bs_home_set_now_playing(title, st->state == MP_PLAYING ? BS_NP_PLAYING : BS_NP_PAUSED);
}

static void player_event(int ev)
{
	const mp_status *st = mp_get();
	int i;

	switch (ev) {
	case MP_EV_ENDED:
		/* avanti da solo, anche quando la vista non e' aperta */
		i = ms_find(&lib, st->path);
		if (i >= 0 && i + 1 < lib.n) {
			play_index(i + 1);
			return;
		}
		break;
	case MP_EV_ERROR:
		bui_toast("Riproduzione non riuscita", 2500);
		break;
	case MP_EV_VOLUME:
		if (M.active && !(M.w_vol->flags & BUI_F_PRESSED))
			bui_widget_set_value(M.w_vol, st->volume);
		return;
	case MP_EV_POS:
		refresh_pos();
		return;
	default:
		break;
	}
	set_current(ms_find(&lib, st->path));
	refresh_now();
	refresh_state();
	refresh_pos();
	home_now_playing();
}

static void on_play(bui_widget *w)
{
	(void)w;
	if (!mp_get()->path[0])
		play_index(0);
	else
		mp_toggle();
}

static void on_prev(bui_widget *w)
{
	const mp_status *st = mp_get();

	(void)w;
	/* come ogni lettore: prima torna all'inizio del brano */
	if (st->state != MP_STOPPED && (st->pos_ds > 30 || M.cur <= 0))
		mp_seek(0);
	else if (M.cur > 0)
		play_index(M.cur - 1);
}

static void on_next(bui_widget *w)
{
	(void)w;
	if (M.cur >= 0 && M.cur + 1 < lib.n)
		play_index(M.cur + 1);
	else if (M.cur < 0)
		play_index(0);
}

static void on_seek(bui_widget *w, int final)
{
	fmt_time(M.s_pos, sizeof(M.s_pos), w->value * 10);
	bui_widget_damage(M.w_pos);
	if (final)
		mp_seek(w->value);
}

static void on_volume(bui_widget *w, int final)
{
	(void)final;
	mp_volume(w->value);
}

static void on_select(bui_list *l, int i)
{
	(void)l;
	play_index(i);
}

/* ------------------------------------------------------------------ */
/* elenco                                                             */
/* ------------------------------------------------------------------ */

static void draw_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	char title[MS_TEXT], sub[MS_TEXT + 160];

	(void)l;
	item_texts(&lib.it[i], title, sizeof(title), sub, sizeof(sub));
	if (i == M.cur)
		flags |= BUI_F_SELECTED;
	bui_list_row(c, r, flags, i == M.cur ? BUI_IC(play, 24) : BUI_IC(music, 24), title, sub, NULL);
}

static const char *empty_text(void)
{
	return lib.loaded ? "Nessun brano nella cartella Musica" : "Ricerca dei brani\xE2\x80\xA6";
}

static void lib_changed(ms_lib *l)
{
	M.cur = ms_find(l, mp_get()->path);
	if (!M.active)
		return;
	M.list.empty_text = empty_text();
	bui_list_set_count(&M.list, l->n);
	refresh_now();
}

/* ------------------------------------------------------------------ */
/* vista                                                              */
/* ------------------------------------------------------------------ */

static void mu_layout(bs_view *v, bui_rect a)
{
	const mp_status *st = mp_get();
	bui_panel *p = &M.p;
	bui_rect card, lr;
	bui_widget *w;
	int wide = a.w >= 700, x, y, cw, bx;

	(void)v;
	bui_panel_clear(p);
	bui_panel_set_viewport(p, a, a.h);
	if (wide) {
		card = bui_r(a.w - 360, 8, 348, a.h - 16);
		lr = bui_r(0, 0, a.w - 364, a.h);
	} else {
		card = bui_r(12, a.h - CARD_H - 20, a.w - 24, CARD_H + 8);
		lr = bui_r(0, 0, a.w, a.h - CARD_H - 24);
	}
	M.list.count = lib.n;
	M.list.row_h = ROW_H;
	M.list.draw_row = draw_row;
	M.list.on_select = on_select;
	M.list.empty_text = empty_text();
	M.list.sel = -1;
	w = bui_add_list(p, lr, &M.list);
	bui_panel_focus(p, w, 0);

	bui_add_card(p, card);
	x = card.x + 20;
	cw = card.w - 40;
	y = card.y + (card.h - CARD_H) / 2;
	M.w_title = bui_add_label(p, bui_r(x, y + 12, cw, 30), M.s_title, BUI_LS_TITLE, BUI_AL_LEFT);
	M.w_sub = bui_add_label(p, bui_r(x, y + 42, cw, 22), M.s_sub, BUI_LS_SMALL, BUI_AL_LEFT);
	M.w_seek = bui_add_slider(p, bui_r(card.x + 4, y + 64, card.w - 8, BUI_TOUCH), 0, 1, 0, on_seek);
	M.w_pos = bui_add_label(p, bui_r(x, y + 112, 96, 20), M.s_pos, BUI_LS_SMALL, BUI_AL_LEFT);
	M.w_dur = bui_add_label(p, bui_r(x + cw - 96, y + 112, 96, 20), M.s_dur, BUI_LS_SMALL, BUI_AL_RIGHT);
	bx = card.x + (card.w - (72 + 16 + 96 + 16 + 72)) / 2;
	bui_add_button(p, bui_r(bx, y + 144, 72, 64), NULL, &bs_ic_prev_32, BUI_BS_TONAL, on_prev);
	M.w_play = bui_add_button(p, bui_r(bx + 88, y + 144, 96, 64), NULL,
				  st->state == MP_PLAYING ? &bs_ic_pause_32 : &bs_ic_play_32, BUI_BS_FILLED,
				  on_play);
	bui_add_button(p, bui_r(bx + 200, y + 144, 72, 64), NULL, &bs_ic_next_32, BUI_BS_TONAL, on_next);
	w = bui_add_label(p, bui_r(x, y + 216, 24, BUI_TOUCH), NULL, BUI_LS_BODY, BUI_AL_CENTER);
	w->icon = BUI_IC(vol_up, 24);
	w->color = BUI_C_TEXT2;
	M.w_vol = bui_add_slider(p, bui_r(x + 24, y + 216, card.x + card.w - 4 - (x + 24), BUI_TOUCH), 0, 100,
				 st->volume, on_volume);
	M.active = 1;
	M.s_pos[0] = M.s_dur[0] = 0;
	refresh_now();
	refresh_pos();
	if (M.cur >= 0)
		bui_list_reveal(&M.list, M.cur);
}

static void mu_enter(bs_view *v)
{
	bui_panel_init(&M.p, v->win);
	mp_set_listener(player_event);
	M.cur = ms_find(&lib, mp_get()->path);
	ms_scan(&lib);
}

static void mu_leave(bs_view *v)
{
	(void)v;
	/* la lettura della cartella, se in corso, finisce da sola: serve
	 * comunque per passare al brano successivo */
	M.active = 0;
	bui_panel_clear(&M.p);
}

static void mu_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&M.p, c, clip);
}

static int mu_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&M.p, ev);
}

static int mu_key(bs_view *v, const bui_key *k)
{
	(void)v;
	switch (k->key) {
	case ' ':
		on_play(NULL);
		return 1;
	case 'n':
	case 'N':
		on_next(NULL);
		return 1;
	case 'p':
	case 'P':
		on_prev(NULL);
		return 1;
	default:
		return bui_panel_key(&M.p, k);
	}
}

bs_view bs_view_music = {
	.enter = mu_enter,
	.leave = mu_leave,
	.layout = mu_layout,
	.draw = mu_draw,
	.pointer = mu_pointer,
	.key = mu_key,
};
