// SPDX-License-Identifier: MIT
/*
 * bui_be.h - interfaccia PRIVATA fra il nucleo del toolkit (bui_core.c) e i
 * backend che mettono i pixel sullo schermo e portano tocchi e tasti:
 *
 *   x11   bui_x.c     finestre X11 + MIT-SHM (sviluppo, prove sotto Xvfb,
 *                     sessione X di ripiego sul Brain)
 *   drm   bui_drm.c   il pannello direttamente via DRM, ingresso da evdev
 *   mem   bui_mem.c   tutto in memoria, ingresso da uno script (prove senza X)
 *
 * drm e mem condividono il compositore software di bui_comp.c: un'unica
 * superficie a tutto schermo su cui home, barra e popup sono livelli.
 * Le viste e la shell non includono questo file.
 */
#ifndef BUI_BE_H
#define BUI_BE_H

#include "bui.h"

typedef struct bui_backend {
	const char *name;
	int (*init)(void);
	void (*shutdown)(void);
	int (*screen_w)(void);
	int (*screen_h)(void);

	bui_win *(*win_create)(int kind, bui_rect r, const char *name);
	void (*win_destroy)(bui_win *w);
	void (*win_show)(bui_win *w);
	void (*win_hide)(bui_win *w);
	void (*win_move_resize)(bui_win *w, bui_rect r);
	void (*win_set_strut_top)(bui_win *w, int h);
	bui_win *(*popup_open)(bui_rect r, unsigned flags);
	bui_win *(*popup_open_round)(bui_rect r, int radius);

	/* eventi gia' arrivati da consegnare prima di timer e disegno */
	void (*pump)(void);
	/* eventi in coda che non si vedrebbero nel poll() (Xlib) */
	int (*pending)(void);
	/* disegna e invia; ritorna fra quanti ms riprovare (-1: nulla in sospeso) */
	int (*flush)(uint32_t now);
	/* descrittore proprio del backend da sorvegliare (-1 nessuno); se
	 * segnala HUP/ERR il ciclo esce */
	int (*conn_fd)(void);
	void (*sync)(void);
	/* cede (1) o riprende (0) schermo e ingresso; NULL se non si puo' */
	int (*suspend)(int on);
	/* tutte le finestre da ridisegnare e reinviare */
	void (*redraw_all)(void);
	/* invia un rettangolo della tela senza ridisegnarlo */
	void (*win_present)(bui_win *w, bui_rect r);
} bui_backend;

extern const bui_backend bui_be_x11, bui_be_drm, bui_be_mem;

/* --- servizi del nucleo per i backend --- */

/* filtri d'ingresso: ritorna 1 se l'evento va scartato */
int bui__input_filter(int kind, const bui_key *k, int suspended);
/* aggancio dei tasti della shell: 1 se il tasto e' stato usato */
int bui__key_hook(const bui_key *k);
/* chi ha chiuso un popup lo annota (bui_popup_last_close) */
void bui__popup_closed_now(void);
/* scurisce un'immagine a 3/8 della luminosita', con dithering */
void bui__dim(bui_px *p, int w, int h, int stride);
/* tutte le finestre cedute? (mentre un'app X ha lo schermo) */
int bui__suspended(void);
/* l'evento col tempo t va scartato (precede l'ultimo risveglio)? */
int bui__input_stale(uint32_t t);
/* orologio dei tempi degli eventi (CLOCK_BOOTTIME, come bui_now) */
#define BUI_CLOCK CLOCK_BOOTTIME

#endif
