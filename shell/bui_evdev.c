// SPDX-License-Identifier: MIT
/*
 * bui_evdev.c - traduzione degli eventi evdev: tocco e tastiera.
 * Solo interi, nessun accesso ai dispositivi (vedi bui_evdev.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bui_evdev.h"

/* ------------------------------------------------------------------ */
/* tocco                                                              */
/* ------------------------------------------------------------------ */

#define ONE 65536	/* 1.0 in virgola fissa 16.16 */

static int norm(int v, int lo, int hi)
{
	long long n;

	if (hi == lo)
		return 0;
	n = (long long)(v - lo) * ONE / (hi - lo);
	return n < 0 ? 0 : n > ONE ? ONE : (int)n;
}

void bui_touch_map(const bui_touch_cal *c, int rx, int ry, int *sx, int *sy)
{
	int nx = norm(rx, c->minx, c->maxx), ny = norm(ry, c->miny, c->maxy);
	int ux, uy;

	/* le stesse matrici di os/brain-rotate (TransformationMatrix di X) */
	switch (c->rot) {
	case 1:			/* 0 -1 1 / 1 0 0 */
		ux = ONE - ny;
		uy = nx;
		break;
	case 2:			/* 0 1 0 / -1 0 1 */
		ux = ny;
		uy = ONE - nx;
		break;
	default:
		ux = nx;
		uy = ny;
		break;
	}
	*sx = (int)(((long long)ux * (c->sw - 1) + ONE / 2) >> 16);
	*sy = (int)(((long long)uy * (c->sh - 1) + ONE / 2) >> 16);
}

int bui_touch_parse_cal(const char *s, bui_touch_cal *c)
{
	int a, b, d, e;

	if (!s || sscanf(s, "%d %d %d %d", &a, &b, &d, &e) != 4 || a == b || d == e)
		return -1;
	c->minx = a;
	c->maxx = b;
	c->miny = d;
	c->maxy = e;
	return 0;
}

void bui_touch_init(bui_touch *t, const bui_touch_cal *cal,
		    void (*emit)(int, int, int, uint32_t, void *), void *user)
{
	memset(t, 0, sizeof(*t));
	t->cal = *cal;
	t->emit = emit;
	t->user = user;
}

void bui_touch_reset(bui_touch *t)
{
	t->was = 0;
	t->btn = 0;
	t->hn = 0;
	t->move_pending = 0;
}

static int med3(const int *v, int n)
{
	int a, b, c;

	if (n == 1)
		return v[0];
	if (n == 2)
		return (v[0] + v[1]) / 2;
	a = v[0];
	b = v[1];
	c = v[2];
	if (a > b) {
		int s = a;

		a = b;
		b = s;
	}
	/* a <= b */
	return c < a ? a : c > b ? b : c;
}

static void push(bui_touch *t)
{
	if (t->hn < 3) {
		t->hx[t->hn] = t->rx;
		t->hy[t->hn] = t->ry;
		t->hn++;
		return;
	}
	t->hx[0] = t->hx[1];
	t->hx[1] = t->hx[2];
	t->hx[2] = t->rx;
	t->hy[0] = t->hy[1];
	t->hy[1] = t->hy[2];
	t->hy[2] = t->ry;
}

void bui_touch_flush(bui_touch *t)
{
	if (!t->move_pending)
		return;
	t->move_pending = 0;
	t->sx = t->px;
	t->sy = t->py;
	t->emit(BUI_MOVE, t->sx, t->sy, t->t, t->user);
}

static void frame(bui_touch *t)
{
	int fx, fy, dz = t->cal.deadzone;

	if (t->btn && !t->was) {
		if (!t->have_xy)
			return;		/* aspetta le coordinate */
		t->hn = 0;
		push(t);
		bui_touch_map(&t->cal, t->rx, t->ry, &t->sx, &t->sy);
		t->was = 1;
		t->move_pending = 0;
		t->emit(BUI_PRESS, t->sx, t->sy, t->t, t->user);
	} else if (t->btn && t->was) {
		/* mediana degli ultimi tre campioni: un campione impazzito del
		 * resistivo non sposta il punto */
		push(t);
		bui_touch_map(&t->cal, med3(t->hx, t->hn), med3(t->hy, t->hn), &fx, &fy);
		if (abs(fx - t->sx) >= dz || abs(fy - t->sy) >= dz || (dz <= 0 && (fx != t->sx || fy != t->sy))) {
			t->px = fx;
			t->py = fy;
			t->move_pending = 1;
		} else {
			t->move_pending = 0;	/* tornato dentro la zona morta */
		}
	} else if (!t->btn && t->was) {
		bui_touch_flush(t);
		t->was = 0;
		t->emit(BUI_RELEASE, t->sx, t->sy, t->t, t->user);
	}
}

void bui_touch_event(bui_touch *t, const struct input_event *ev)
{
	switch (ev->type) {
	case EV_ABS:
		if (ev->code == ABS_X) {
			t->rx = ev->value;
			t->have_xy |= 1;
		} else if (ev->code == ABS_Y) {
			t->ry = ev->value;
			t->have_xy |= 2;
		}
		break;
	case EV_KEY:
		if (ev->code == BTN_TOUCH || ev->code == BTN_LEFT)
			t->btn = ev->value != 0;
		break;
	case EV_SYN:
		if (ev->code == SYN_DROPPED) {
			t->dropped = 1;
			break;
		}
		if (ev->code != SYN_REPORT)
			break;
		t->t = (uint32_t)ev->input_event_sec * 1000u + (uint32_t)(ev->input_event_usec / 1000);
		if (t->dropped) {
			/* eventi persi: si riparte dal prossimo pacchetto completo */
			t->dropped = 0;
			break;
		}
		if (t->have_xy != 3 && t->btn && !t->was)
			break;
		frame(t);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* tastiera                                                           */
/* ------------------------------------------------------------------ */

/* mappa US: carattere senza e con Shift */
static const struct {
	uint16_t code;
	char lo, hi;
} us[] = {
	{ KEY_1, '1', '!' }, { KEY_2, '2', '@' }, { KEY_3, '3', '#' }, { KEY_4, '4', '$' },
	{ KEY_5, '5', '%' }, { KEY_6, '6', '^' }, { KEY_7, '7', '&' }, { KEY_8, '8', '*' },
	{ KEY_9, '9', '(' }, { KEY_0, '0', ')' }, { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' },
	{ KEY_Q, 'q', 'Q' }, { KEY_W, 'w', 'W' }, { KEY_E, 'e', 'E' }, { KEY_R, 'r', 'R' },
	{ KEY_T, 't', 'T' }, { KEY_Y, 'y', 'Y' }, { KEY_U, 'u', 'U' }, { KEY_I, 'i', 'I' },
	{ KEY_O, 'o', 'O' }, { KEY_P, 'p', 'P' }, { KEY_LEFTBRACE, '[', '{' }, { KEY_RIGHTBRACE, ']', '}' },
	{ KEY_A, 'a', 'A' }, { KEY_S, 's', 'S' }, { KEY_D, 'd', 'D' }, { KEY_F, 'f', 'F' },
	{ KEY_G, 'g', 'G' }, { KEY_H, 'h', 'H' }, { KEY_J, 'j', 'J' }, { KEY_K, 'k', 'K' },
	{ KEY_L, 'l', 'L' }, { KEY_SEMICOLON, ';', ':' }, { KEY_APOSTROPHE, '\'', '"' },
	{ KEY_GRAVE, '`', '~' }, { KEY_BACKSLASH, '\\', '|' },
	{ KEY_Z, 'z', 'Z' }, { KEY_X, 'x', 'X' }, { KEY_C, 'c', 'C' }, { KEY_V, 'v', 'V' },
	{ KEY_B, 'b', 'B' }, { KEY_N, 'n', 'N' }, { KEY_M, 'm', 'M' }, { KEY_COMMA, ',', '<' },
	{ KEY_DOT, '.', '>' }, { KEY_SLASH, '/', '?' }, { KEY_SPACE, ' ', ' ' },
	{ KEY_KP0, '0', '0' }, { KEY_KP1, '1', '1' }, { KEY_KP2, '2', '2' }, { KEY_KP3, '3', '3' },
	{ KEY_KP4, '4', '4' }, { KEY_KP5, '5', '5' }, { KEY_KP6, '6', '6' }, { KEY_KP7, '7', '7' },
	{ KEY_KP8, '8', '8' }, { KEY_KP9, '9', '9' }, { KEY_KPDOT, '.', '.' }, { KEY_KPPLUS, '+', '+' },
	{ KEY_KPMINUS, '-', '-' }, { KEY_KPASTERISK, '*', '*' }, { KEY_KPSLASH, '/', '/' },
};

/*
 * Accentate senza tasti morti, AltGr o composizione: Alt + un tasto (la
 * tabella e' nel README). Con Shift la maiuscola.
 */
static const struct {
	uint16_t code;
	uint16_t lo, hi;
} alt_map[] = {
	{ KEY_A, 0xE0, 0xC0 },	/* a grave */
	{ KEY_E, 0xE8, 0xC8 },	/* e grave */
	{ KEY_R, 0xE9, 0xC9 },	/* e acuta (il tasto accanto alla e) */
	{ KEY_I, 0xEC, 0xCC },	/* i grave */
	{ KEY_O, 0xF2, 0xD2 },	/* o grave */
	{ KEY_U, 0xF9, 0xD9 },	/* u grave */
	{ KEY_C, 0xE7, 0xC7 },	/* c cediglia */
	{ KEY_4, 0x20AC, 0x20AC },	/* euro */
	{ KEY_0, 0xB0, 0xB0 },	/* gradi */
	{ KEY_MINUS, 0x2014, 0x2014 },	/* lineetta */
	{ KEY_COMMA, 0xAB, 0xAB },	/* << */
	{ KEY_DOT, 0xBB, 0xBB },	/* >> */
};

static const struct {
	uint16_t code;
	int key;
} special[] = {
	{ KEY_UP, BUI_K_UP }, { KEY_DOWN, BUI_K_DOWN }, { KEY_LEFT, BUI_K_LEFT },
	{ KEY_RIGHT, BUI_K_RIGHT }, { KEY_ENTER, BUI_K_ENTER }, { KEY_KPENTER, BUI_K_ENTER },
	{ KEY_ESC, BUI_K_ESC }, { KEY_BACKSPACE, BUI_K_BACKSPACE }, { KEY_TAB, BUI_K_TAB },
	{ KEY_DELETE, BUI_K_DELETE }, { KEY_PAGEUP, BUI_K_PGUP }, { KEY_PAGEDOWN, BUI_K_PGDN },
	{ KEY_HOME, BUI_K_HOME }, { KEY_END, BUI_K_END }, { KEY_HOMEPAGE, BUI_K_HOMEPAGE },
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void update_mods(bui_kbd *k)
{
	k->mods = 0;
	if (k->shift_l || k->shift_r)
		k->mods |= BUI_MOD_SHIFT;
	if (k->ctrl_l || k->ctrl_r)
		k->mods |= BUI_MOD_CTRL;
	if (k->alt_l || k->alt_r)
		k->mods |= BUI_MOD_ALT;
}

void bui_kbd_reset(bui_kbd *k)
{
	memset(k, 0, sizeof(*k));
}

/* tasto -> evento del toolkit, secondo i modificatori correnti */
static int translate(const bui_kbd *k, int code, bui_key *out)
{
	int i, shift = (k->mods & BUI_MOD_SHIFT) != 0;

	out->mods = k->mods;
	for (i = 0; i < N(special); i++)
		if (special[i].code == code) {
			out->key = special[i].key;
			return 1;
		}
	if (k->mods & BUI_MOD_ALT)
		for (i = 0; i < N(alt_map); i++)
			if (alt_map[i].code == code) {
				/* un carattere prodotto: niente Alt, altrimenti l'editor lo scarta */
				out->key = shift ^ k->caps ? alt_map[i].hi : alt_map[i].lo;
				out->mods = 0;
				return 1;
			}
	/* come XLookupString: con Ctrl i tasti di carattere non producono testo */
	if (k->mods & BUI_MOD_CTRL)
		return 0;
	for (i = 0; i < N(us); i++)
		if (us[i].code == code) {
			char c = us[i].lo;

			if (c >= 'a' && c <= 'z')
				c = shift ^ k->caps ? us[i].hi : us[i].lo;
			else if (shift)
				c = us[i].hi;
			out->key = (unsigned char)c;
			return 1;
		}
	return 0;
}

static int is_modifier(int code)
{
	return code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT || code == KEY_LEFTCTRL ||
	       code == KEY_RIGHTCTRL || code == KEY_LEFTALT || code == KEY_RIGHTALT ||
	       code == KEY_CAPSLOCK;
}

int bui_kbd_event(bui_kbd *k, int code, int value, uint32_t now, bui_key *out)
{
	if (value == 2)
		return 0;	/* ripetizione del kernel: la facciamo noi */
	if (code == KEY_POWER) {
		if (!out)
			return 0;
		/* pressione e rilascio arrivano entrambi, senza ripetizione: la
		 * shell misura quanto resta premuto */
		k->rep_code = 0;
		out->key = value ? BUI_K_POWER : BUI_K_POWER_UP;
		out->mods = k->mods;
		return 1;
	}
	switch (code) {
	case KEY_LEFTSHIFT: k->shift_l = value != 0; break;
	case KEY_RIGHTSHIFT: k->shift_r = value != 0; break;
	case KEY_LEFTCTRL: k->ctrl_l = value != 0; break;
	case KEY_RIGHTCTRL: k->ctrl_r = value != 0; break;
	case KEY_LEFTALT: k->alt_l = value != 0; break;
	case KEY_RIGHTALT: k->alt_r = value != 0; break;
	case KEY_CAPSLOCK:
		if (value)
			k->caps ^= 1;
		break;
	default:
		break;
	}
	update_mods(k);
	if (is_modifier(code))
		return 0;
	if (!value) {
		if (code == k->rep_code)
			k->rep_code = 0;
		return 0;
	}
	k->rep_code = 0;
	if (!translate(k, code, out))
		return 0;
	if (code == KEY_HOMEPAGE)
		return 1;	/* azione unica: nessuna ripetizione */
	k->rep_code = code;
	k->rep_key = *out;
	k->rep_due = now + BUI_KBD_DELAY_MS;
	return 1;
}

int bui_kbd_repeat(bui_kbd *k, uint32_t now, bui_key *out)
{
	if (!k->rep_code || (int32_t)(now - k->rep_due) < 0)
		return 0;
	*out = k->rep_key;
	k->rep_due += BUI_KBD_RATE_MS;
	if ((int32_t)(now - k->rep_due) >= 0)
		k->rep_due = now + BUI_KBD_RATE_MS;	/* in ritardo: niente raffiche */
	return 1;
}

int bui_kbd_code_for(int c, int *shift, int *alt)
{
	int i;

	*shift = *alt = 0;
	for (i = 0; i < N(us); i++) {
		if (us[i].code >= KEY_KP7 && us[i].code <= KEY_KPDOT)
			continue;
		if ((unsigned char)us[i].lo == c)
			return us[i].code;
		if ((unsigned char)us[i].hi == c) {
			*shift = 1;
			return us[i].code;
		}
	}
	for (i = 0; i < N(alt_map); i++) {
		if (alt_map[i].lo == c) {
			*alt = 1;
			return alt_map[i].code;
		}
		if (alt_map[i].hi == c) {
			*alt = *shift = 1;
			return alt_map[i].code;
		}
	}
	return 0;
}

static const struct {
	const char *name;
	uint16_t code;
} names[] = {
	{ "Return", KEY_ENTER }, { "Enter", KEY_ENTER }, { "KP_Enter", KEY_KPENTER },
	{ "Escape", KEY_ESC }, { "Esc", KEY_ESC }, { "BackSpace", KEY_BACKSPACE },
	{ "Tab", KEY_TAB }, { "Delete", KEY_DELETE }, { "Up", KEY_UP }, { "Down", KEY_DOWN },
	{ "Left", KEY_LEFT }, { "Right", KEY_RIGHT }, { "Prior", KEY_PAGEUP },
	{ "Page_Up", KEY_PAGEUP }, { "Next", KEY_PAGEDOWN }, { "Page_Down", KEY_PAGEDOWN },
	{ "Home", KEY_HOME }, { "End", KEY_END }, { "space", KEY_SPACE },
	{ "minus", KEY_MINUS }, { "equal", KEY_EQUAL }, { "comma", KEY_COMMA },
	{ "period", KEY_DOT }, { "slash", KEY_SLASH }, { "semicolon", KEY_SEMICOLON },
	{ "apostrophe", KEY_APOSTROPHE }, { "grave", KEY_GRAVE }, { "backslash", KEY_BACKSLASH },
	{ "bracketleft", KEY_LEFTBRACE }, { "bracketright", KEY_RIGHTBRACE },
	{ "Shift_L", KEY_LEFTSHIFT }, { "Shift_R", KEY_RIGHTSHIFT }, { "Control_L", KEY_LEFTCTRL },
	{ "Control_R", KEY_RIGHTCTRL }, { "Alt_L", KEY_LEFTALT }, { "Alt_R", KEY_RIGHTALT },
	{ "Caps_Lock", KEY_CAPSLOCK }, { "Insert", KEY_INSERT }, { "Power", KEY_POWER },
	{ "XF86PowerOff", KEY_POWER }, { "XF86HomePage", KEY_HOMEPAGE }, { "HomePage", KEY_HOMEPAGE },
};

int bui_kbd_code_by_name(const char *name)
{
	int i, sh, al;

	for (i = 0; i < N(names); i++)
		if (!strcmp(names[i].name, name))
			return names[i].code;
	if (name[0] && !name[1]) {
		int c = (unsigned char)name[0];

		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		return bui_kbd_code_for(c, &sh, &al);
	}
	return 0;
}
