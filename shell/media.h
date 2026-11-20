// SPDX-License-Identifier: MIT
/*
 * media.h - cio' che Note, Musica e Video hanno in comune:
 *
 *   media_scan.c         elenco dei file di una cartella, letto da un processo
 *                        figlio (l'interfaccia non aspetta mai la scheda SD)
 *   view_music_player.c  brainplay come figlio in modalita' --slave
 *   view_notes_edit.c    l'editor di testo delle note
 *   media_icons.c        qualche icona in piu'
 */
#ifndef MEDIA_H
#define MEDIA_H

#include <time.h>

#include "shell.h"

/* ------------------------------------------------------------------ */
/* Icone in piu' (media_icons.c)                                      */
/* ------------------------------------------------------------------ */

extern const bui_icon bs_ic_prev_32, bs_ic_next_32, bs_ic_play_32, bs_ic_pause_32;
extern const bui_icon bs_ic_delete_24, bs_ic_save_24;

/* ------------------------------------------------------------------ */
/* Libreria di file (media_scan.c)                                    */
/* ------------------------------------------------------------------ */

#define MS_MAX_ITEMS 4000	/* oltre, l'elenco viene troncato */
#define MS_TEXT 160		/* byte per ciascuno dei due testi aggiuntivi */

typedef struct ms_item {
	char *path;		/* percorso completo */
	const char *rel;	/* dentro path: relativo alla cartella della libreria */
	const char *name;	/* dentro path: nome del file */
	char *s1, *s2;		/* testi prodotti da probe() ("" se assenti) */
	long long size;
	time_t mtime;
} ms_item;

typedef struct ms_lib ms_lib;
struct ms_lib {
	/* --- da riempire prima di ms_scan() --- */
	const char *subdir;	/* cartella sotto $HOME: "Musica" */
	const char *exts;	/* estensioni separate da spazi: "mp3 flac ogg" */
	int recursive;
	/* nel processo figlio: ricava due testi dal file (possono restare vuoti) */
	void (*probe)(const char *path, char *s1, char *s2, int cap);
	int (*cmp)(const ms_item *a, const ms_item *b);
	/* l'elenco (it, n, scanning) e' cambiato */
	void (*changed)(ms_lib *l);

	/* --- stato --- */
	ms_item *it;		/* elenco pubblicato, ordinato con cmp */
	int n;
	int scanning;		/* lettura in corso */
	int loaded;		/* almeno una lettura e' arrivata in fondo */
	/* --- privato --- */
	ms_item *st;		/* elenco in preparazione durante una rilettura */
	int sn, cap, scap;
	int fd, len;
	char *line;		/* riga in arrivo dal figlio (solo durante la lettura) */
};

/* avvia (o riavvia) la lettura. La prima volta le voci compaiono man mano;
 * nelle riletture l'elenco viene sostituito tutto insieme alla fine */
void ms_scan(ms_lib *l);
/* interrompe la lettura in corso (l'elenco pubblicato resta) */
void ms_cancel(ms_lib *l);
void ms_free(ms_lib *l);
/* indice della voce con questo percorso, -1 se manca */
int ms_find(const ms_lib *l, const char *path);
/* aggiorna o aggiunge una voce senza rileggere la cartella; ritorna l'indice */
int ms_upsert(ms_lib *l, const char *path, time_t mtime, long long size, const char *s1,
	      const char *s2);
void ms_remove(ms_lib *l, const char *path);
/* cartella della libreria ($HOME/subdir) */
void ms_dir(const ms_lib *l, char *buf, int len);

/* ordine per cartella e poi per nome (ms_natcmp) */
int ms_cmp_path(const ms_item *a, const ms_item *b);
/* nome del file senza estensione; cartella relativa alla libreria ("" se e' la radice) */
void ms_title(const ms_item *it, char *buf, int cap);
void ms_folder(const ms_item *it, char *buf, int cap);

/* da chiamare nel figlio subito dopo fork(): segnali come all'avvio */
void ms_child_reset(void);

/* testi in italiano: "oggi 14:05", "19 nov", "19 nov 2025"; "820 kB", "1,4 GB" */
void ms_fmt_date(char *buf, int len, time_t t);
void ms_fmt_size(char *buf, int len, long long bytes);
/* confronto senza maiuscole, coi numeri in ordine naturale ("2" < "10") */
int ms_natcmp(const char *a, const char *b);
/* tronca a un numero di byte senza spezzare un carattere UTF-8 */
void ms_utf8_trunc(char *s, int max_bytes);
/* copia al piu' cap-1 byte sostituendo i caratteri di controllo con spazi */
void ms_clean(char *dst, int cap, const char *src, int len);

/* ------------------------------------------------------------------ */
/* Lettore (view_music_player.c)                                      */
/* ------------------------------------------------------------------ */

enum { MP_STOPPED, MP_PLAYING, MP_PAUSED };
enum {
	MP_EV_STATE,	/* state (e brano) cambiati */
	MP_EV_POS,	/* pos o dur cambiati */
	MP_EV_META,	/* titolo o artista arrivati dal lettore */
	MP_EV_VOLUME,
	MP_EV_ENDED,	/* il brano e' arrivato in fondo */
	MP_EV_ERROR,	/* il lettore non e' partito o e' morto */
};

typedef struct mp_status {
	int state;		/* MP_* */
	int pos_ds, dur_ds;	/* decimi di secondo; dur 0 = sconosciuta */
	int volume;		/* 0..100 */
	char path[4096];	/* brano caricato ("" nessuno) */
	char title[MS_TEXT];	/* dal lettore ("" finche' non arriva) */
	char artist[MS_TEXT];
} mp_status;

const mp_status *mp_get(void);
/* chi vuole sapere dei cambiamenti (uno solo: la vista Musica) */
void mp_set_listener(void (*cb)(int ev));
/* ferma il brano in corso e avvia questo; 0 se il processo e' partito */
int mp_play(const char *path);
void mp_toggle(void);
void mp_pause(void);		/* senza effetto se non sta suonando */
void mp_seek(int seconds);
void mp_volume(int v);
void mp_stop(void);
/* percorso del lettore: $BRAIN_PLAYER oppure "brainplay" */
const char *mp_program(void);
/* titolo e artista dai tag ID3 di un mp3 (view_music_tags.c; gira nel figlio di ms_scan) */
void mp_probe_tags(const char *path, char *title, char *artist, int cap);

/* ------------------------------------------------------------------ */
/* Editor delle note (view_notes_edit.c)                              */
/* ------------------------------------------------------------------ */

#define NE_MAX_BYTES (256 * 1024)	/* dimensione massima di una nota */

/* prepara l'editor con questo testo (copiato; tab e CR normalizzati).
 * changed() viene chiamata dopo ogni modifica. Ritorna 0 se riuscito */
int ne_open(bui_win *win, const char *text, int len, void (*changed)(void));
void ne_close(void);
void ne_layout(bui_rect area);
void ne_draw(bui_canvas *c, bui_rect clip);
int ne_pointer(const bui_pointer *ev);
int ne_key(const bui_key *k);
/* testo corrente (non terminato da zero) */
const char *ne_text(int *len);

#endif
