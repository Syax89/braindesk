// SPDX-License-Identifier: MIT
/*
 * bui_evdev.h - traduzione degli eventi evdev (struct input_event) negli
 * eventi del toolkit: tocco del pannello resistivo (calibrazione, rotazione,
 * filtro) e tastiera (mappa US con Shift, accentate con Alt, ripetizione).
 *
 * Qui non c'e' nessun accesso ai dispositivi: solo stato e conti interi,
 * cosi' la traduzione si prova da sola con flussi registrati
 * (test/evdev_test.c). L'apertura dei dispositivi e' in bui_drm.c.
 */
#ifndef BUI_EVDEV_H
#define BUI_EVDEV_H

#include <linux/input.h>
#include <stdint.h>

#include "bui.h"

/* ---- tocco ---- */

typedef struct bui_touch_cal {
	/* valori grezzi che cadono sui bordi sinistro, destro, alto, basso dello
	 * schermo in orizzontale (come "Calibration" di xf86-input-evdev: miny >
	 * maxy vuol dire asse invertito) */
	int minx, maxx, miny, maxy;
	int rot;		/* /etc/brain-rotation: 0 orizzontale, 1 verticale, 2 verticale capovolto */
	int sw, sh;		/* schermo nell'orientamento in uso */
	int deadzone;		/* px: un movimento piu' piccolo non viene inviato */
} bui_touch_cal;

typedef struct bui_touch {
	bui_touch_cal cal;
	int rx, ry;		/* ultimo campione grezzo */
	int hx[3], hy[3], hn;	/* ultimi campioni (mediana di 3) */
	uint8_t btn, was, have_xy, move_pending, dropped;
	int sx, sy;		/* ultimo punto inviato */
	int px, py;		/* movimento in attesa (si invia solo l'ultimo di una raffica) */
	uint32_t t;
	void (*emit)(int type, int x, int y, uint32_t t, void *user);
	void *user;
} bui_touch;

/* coordinate dello schermo per un campione grezzo (senza filtro) */
void bui_touch_map(const bui_touch_cal *c, int rx, int ry, int *sx, int *sy);
void bui_touch_init(bui_touch *t, const bui_touch_cal *cal,
		    void (*emit)(int type, int x, int y, uint32_t t, void *user), void *user);
/* un evento letto dal dispositivo */
void bui_touch_event(bui_touch *t, const struct input_event *ev);
/* fine di una lettura: invia il movimento rimasto in attesa */
void bui_touch_flush(bui_touch *t);
/* dimentica il tocco in corso senza inviare nulla (dispositivo ceduto) */
void bui_touch_reset(bui_touch *t);
/* "181 3986 3986 255" (Option "Calibration" di xorg.conf); 0 se letta */
int bui_touch_parse_cal(const char *s, bui_touch_cal *c);

/* ---- tastiera ---- */

#define BUI_KBD_DELAY_MS 500	/* prima ripetizione */
#define BUI_KBD_RATE_MS 40	/* poi 25 al secondo */

typedef struct bui_kbd {
	unsigned mods;		/* BUI_MOD_* correnti */
	uint8_t shift_l, shift_r, ctrl_l, ctrl_r, alt_l, alt_r, caps;
	int rep_code;		/* tasto che si sta ripetendo (0 nessuno) */
	bui_key rep_key;
	uint32_t rep_due;
} bui_kbd;

/*
 * Un evento EV_KEY (value 1 premuto, 0 rilasciato; 2, la ripetizione del
 * kernel, e' ignorato: la ripetizione la fa bui_kbd_repeat). Ritorna 1 se
 * ne esce un tasto del toolkit in *out. `now` serve a programmare la
 * ripetizione.
 */
int bui_kbd_event(bui_kbd *k, int code, int value, uint32_t now, bui_key *out);
/* ripetizione: se e' l'ora ritorna 1 e il tasto da ripetere */
int bui_kbd_repeat(bui_kbd *k, uint32_t now, bui_key *out);
/* ferma la ripetizione e dimentica i modificatori premuti */
void bui_kbd_reset(bui_kbd *k);
/* per gli script di prova: codice evdev e Shift/Alt che producono il carattere c */
int bui_kbd_code_for(int c, int *shift, int *alt);
/* nome di un tasto ("Return", "a", "F1"...) -> codice evdev, 0 se sconosciuto */
int bui_kbd_code_by_name(const char *name);

#endif
