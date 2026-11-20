// SPDX-License-Identifier: MIT
/*
 * keys.c - i tasti del dizionario del PW-SH5 come tasti della shell, e il
 * selettore delle app recenti.
 *
 * Il device tree del Brain li manda come tasti normali:
 *
 *   Kokugo (国語)          KEY_PAGEUP     BUI_K_PGUP
 *   Eiwa/Waei (英和・和英) KEY_PAGEDOWN   BUI_K_PGDN
 *   Brain English          KEY_TAB        BUI_K_TAB
 *   My Dictionary          KEY_INSERT     BUI_K_INSERT
 *   Rireki (履歴)          KEY_DELETE     BUI_K_DELETE
 *
 * Mentre si scrive (terminale, editor delle note, campo di rinomina: la
 * vista lo dice con text_input) e dentro i popup (dialoghi, impostazioni
 * rapide, calendario, menu di spegnimento) restano quei tasti. Altrove fanno
 * l'azione scelta in Impostazioni -> Tasti (bs_pref.keys[], chiavi key_* del
 * file di configurazione). Con Onsei (Alt) fanno sempre luminosita' e volume.
 * Con Ctrl o Maiusc restano tasti normali (Maiusc+Tab, Ctrl+PagSu).
 *
 * Pressione lunga: evdev manda anche il rilascio (BUI_K_RELEASED). My
 * Dictionary tenuto 0,5 s apre una nota nuova; Kokugo tenuto e poi
 * rilasciato apre l'app scelta nel selettore. Sotto X il rilascio non
 * arriva: tutto vale come pressione breve.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media.h"
#include "shell.h"

#define HOLD_MS 500		/* My Dictionary tenuto: nota nuova */
#define PICK_MS 450		/* Kokugo tenuto almeno cosi' e rilasciato: apre la scelta */
#define MAX_RECENT 8

static const int bui_keys[BS_NKEYS] = { BUI_K_PGUP, BUI_K_PGDN, BUI_K_TAB, BUI_K_INSERT, BUI_K_DELETE };
static const char *const key_names[BS_NKEYS] = {
	"Kokugo", "Eiwa \xC2\xB7 Waei", "Brain English", "My Dictionary", "Rireki",
};
static const char *const std_names[BS_NKEYS] = { "PagSu", "PagGi\xC3\xB9", "Tab", "Ins", "Canc" };
static const char *const act_ids[BS_KA_COUNT] = {
	"standard", "none", "switcher", "quick", "screenshot", "saver", "music", "note", "app",
};
static const char *const act_labels[BS_KA_COUNT] = {
	"Tasto normale", "Nessuna azione", "App recenti", "Impostazioni rapide", "Schermata",
	"Risparmio energetico s\xC3\xAC/no", "Musica: riproduci/pausa", "Note (tenuto: nuova nota)",
	"Apri un'app",
};
static const char *const defaults[BS_NKEYS] = { "switcher", "quick", "music", "note", "screenshot" };

static struct {
	uint8_t down[BS_NKEYS];
	uint8_t acted[BS_NKEYS];	/* azione fatta alla pressione (attende il rilascio) */
	uint32_t t_down[BS_NKEYS];
	int note_timer;
	int mute_vol;		/* volume prima del muto (Onsei+Brain English) */

	const bs_app *recent[MAX_RECENT];
	int nrecent;

	/* selettore */
	bui_win *sw;
	const bs_app *item[MAX_RECENT];
	int n, sel, cols;
	bui_rect card, cell[MAX_RECENT];
	char sub[MAX_RECENT][32];
} K;

/* ------------------------------------------------------------------ */
/* preferenze                                                         */
/* ------------------------------------------------------------------ */

const char *bs_key_name(int key) { return key >= 0 && key < BS_NKEYS ? key_names[key] : "?"; }
const char *bs_key_std_name(int key) { return key >= 0 && key < BS_NKEYS ? std_names[key] : "?"; }
const char *bs_key_action_label(int a) { return a >= 0 && a < BS_KA_COUNT ? act_labels[a] : "?"; }

static const bs_app *app_by_id(const char *id)
{
	int i;

	for (i = 0; id && *id && i < bs_napps; i++)
		if (!strcmp(bs_apps[i].id, id))
			return &bs_apps[i];
	return NULL;
}

static int parse(const char *v, const char **app)
{
	int i;

	*app = "";
	if (!strncmp(v, "app:", 4)) {
		*app = v + 4;
		return app_by_id(*app) ? BS_KA_APP : -1;
	}
	for (i = 0; i < BS_KA_COUNT; i++)
		if (i != BS_KA_APP && !strcmp(v, act_ids[i]))
			return i;
	return -1;
}

int bs_key_action(int key, const char **app)
{
	int a;

	*app = "";
	if (key < 0 || key >= BS_NKEYS)
		return BS_KA_STANDARD;
	a = parse(bs_pref.keys[key], app);
	if (a < 0)
		a = parse(defaults[key], app);	/* valore sconosciuto nel file: il default */
	return a;
}

void bs_key_set(int key, int action, const char *app)
{
	if (key < 0 || key >= BS_NKEYS || action < 0 || action >= BS_KA_COUNT)
		return;
	if (action == BS_KA_APP)
		snprintf(bs_pref.keys[key], sizeof(bs_pref.keys[key]), "app:%s", app ? app : "");
	else
		snprintf(bs_pref.keys[key], sizeof(bs_pref.keys[key]), "%s", act_ids[action]);
	bs_conf_save();
}

void bs_keys_defaults(void)
{
	int i;

	for (i = 0; i < BS_NKEYS; i++)
		snprintf(bs_pref.keys[i], sizeof(bs_pref.keys[i]), "%s", defaults[i]);
	bs_conf_save();
}

void bs_key_describe(int key, char *buf, int len)
{
	const char *app;
	int a = bs_key_action(key, &app);
	const bs_app *ap = app_by_id(app);

	if (a == BS_KA_APP && ap)
		snprintf(buf, (size_t)len, T("Apri %s"), T(ap->label));
	else if (a == BS_KA_STANDARD)
		snprintf(buf, (size_t)len, T("Tasto normale (%s)"), std_names[key]);
	else
		snprintf(buf, (size_t)len, "%s", act_labels[a]);
}

/* ------------------------------------------------------------------ */
/* app recenti                                                        */
/* ------------------------------------------------------------------ */

void bs_keys_recent(const bs_app *a)
{
	int i, j;

	if (!a || (a->flags & BS_APP_HIDDEN))
		return;
	for (i = 0; i < K.nrecent && K.recent[i] != a; i++)
		;
	if (i == K.nrecent && K.nrecent < MAX_RECENT)
		K.nrecent++;
	if (i >= K.nrecent)
		i = K.nrecent - 1;
	for (j = i; j > 0; j--)
		K.recent[j] = K.recent[j - 1];
	K.recent[0] = a;
}

/* ------------------------------------------------------------------ */
/* selettore                                                          */
/* ------------------------------------------------------------------ */

#define SW_HINT "Kokugo: la successiva \xC2\xB7 Invio: apri \xC2\xB7 Esc: annulla"
#define ITEM_W 124
#define ITEM_H 120
#define ITEM_GAP 8

int bs_switcher_is_open(void)
{
	return K.sw != NULL;
}

void bs_switcher_close(void)
{
	if (!K.sw)
		return;
	bui_popup_close(K.sw);
	K.sw = NULL;
}

static void sw_paint(bui_win *w, bui_canvas *c, bui_rect clip)
{
	int i;

	(void)clip;
	bui_popup_backdrop(w, c, clip);
	bui_shadow(c, K.card, bui_th.r_card, 14);
	bui_rrect(c, K.card, bui_th.r_card, BUI_C_SURFACE);
	bui_text_box(c, BUI_FONT_TITLE, bui_r(K.card.x + 20, K.card.y + 12, K.card.w - 40, 32), "App recenti",
		     BUI_C_TEXT, BUI_AL_LEFT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(K.card.x + 20, K.card.y + K.card.h - 34, K.card.w - 40, 24), SW_HINT,
		     BUI_C_TEXT2, BUI_AL_LEFT);
	for (i = 0; i < K.n; i++) {
		bui_rect r = K.cell[i];
		int sel = i == K.sel;

		bui_rrect(c, r, bui_th.r_tile, sel ? BUI_C_ACCENT_BG : BUI_C_SURFACE2);
		if (sel)
			bui_rrect_line(c, r, bui_th.r_tile, 3, BUI_C_ACCENT);
		bui_icon_draw(c, K.item[i]->icon, r.x + (r.w - 40) / 2, r.y + 18, sel ? BUI_C_ACCENT : BUI_C_TEXT);
		bui_text_box(c, BUI_FONT_SMALL_B, bui_r(r.x + 6, r.y + 66, r.w - 12, 22), K.item[i]->label,
			     BUI_C_TEXT, BUI_AL_CENTER);
		if (K.sub[i][0])
			bui_text_box(c, BUI_FONT_SMALL, bui_r(r.x + 6, r.y + 88, r.w - 12, 20), K.sub[i], BUI_C_TEXT2,
				     BUI_AL_CENTER);
	}
}

static void sw_select(int i)
{
	if (!K.sw || !K.n)
		return;
	K.sel = (i % K.n + K.n) % K.n;
	bui_win_damage(K.sw, K.card);
}

static void sw_pick(void)
{
	const bs_app *a;

	if (!K.sw)
		return;
	a = K.n ? K.item[K.sel] : NULL;
	bs_switcher_close();
	if (a && strcmp(bs_home_view_id(), a->id))
		bs_open(a->id);
}

static void sw_pointer(bui_win *w, const bui_pointer *ev)
{
	int i;

	(void)w;
	if (ev->type != BUI_RELEASE && ev->type != BUI_PRESS)
		return;
	for (i = 0; i < K.n; i++)
		if (bui_rect_has(K.cell[i], ev->x, ev->y)) {
			if (ev->type == BUI_PRESS) {
				sw_select(i);
			} else if (i == K.sel) {
				sw_pick();
			}
			return;
		}
	if (ev->type == BUI_PRESS && !bui_rect_has(K.card, ev->x, ev->y))
		bs_switcher_close();
}

static void sw_key(bui_win *w, const bui_key *k)
{
	(void)w;
	switch (k->key) {
	case BUI_K_LEFT:
		sw_select(K.sel - 1);
		break;
	case BUI_K_RIGHT:
		sw_select(K.sel + 1);
		break;
	case BUI_K_UP:
		sw_select(K.sel - K.cols);
		break;
	case BUI_K_DOWN:
		sw_select(K.sel + K.cols);
		break;
	case BUI_K_ENTER:
	case ' ':
		sw_pick();
		break;
	case BUI_K_ESC:
	case BUI_K_BACKSPACE:
		bs_switcher_close();
		break;
	default:
		break;
	}
}

static void sw_open(void)
{
	int sw = bui_screen_w(), sh = bui_screen_h(), i, rows, cw, ch, x0, y0;
	const char *cur = bs_home_view_id();

	K.n = 0;
	for (i = 0; i < K.nrecent; i++)
		K.item[K.n++] = K.recent[i];
	if (!K.n) {
		bui_toast("Nessuna app recente", 1800);
		return;
	}
	/* come Alt+Tab: la prima pressione sceglie l'app precedente */
	K.sel = K.n > 1 && !strcmp(cur, K.item[0]->id) ? 1 : 0;
	for (i = 0; i < K.n; i++) {
		const char *s = "";

		if (!strcmp(cur, K.item[i]->id))
			s = "aperta";
		else if (!strcmp(K.item[i]->id, "terminal") && !strcmp(bs_term_state(), "running"))
			s = "sessione attiva";
		else if (!strcmp(K.item[i]->id, "music") && mp_get()->state == MP_PLAYING)
			s = "in riproduzione";
		snprintf(K.sub[i], sizeof(K.sub[i]), "%s", s);
	}
	K.cols = (sw - 2 * 24 - 2 * 20 + ITEM_GAP) / (ITEM_W + ITEM_GAP);
	if (K.cols > K.n)
		K.cols = K.n;
	if (K.cols < 1)
		K.cols = 1;
	rows = (K.n + K.cols - 1) / K.cols;
	cw = K.cols * (ITEM_W + ITEM_GAP) - ITEM_GAP + 40;
	i = bui_text_width(BUI_FONT_SMALL, SW_HINT, -1) + 40;
	if (cw < i)
		cw = i;
	if (cw > sw - 32)
		cw = sw - 32;
	ch = 56 + rows * (ITEM_H + ITEM_GAP) - ITEM_GAP + 44;
	K.sw = bui_popup_open(bui_r(0, 0, sw, sh), BUI_POPUP_MODAL | BUI_POPUP_DIM);
	if (!K.sw)
		return;
	K.sw->paint = sw_paint;
	K.sw->pointer = sw_pointer;
	K.sw->key = sw_key;
	K.card = bui_r((sw - cw) / 2, (sh - ch) / 2, cw, ch);
	x0 = K.card.x + (cw - (K.cols * (ITEM_W + ITEM_GAP) - ITEM_GAP)) / 2;
	y0 = K.card.y + 56;
	for (i = 0; i < K.n; i++)
		K.cell[i] = bui_r(x0 + (i % K.cols) * (ITEM_W + ITEM_GAP), y0 + (i / K.cols) * (ITEM_H + ITEM_GAP),
				  ITEM_W, ITEM_H);
	bui_win_damage_all(K.sw);
}

/* ------------------------------------------------------------------ */
/* azioni                                                             */
/* ------------------------------------------------------------------ */

static void toast_level(const char *what, int v)
{
	char msg[64];

	snprintf(msg, sizeof(msg), "%s %d%%", what, v);
	bui_toast(msg, 1200);
}

/* Onsei + tasto: luminosita' e volume, ovunque (anche nel terminale) */
static void onsei(int key)
{
	int b = bs_brightness(), v = bs_volume(), max = bs_pref.saver ? BS_SAVER_BRIGHTNESS : 100;

	switch (key) {
	case BS_KEY_KOKUGO:
	case BS_KEY_EIWA:
		if (b < 0) {
			bui_toast("Luminosit\xC3\xA0 non disponibile", 1500);
			return;
		}
		b += key == BS_KEY_EIWA ? 10 : -10;
		b = b < 5 ? 5 : b > max ? max : b;
		bs_set_brightness(b, 1);
		toast_level("Luminosit\xC3\xA0", b);
		break;
	case BS_KEY_MYDICT:
	case BS_KEY_HISTORY:
		if (v < 0) {
			bui_toast("Volume non disponibile", 1500);
			return;
		}
		v += key == BS_KEY_HISTORY ? 10 : -10;
		v = v < 0 ? 0 : v > 100 ? 100 : v;
		bs_set_volume(v, 1);
		toast_level("Volume", v);
		break;
	case BS_KEY_ENGLISH:
		if (v < 0) {
			bui_toast("Volume non disponibile", 1500);
			return;
		}
		if (v > 0) {
			K.mute_vol = v;
			bs_set_volume(0, 1);
			bui_toast("Audio disattivato", 1200);
		} else {
			v = K.mute_vol > 0 ? K.mute_vol : 50;
			bs_set_volume(v, 1);
			toast_level("Volume", v);
		}
		break;
	}
}

static void note_fire(void *user)
{
	(void)user;
	K.note_timer = 0;
	if (!K.down[BS_KEY_MYDICT] || !K.acted[BS_KEY_MYDICT])
		return;
	K.acted[BS_KEY_MYDICT] = 0;	/* gia' fatto: il rilascio non apre piu' l'elenco */
	bs_notes_new();
}

static void do_action(int key, int a, const char *app, int has_release)
{
	const mp_status *m;

	switch (a) {
	case BS_KA_SWITCHER:
		sw_open();
		break;
	case BS_KA_QUICK:
		bs_bar_quick_toggle();
		break;
	case BS_KA_SCREENSHOT:
		bs_screenshot();
		break;
	case BS_KA_SAVER:
		bs_power_saver_set(!bs_pref.saver);
		bui_toast(bs_pref.saver ? "Risparmio energetico attivo" : "Risparmio energetico disattivato", 1800);
		break;
	case BS_KA_MUSIC:
		m = mp_get();
		if (!m->path[0]) {
			bs_open("music");
			break;
		}
		bui_toast(m->state == MP_PLAYING ? "Musica in pausa" : "Musica: riproduzione", 1200);
		mp_toggle();
		break;
	case BS_KA_NOTE:
		if (!has_release) {
			bs_open("notes");
			break;
		}
		/* breve: l'elenco al rilascio; tenuto: una nota nuova */
		K.acted[key] = 1;
		bui_timer_del(K.note_timer);
		K.note_timer = bui_timer_add(HOLD_MS, 0, note_fire, NULL);
		break;
	case BS_KA_APP:
		if (bs_open(app) < 0)
			bui_toast("App non trovata", 1500);
		break;
	default:
		break;
	}
}

static void released(int key, const bui_key *k)
{
	const char *app;
	int a = bs_key_action(key, &app);

	(void)k;
	if (key == BS_KEY_MYDICT && K.acted[key] && a == BS_KA_NOTE) {
		bui_timer_del(K.note_timer);
		K.note_timer = 0;
		bs_open("notes");
	} else if (key == BS_KEY_KOKUGO && K.sw && a == BS_KA_SWITCHER &&
		   (int32_t)(bui_now() - K.t_down[key]) >= PICK_MS) {
		sw_pick();	/* tenuto e lasciato: come Alt+Tab */
	}
	K.acted[key] = 0;
}

static int popup_open(void)
{
	return bui_dialog_active() || bs_bar_quick_open() || bs_power_menu_open();
}

static int hook(const bui_key *k)
{
	int up = (k->key & BUI_K_RELEASED) != 0, code = k->key & ~BUI_K_RELEASED;
	int key = -1, i, a, x11 = bui_backend_is_x11(), repeat;
	const char *app;

	for (i = 0; i < BS_NKEYS; i++)
		if (bui_keys[i] == code)
			key = i;
	if (key < 0)
		return 0;
	if (up) {
		if (K.down[key])
			released(key, k);
		K.down[key] = 0;
		return 1;
	}
	/* sotto X non arriva il rilascio: ogni pressione vale da sola. Un
	 * rilascio perso (scartato a schermo spento) non blocca il tasto: dopo
	 * 20 s una pressione e' di nuovo una pressione */
	repeat = !x11 && K.down[key] && (int32_t)(bui_now() - K.t_down[key]) < 20000;
	if (!repeat) {
		K.t_down[key] = bui_now();
		K.acted[key] = 0;
	}
	K.down[key] = !x11;
	if ((k->mods & BUI_MOD_ALT) && !(k->mods & BUI_MOD_CTRL)) {
		onsei(key);
		return 1;
	}
	if (k->mods & (BUI_MOD_CTRL | BUI_MOD_SHIFT))
		return 0;
	a = bs_key_action(key, &app);
	if (K.sw) {
		if (key == BS_KEY_KOKUGO && a == BS_KA_SWITCHER) {
			if (!repeat)
				sw_select(K.sel + 1);
			return 1;
		}
		bs_switcher_close();
	}
	if (popup_open()) {
		/* nei popup i tasti restano quelli; il tasto delle impostazioni rapide le chiude */
		if (a == BS_KA_QUICK && bs_bar_quick_open() && !bui_dialog_active()) {
			if (!repeat)
				bs_bar_quick_toggle();
			return 1;
		}
		return 0;
	}
	if (a == BS_KA_STANDARD || bs_home_text_input())
		return 0;
	if (!repeat)
		do_action(key, a, app, !x11);
	return 1;
}

void bs_keys_init(void)
{
	bui_set_key_hook(hook);
}
