// SPDX-License-Identifier: MIT
/*
 * bui_x.h - accesso al livello Xlib di bui, per il codice che deve parlare
 * col gestore di finestre (EWMH). Le viste non ne hanno bisogno.
 */
#ifndef BUI_X_H
#define BUI_X_H

#include <X11/Xlib.h>

#include "bui.h"

Display *bui_x_display(void);
Window bui_x_root(void);
Window bui_x_window(const bui_win *w);
/* ora dell'ultimo evento di ingresso ricevuto (per i messaggi EWMH) */
Time bui_x_time(void);
/* riceve gli eventi delle finestre non nostre (radice, altri client) */
void bui_x_set_hook(void (*hook)(XEvent *ev));
/* riceve gli eventi che non sono di una finestra bui e che il primo hook non
 * conosce (estensioni, finestre create a mano); ritorna 1 se l'ha usato */
void bui_x_set_ext_hook(int (*hook)(XEvent *ev));
/* vero se l'ultima richiesta ha prodotto un errore X (finestra sparita, ...) */
int bui_x_error_pending(void);

#endif
