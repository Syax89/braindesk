// SPDX-License-Identifier: MIT
/*
 * bui_comp.h - compositore software dei backend drm e mem (privato).
 *
 * Ogni finestra bui (home, barra, popup) ha la sua tela in RAM normale. Il
 * compositore tiene l'ordine dei livelli (desktop < normali < dock < popup),
 * chiede alle finestre di ridisegnare i loro rettangoli sporchi e copia
 * nella superficie d'uscita (il dumb buffer DRM, o un buffer in memoria)
 * soltanto i rettangoli cambiati, poi passa all'uscita l'elenco delle fasce
 * di righe da inviare al pannello. Nella superficie d'uscita si scrive e
 * basta, non si legge mai (la memoria CMA write-combined e' lentissima da
 * leggere): il fondo dei popup si ricompone dalle tele.
 */
#ifndef BUI_COMP_H
#define BUI_COMP_H

#include "bui.h"

/*
 * present(): i rettangoli (coordinate dello schermo) appena scritti in
 * `target`, gia' raccolti in fasce di righe ordinate e senza sovrapposizioni
 * (x = 0, w = larghezza dello schermo).
 */
int bui_comp_init(int sw, int sh, bui_px *target, int stride,
		  void (*present)(const bui_rect *bands, int n));
void bui_comp_shutdown(void);
int bui_comp_flush(uint32_t now);
/* tutto lo schermo da ricomporre (ripresa dopo un'app X) */
void bui_comp_damage_screen(bui_rect r);
/* ingresso gia' tradotto in coordinate dello schermo */
void bui_comp_pointer(int type, int x, int y, uint32_t t);
void bui_comp_key(const bui_key *k);
/* annulla un tocco in corso (dispositivi ceduti a X) */
void bui_comp_cancel_pointer(void);
int bui_comp_screen_w(void);
int bui_comp_screen_h(void);
int bui_comp_popups(void);
void bui_comp_redraw_all(void);	/* popup mappati (per le prove) */
/* pixel gia' pronti nella tela della finestra: solo da ricomporre */
void bui_comp_win_present(bui_win *w, bui_rect r);

bui_win *bui_comp_win_create(int kind, bui_rect r, const char *name);
void bui_comp_win_destroy(bui_win *w);
void bui_comp_win_show(bui_win *w);
void bui_comp_win_hide(bui_win *w);
void bui_comp_win_move_resize(bui_win *w, bui_rect r);
void bui_comp_win_set_strut_top(bui_win *w, int h);
bui_win *bui_comp_popup_open(bui_rect r, unsigned flags);
bui_win *bui_comp_popup_open_round(bui_rect r, int radius);

/* raccoglie rettangoli in fasce di righe: unisce quelle distanti meno di
 * `gap` righe. Ritorna il numero di fasce (al massimo n) */
int bui_comp_bands(const bui_rect *r, int n, int sw, int gap, bui_rect *out);

#endif
