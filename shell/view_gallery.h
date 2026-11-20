// SPDX-License-Identifier: MIT
/*
 * view_gallery.h - il visualizzatore di immagini a tutto schermo, usato dalla
 * Galleria e da Esplora. Non e' una vista: e' un componente che la vista
 * ospite mostra al posto del proprio contenuto, girandogli gli eventi.
 */
#ifndef VIEW_GALLERY_H
#define VIEW_GALLERY_H

#include "shell.h"

/* l'elenco delle immagini appartiene a chi apre il visualizzatore */
typedef struct bs_viewer_src {
	int (*count)(void *user);
	const char *(*path)(int index, void *user);
	/* il file `index` e' stato eliminato dal disco: toglilo dall'elenco */
	void (*removed)(int index, void *user);
	/* l'utente e' tornato indietro (il visualizzatore si e' gia' chiuso);
	 * index e' l'ultima immagine mostrata, -1 se non ne restano */
	void (*closed)(int index, void *user);
	void *user;
} bs_viewer_src;

void bs_viewer_open(bui_win *win, bui_rect area, const bs_viewer_src *src, int index);
/* libera tutto e annulla i lavori in corso; non chiama closed() */
void bs_viewer_close(void);
int bs_viewer_active(void);
void bs_viewer_layout(bui_rect area);
void bs_viewer_draw(bui_canvas *c, bui_rect clip);
int bs_viewer_pointer(const bui_pointer *ev);
int bs_viewer_key(const bui_key *k);

#endif
