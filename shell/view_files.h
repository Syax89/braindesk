// SPDX-License-Identifier: MIT
/*
 * view_files.h - parti interne di Esplora: il lettore di testi.
 */
#ifndef VIEW_FILES_H
#define VIEW_FILES_H

#include "shell.h"

#define BS_TEXT_MAX_BYTES (1024 * 1024)	/* si legge al massimo il primo MB */
#define BS_TEXT_MAX_LINES 200000

enum { BS_TEXT_OK, BS_TEXT_E_OPEN, BS_TEXT_E_BINARY, BS_TEXT_E_MEM };

/*
 * Carica `path` nel filo di lavoro (lettura, pulizia, a capo per `width`
 * pixel) e poi chiama done() nel filo principale. Con sniff != 0 un file che
 * non sembra testo viene rifiutato (BS_TEXT_E_BINARY). Una sola richiesta
 * alla volta: una nuova annulla la precedente.
 */
void bs_text_load(const char *path, int width, int sniff, void (*done)(int result, void *user),
		  void *user);
/* annulla il caricamento e libera il testo */
void bs_text_close(void);
/* dopo un caricamento riuscito: mostra il testo nell'area data */
void bs_text_show(bui_win *win, bui_rect area);
/* cambio di dimensione: 1 se basta, 0 se il testo va ricaricato (altra larghezza) */
int bs_text_layout(bui_rect area);
void bs_text_draw(bui_canvas *c, bui_rect clip);
int bs_text_pointer(const bui_pointer *ev);
int bs_text_key(const bui_key *k);
/* "12 kB · 340 righe" */
void bs_text_info(char *buf, int len);
/* larghezza utile per il testo in un'area larga area_w */
int bs_text_width_for(int area_w);

/* "1,4 MB", "12 kB", "340 byte" */
void bs_fmt_size(char *buf, int len, long long bytes);

#endif
