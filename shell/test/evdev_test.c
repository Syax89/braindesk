// SPDX-License-Identifier: MIT
/*
 * evdev_test.c - prova della traduzione evdev (bui_evdev.c) con flussi di
 * struct input_event come li manda il kernel: tocco del pannello resistivo
 * (calibrazione di 99-calibrator.conf, le tre rotazioni di brain-rotate,
 * mediana, zona morta, raffiche, SYN_DROPPED) e tastiera (mappa US, Shift,
 * Bloc Maiusc, Ctrl, accentate con Alt, ripetizione).
 *
 *   make tests && build/native/evdev_test     (oppure build/debug/: ASan+UBSan)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bui_evdev.h"

static int fails, checks;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		fails++; \
		printf("FALLITO %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

/* ---- tocco ---- */

struct out {
	int type, x, y;
	uint32_t t;
};
static struct out got[256];
static int ngot;

static void emit(int type, int x, int y, uint32_t t, void *user)
{
	(void)user;
	if (ngot < 256)
		got[ngot++] = (struct out){ type, x, y, t };
}

static uint32_t clock_ms;

static void ev(bui_touch *t, int type, int code, int value)
{
	struct input_event e;

	memset(&e, 0, sizeof(e));
	e.input_event_sec = clock_ms / 1000;
	e.input_event_usec = (clock_ms % 1000) * 1000;
	e.type = (unsigned short)type;
	e.code = (unsigned short)code;
	e.value = value;
	bui_touch_event(t, &e);
}

/* un pacchetto completo come lo manda mxs-lradc-ts: X, Y, pressione, SYN */
static void sample(bui_touch *t, int rx, int ry)
{
	ev(t, EV_ABS, ABS_X, rx);
	ev(t, EV_ABS, ABS_Y, ry);
	ev(t, EV_ABS, ABS_PRESSURE, 200);
	ev(t, EV_SYN, SYN_REPORT, 0);
	clock_ms += 5;
}

static void down(bui_touch *t, int rx, int ry)
{
	ev(t, EV_ABS, ABS_X, rx);
	ev(t, EV_ABS, ABS_Y, ry);
	ev(t, EV_ABS, ABS_PRESSURE, 200);
	ev(t, EV_KEY, BTN_TOUCH, 1);
	ev(t, EV_SYN, SYN_REPORT, 0);
	clock_ms += 5;
}

static void up(bui_touch *t)
{
	ev(t, EV_ABS, ABS_PRESSURE, 0);
	ev(t, EV_KEY, BTN_TOUCH, 0);
	ev(t, EV_SYN, SYN_REPORT, 0);
	clock_ms += 5;
}

static const char *const tn[] = { "PRESS", "MOVE", "RELEASE", "WHEEL", "CANCEL" };

static void test_map(void)
{
	bui_touch_cal c;
	int x, y;

	memset(&c, 0, sizeof(c));
	CHECK(bui_touch_parse_cal("181 3986 3986 255", &c) == 0, "parse");
	CHECK(c.minx == 181 && c.maxx == 3986 && c.miny == 3986 && c.maxy == 255, "valori");
	CHECK(bui_touch_parse_cal("1 2 3", &c) < 0, "parse incompleto rifiutato");

	/* orizzontale: angoli e centro (l'asse y e' invertito nella calibrazione) */
	c.sw = 854;
	c.sh = 480;
	c.rot = 0;
	bui_touch_map(&c, 181, 3986, &x, &y);
	CHECK(x == 0 && y == 0, "alto a sinistra -> %d,%d", x, y);
	bui_touch_map(&c, 3986, 255, &x, &y);
	CHECK(x == 853 && y == 479, "basso a destra -> %d,%d", x, y);
	bui_touch_map(&c, (181 + 3986) / 2, (3986 + 255) / 2, &x, &y);
	CHECK(abs(x - 426) <= 1 && abs(y - 239) <= 1, "centro -> %d,%d", x, y);
	/* fuori dalla calibrazione: bloccato ai bordi */
	bui_touch_map(&c, 0, 4095, &x, &y);
	CHECK(x == 0 && y == 0, "oltre l'angolo -> %d,%d", x, y);
	bui_touch_map(&c, 4095, 0, &x, &y);
	CHECK(x == 853 && y == 479, "oltre l'angolo opposto -> %d,%d", x, y);

	/* verticale (rot 1, matrice "0 -1 1 1 0 0 0 0 1"): schermo 480x854 */
	c.sw = 480;
	c.sh = 854;
	c.rot = 1;
	bui_touch_map(&c, 181, 3986, &x, &y);	/* (nx, ny) = (0, 0) -> (1, 0) */
	CHECK(x == 479 && y == 0, "rot1 angolo grezzo alto-sinistra -> %d,%d", x, y);
	bui_touch_map(&c, 3986, 3986, &x, &y);	/* (1, 0) -> (1, 1) */
	CHECK(x == 479 && y == 853, "rot1 (1,0) -> %d,%d", x, y);
	bui_touch_map(&c, 181, 255, &x, &y);	/* (0, 1) -> (0, 0) */
	CHECK(x == 0 && y == 0, "rot1 (0,1) -> %d,%d", x, y);
	/* un quarto lungo x del pannello = un quarto lungo y dello schermo */
	bui_touch_map(&c, 181 + (3986 - 181) / 4, 3986 - (3986 - 255) / 4, &x, &y);
	CHECK(abs(x - 359) <= 1 && abs(y - 213) <= 1, "rot1 punto interno -> %d,%d", x, y);

	/* verticale capovolto (rot 2, "0 1 0 -1 0 1 0 0 1") */
	c.rot = 2;
	bui_touch_map(&c, 181, 3986, &x, &y);	/* (0, 0) -> (0, 1) */
	CHECK(x == 0 && y == 853, "rot2 (0,0) -> %d,%d", x, y);
	bui_touch_map(&c, 3986, 255, &x, &y);	/* (1, 1) -> (1, 0) */
	CHECK(x == 479 && y == 0, "rot2 (1,1) -> %d,%d", x, y);
	bui_touch_map(&c, 181 + (3986 - 181) / 4, 3986 - (3986 - 255) / 4, &x, &y);
	CHECK(abs(x - 120) <= 1 && abs(y - 640) <= 1, "rot2 punto interno -> %d,%d", x, y);
}

static bui_touch_cal land(void)
{
	bui_touch_cal c;

	memset(&c, 0, sizeof(c));
	bui_touch_parse_cal("181 3986 3986 255", &c);
	c.sw = 854;
	c.sh = 480;
	c.deadzone = 2;
	return c;
}

/* grezzo per un punto dello schermo in orizzontale (inverso della mappa) */
static int raw_x(int x) { return 181 + (3986 - 181) * x / 853; }
static int raw_y(int y) { return 3986 - (3986 - 255) * y / 479; }

static void test_tap(void)
{
	bui_touch t;
	bui_touch_cal c = land();

	ngot = 0;
	clock_ms = 100000;
	bui_touch_init(&t, &c, emit, NULL);
	down(&t, raw_x(400), raw_y(200));
	CHECK(ngot == 1 && got[0].type == BUI_PRESS, "un PRESS subito (%d eventi)", ngot);
	CHECK(abs(got[0].x - 400) <= 1 && abs(got[0].y - 200) <= 1, "PRESS a %d,%d", got[0].x, got[0].y);
	CHECK(got[0].t == 100000, "tempo del PRESS %u", (unsigned)got[0].t);
	/* rumore del resistivo: +-6 grezzi (meno di 2 px) non deve generare MOVE */
	sample(&t, raw_x(400) + 6, raw_y(200) - 5);
	sample(&t, raw_x(400) - 4, raw_y(200) + 6);
	sample(&t, raw_x(400) + 3, raw_y(200));
	bui_touch_flush(&t);
	CHECK(ngot == 1, "rumore filtrato (eventi: %d)", ngot);
	up(&t);
	CHECK(ngot == 2 && got[1].type == BUI_RELEASE, "RELEASE");
	CHECK(got[1].x == got[0].x && got[1].y == got[0].y, "rilascio nel punto del tocco %d,%d",
	      got[1].x, got[1].y);
}

static void test_drag_spike(void)
{
	bui_touch t;
	bui_touch_cal c = land();
	int i, moves = 0, maxjump = 0, px, py;

	ngot = 0;
	clock_ms = 200000;
	bui_touch_init(&t, &c, emit, NULL);
	down(&t, raw_x(100), raw_y(300));
	px = got[0].x;
	py = got[0].y;
	/* trascinamento verso l'alto, un campione ogni 5 ms, con un campione
	 * impazzito a meta' (tipico del resistivo quando la pressione cala) */
	for (i = 1; i <= 40; i++) {
		int y = 300 - i * 5;

		if (i == 20)
			sample(&t, raw_x(700), raw_y(20));	/* lo spike */
		else
			sample(&t, raw_x(100) + (i & 1 ? 5 : -5), raw_y(y));
		bui_touch_flush(&t);	/* una lettura per pacchetto */
	}
	for (i = 1; i < ngot; i++) {
		int j = abs(got[i].x - px) + abs(got[i].y - py);

		if (got[i].type == BUI_MOVE)
			moves++;
		if (j > maxjump)
			maxjump = j;
		px = got[i].x;
		py = got[i].y;
	}
	CHECK(moves >= 35, "movimenti inviati: %d", moves);
	CHECK(maxjump <= 12, "lo spike non sposta il punto (salto massimo %d px)", maxjump);
	for (i = 1; i < ngot; i++)
		CHECK(abs(got[i].x - 100) <= 2, "x stabile durante il trascinamento verticale: %d", got[i].x);
	up(&t);
	CHECK(got[ngot - 1].type == BUI_RELEASE && abs(got[ngot - 1].y - 100) <= 6,
	      "rilascio in fondo al trascinamento (y %d)", got[ngot - 1].y);
}

static void test_burst(void)
{
	bui_touch t;
	bui_touch_cal c = land();
	int i;

	/* una lettura con molti pacchetti (shell occupata): un solo MOVE, l'ultimo */
	ngot = 0;
	clock_ms = 300000;
	bui_touch_init(&t, &c, emit, NULL);
	down(&t, raw_x(400), raw_y(400));
	for (i = 1; i <= 10; i++)
		sample(&t, raw_x(400), raw_y(400 - i * 20));
	CHECK(ngot == 1, "nessun MOVE prima della fine della lettura (%d)", ngot);
	bui_touch_flush(&t);
	CHECK(ngot == 2 && got[1].type == BUI_MOVE, "un solo MOVE per la raffica");
	CHECK(abs(got[1].y - 220) <= 2, "il MOVE e' l'ultimo punto filtrato (y %d)", got[1].y);
	/* il rilascio porta con se' il movimento in attesa */
	sample(&t, raw_x(400), raw_y(150));
	up(&t);
	CHECK(ngot == 4 && got[2].type == BUI_MOVE && got[3].type == BUI_RELEASE,
	      "MOVE in attesa inviato prima del RELEASE (%d eventi)", ngot);
}

static void test_dropped(void)
{
	bui_touch t;
	bui_touch_cal c = land();

	ngot = 0;
	clock_ms = 400000;
	bui_touch_init(&t, &c, emit, NULL);
	/* coda del kernel piena: SYN_DROPPED, poi pezzi di pacchetto da scartare */
	ev(&t, EV_ABS, ABS_X, raw_x(10));
	ev(&t, EV_SYN, SYN_DROPPED, 0);
	ev(&t, EV_KEY, BTN_TOUCH, 1);
	ev(&t, EV_SYN, SYN_REPORT, 0);
	CHECK(ngot == 0, "pacchetto dopo SYN_DROPPED scartato");
	/* il pacchetto seguente e' valido: il tocco parte li' */
	sample(&t, raw_x(500), raw_y(100));
	CHECK(ngot == 1 && got[0].type == BUI_PRESS && abs(got[0].x - 500) <= 1, "ripresa dopo la perdita");
	up(&t);
	/* reset: il tocco in corso viene dimenticato senza eventi */
	down(&t, raw_x(50), raw_y(50));
	bui_touch_reset(&t);
	up(&t);
	CHECK(ngot == 3, "dopo reset nessun RELEASE (%d)", ngot);
	/* tavoletta USB senza BTN_TOUCH ma con BTN_LEFT */
	ev(&t, EV_ABS, ABS_X, raw_x(600));
	ev(&t, EV_ABS, ABS_Y, raw_y(300));
	ev(&t, EV_KEY, BTN_LEFT, 1);
	ev(&t, EV_SYN, SYN_REPORT, 0);
	CHECK(ngot == 4 && got[3].type == BUI_PRESS, "BTN_LEFT vale come tocco");
}

/* ---- tastiera ---- */

static int press(bui_kbd *k, int code, bui_key *out)
{
	int r = bui_kbd_event(k, code, 1, 0, out);

	bui_kbd_event(k, code, 0, 0, NULL);
	return r;
}

static void test_keys(void)
{
	bui_kbd k;
	bui_key o;
	int c, n = 0;

	bui_kbd_reset(&k);
	CHECK(press(&k, KEY_A, &o) && o.key == 'a' && o.mods == 0, "a");
	CHECK(press(&k, KEY_SPACE, &o) && o.key == ' ', "spazio");
	CHECK(press(&k, KEY_ENTER, &o) && o.key == BUI_K_ENTER, "Invio");
	CHECK(press(&k, KEY_KPENTER, &o) && o.key == BUI_K_ENTER, "Invio del tastierino");
	CHECK(press(&k, KEY_ESC, &o) && o.key == BUI_K_ESC, "Esc (Home del Brain)");
	CHECK(press(&k, KEY_PAGEUP, &o) && o.key == BUI_K_PGUP, "PagSu");
	CHECK(press(&k, KEY_BACKSPACE, &o) && o.key == BUI_K_BACKSPACE, "Backspace");
	CHECK(!press(&k, KEY_INSERT, &o), "Ins non ha un equivalente");

	bui_kbd_event(&k, KEY_LEFTSHIFT, 1, 0, &o);
	CHECK(press(&k, KEY_A, &o) && o.key == 'A' && o.mods == BUI_MOD_SHIFT, "Shift+a");
	CHECK(press(&k, KEY_1, &o) && o.key == '!', "Shift+1");
	CHECK(press(&k, KEY_SLASH, &o) && o.key == '?', "Shift+/");
	CHECK(press(&k, KEY_TAB, &o) && o.key == BUI_K_TAB && (o.mods & BUI_MOD_SHIFT), "Shift+Tab");
	bui_kbd_event(&k, KEY_LEFTSHIFT, 0, 0, &o);
	CHECK(press(&k, KEY_A, &o) && o.key == 'a' && !o.mods, "Shift rilasciato");

	press(&k, KEY_CAPSLOCK, &o);
	CHECK(press(&k, KEY_B, &o) && o.key == 'B', "Bloc Maiusc");
	CHECK(press(&k, KEY_2, &o) && o.key == '2', "Bloc Maiusc non tocca le cifre");
	bui_kbd_event(&k, KEY_RIGHTSHIFT, 1, 0, &o);
	CHECK(press(&k, KEY_B, &o) && o.key == 'b', "Bloc Maiusc + Shift");
	bui_kbd_event(&k, KEY_RIGHTSHIFT, 0, 0, &o);
	press(&k, KEY_CAPSLOCK, &o);

	bui_kbd_event(&k, KEY_LEFTCTRL, 1, 0, &o);
	CHECK(!press(&k, KEY_A, &o), "Ctrl+a non produce testo (come XLookupString)");
	CHECK(press(&k, KEY_HOME, &o) && o.key == BUI_K_HOME && o.mods == BUI_MOD_CTRL, "Ctrl+Home");
	bui_kbd_event(&k, KEY_LEFTALT, 1, 0, &o);
	CHECK(press(&k, KEY_ESC, &o) && o.key == BUI_K_ESC &&
	      o.mods == (BUI_MOD_CTRL | BUI_MOD_ALT), "Ctrl+Alt+Esc (ritorno dalla sessione X)");
	bui_kbd_event(&k, KEY_LEFTCTRL, 0, 0, &o);

	/* accentate: Alt + tasto, maiuscola con Shift; niente Alt nei modificatori */
	CHECK(press(&k, KEY_E, &o) && o.key == 0xE8 && o.mods == 0, "Alt+e = e grave");
	CHECK(press(&k, KEY_R, &o) && o.key == 0xE9, "Alt+r = e acuta");
	CHECK(press(&k, KEY_A, &o) && o.key == 0xE0, "Alt+a = a grave");
	CHECK(press(&k, KEY_I, &o) && o.key == 0xEC, "Alt+i");
	CHECK(press(&k, KEY_O, &o) && o.key == 0xF2, "Alt+o");
	CHECK(press(&k, KEY_U, &o) && o.key == 0xF9, "Alt+u");
	CHECK(press(&k, KEY_4, &o) && o.key == 0x20AC, "Alt+4 = euro");
	CHECK(press(&k, KEY_X, &o) && o.key == 'x' && o.mods == BUI_MOD_ALT, "Alt+x: carattere con Alt");
	bui_kbd_event(&k, KEY_LEFTSHIFT, 1, 0, &o);
	CHECK(press(&k, KEY_E, &o) && o.key == 0xC8, "Alt+Shift+e = E grave");
	bui_kbd_event(&k, KEY_LEFTSHIFT, 0, 0, &o);
	bui_kbd_event(&k, KEY_LEFTALT, 0, 0, &o);

	/* ogni carattere stampabile ASCII e le accentate si possono scrivere */
	for (c = 0x20; c < 0x7F; c++) {
		int sh, al, code = bui_kbd_code_for(c, &sh, &al);

		bui_kbd_reset(&k);
		if (al)
			bui_kbd_event(&k, KEY_LEFTALT, 1, 0, &o);
		if (sh)
			bui_kbd_event(&k, KEY_LEFTSHIFT, 1, 0, &o);
		if (code && press(&k, code, &o) && o.key == c)
			n++;
		else
			CHECK(0, "carattere %c (codice %d)", c, code);
	}
	CHECK(n == 0x7F - 0x20, "ASCII completo (%d)", n);
	{
		static const int it[] = { 0xE0, 0xE8, 0xE9, 0xEC, 0xF2, 0xF9, 0xC0, 0xC8, 0xC9, 0xCC, 0xD2, 0xD9, 0x20AC };
		unsigned i;

		for (i = 0; i < sizeof(it) / sizeof(it[0]); i++) {
			int sh, al, code = bui_kbd_code_for(it[i], &sh, &al);

			bui_kbd_reset(&k);
			if (al)
				bui_kbd_event(&k, KEY_LEFTALT, 1, 0, &o);
			if (sh)
				bui_kbd_event(&k, KEY_LEFTSHIFT, 1, 0, &o);
			CHECK(code && press(&k, code, &o) && o.key == it[i], "accentata U+%04X", it[i]);
		}
	}
	CHECK(bui_kbd_code_by_name("Return") == KEY_ENTER && bui_kbd_code_by_name("a") == KEY_A &&
	      bui_kbd_code_by_name("A") == KEY_A && bui_kbd_code_by_name("comma") == KEY_COMMA,
	      "nomi dei tasti");
}

static void test_repeat(void)
{
	bui_kbd k;
	bui_key o;
	int n = 0;
	uint32_t t;

	bui_kbd_reset(&k);
	CHECK(bui_kbd_event(&k, KEY_DOWN, 1, 1000, &o) && o.key == BUI_K_DOWN, "Giu'");
	CHECK(!bui_kbd_event(&k, KEY_DOWN, 2, 1300, &o), "la ripetizione del kernel e' ignorata");
	CHECK(!bui_kbd_repeat(&k, 1499, &o), "niente prima di %d ms", BUI_KBD_DELAY_MS);
	CHECK(bui_kbd_repeat(&k, 1500, &o) && o.key == BUI_K_DOWN, "prima ripetizione a 500 ms");
	CHECK(!bui_kbd_repeat(&k, 1539, &o), "non prima di 40 ms");
	for (t = 1540; t < 2540; t += 10)
		n += bui_kbd_repeat(&k, t, &o);
	CHECK(n == 25, "25 ripetizioni al secondo (%d)", n);
	/* in ritardo di 300 ms: una sola ripetizione, niente raffica */
	CHECK(bui_kbd_repeat(&k, 2900, &o) && !bui_kbd_repeat(&k, 2900, &o), "niente raffiche");
	bui_kbd_event(&k, KEY_DOWN, 0, 3000, &o);
	CHECK(!bui_kbd_repeat(&k, 5000, &o), "rilascio: stop");
	/* un altro tasto ferma la ripetizione del primo */
	bui_kbd_event(&k, KEY_A, 1, 6000, &o);
	bui_kbd_event(&k, KEY_B, 1, 6100, &o);
	bui_kbd_event(&k, KEY_A, 0, 6200, &o);
	CHECK(bui_kbd_repeat(&k, 6600, &o) && o.key == 'b', "ripete l'ultimo tasto premuto");
	bui_kbd_event(&k, KEY_LEFTSHIFT, 1, 7000, &o);
	CHECK(bui_kbd_repeat(&k, 7000, &o) && o.key == 'b', "un modificatore non ferma la ripetizione");
	bui_kbd_event(&k, KEY_B, 0, 7100, &o);
	CHECK(!bui_kbd_repeat(&k, 9000, &o), "fine");
	/* tasto di accensione: pressione e rilascio, mai ripetuto */
	bui_kbd_reset(&k);
	CHECK(bui_kbd_event(&k, KEY_POWER, 1, 10000, &o) && o.key == BUI_K_POWER, "KEY_POWER premuto");
	CHECK(!bui_kbd_repeat(&k, 12000, &o), "KEY_POWER non si ripete");
	CHECK(!bui_kbd_event(&k, KEY_POWER, 2, 10500, &o), "ripetizione del kernel ignorata");
	CHECK(bui_kbd_event(&k, KEY_POWER, 0, 12500, &o) && o.key == BUI_K_POWER_UP, "KEY_POWER rilasciato");
	CHECK(bui_kbd_code_by_name("Power") == KEY_POWER, "nome Power");
	/* tasto Home del Brain (KEY_HOMEPAGE; "Go Back" resta KEY_ESC) */
	bui_kbd_reset(&k);
	CHECK(bui_kbd_event(&k, KEY_HOMEPAGE, 1, 20000, &o) && o.key == BUI_K_HOMEPAGE, "KEY_HOMEPAGE");
	CHECK(!bui_kbd_repeat(&k, 22000, &o), "KEY_HOMEPAGE non si ripete");
	CHECK(!bui_kbd_event(&k, KEY_HOMEPAGE, 0, 22100, &o), "rilascio di Home: niente");
	CHECK(press(&k, KEY_ESC, &o) && o.key == BUI_K_ESC, "Go Back resta Esc");
	CHECK(bui_kbd_code_by_name("XF86HomePage") == KEY_HOMEPAGE, "nome XF86HomePage");
}

int main(void)
{
	test_map();
	test_tap();
	test_drag_spike();
	test_burst();
	test_dropped();
	test_keys();
	test_repeat();
	(void)tn;
	printf("evdev_test: %d controlli, %d falliti\n", checks, fails);
	return fails != 0;
}
