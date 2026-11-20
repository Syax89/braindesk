// SPDX-License-Identifier: MIT
/*
 * vt.h - emulatore di terminale VT100/xterm per la vista Terminale.
 *
 * Solo stato e conti interi, nessun accesso a bui o al PTY: riceve i byte
 * scritti dai programmi (vt_write) e tiene la griglia delle celle, lo
 * storico, le modalita' e quali righe sono cambiate. Cosi' si prova da solo
 * con flussi registrati (test/vt_test.c). Il disegno e' in view_term.c.
 *
 * Coperto: UTF-8 (caratteri larghi su due celle, combinanti ignorati),
 * movimenti del cursore, SGR (16, 256 colori e truecolor, che diventa
 * RGB565), grassetto/attenuato/sottolineato/inverso/barrato, cancellazioni
 * (con il colore di fondo corrente: "bce"), regioni di scorrimento
 * (DECSTBM), inserimento e cancellazione di righe e caratteri, schermo
 * alternativo (47/1047/1049), tasti cursore in modo applicazione (DECCKM),
 * modo inserimento, a capo automatico differito, tabulazioni, set grafico
 * DEC (linee dei riquadri di ncurses), REP, risposte a DA/DSR, titolo
 * (OSC 0/1/2). Ignorati senza danni: incolla tra parentesi (2004), mouse,
 * forma del cursore, operazioni sulla finestra, DCS/APC/PM.
 */
#ifndef VT_H
#define VT_H

#include <stddef.h>
#include <stdint.h>

/* attributi di una cella */
enum {
	VT_BOLD = 1 << 0,
	VT_DIM = 1 << 1,
	VT_ITALIC = 1 << 2,
	VT_UNDER = 1 << 3,
	VT_BLINK = 1 << 4,
	VT_REVERSE = 1 << 5,
	VT_INVIS = 1 << 6,
	VT_STRIKE = 1 << 7,
	VT_WIDE = 1 << 8,	/* prima meta' di un carattere largo */
	VT_WCONT = 1 << 9,	/* seconda meta' (cp = 0) */
	VT_FG_RGB = 1 << 10,	/* fg e' un colore RGB565, non un indice */
	VT_BG_RGB = 1 << 11,
};

/* fg/bg: 0..255 indice della tavolozza, VT_DEFAULT colore del tema; con
 * VT_FG_RGB / VT_BG_RGB il valore e' direttamente RGB565 */
#define VT_DEFAULT 256

typedef struct vt_cell {
	uint32_t cp;		/* codice Unicode, ' ' per una cella vuota */
	uint16_t fg, bg;
	uint16_t attr;
	uint16_t pad;
} vt_cell;

typedef struct vt_cursor {
	int x, y;
	vt_cell pen;		/* colori e attributi dei caratteri nuovi */
	uint8_t wrapnext;	/* scritto nell'ultima colonna: a capo al prossimo carattere */
	uint8_t origin;		/* DECOM */
	uint8_t cs[2];		/* G0, G1: 'B' ASCII, '0' grafico DEC, 'A' UK */
	uint8_t gl;		/* 0 = G0, 1 = G1 (SI/SO) */
} vt_cursor;

typedef struct vt {
	int cols, rows;
	vt_cell **line;		/* righe dello schermo in uso */
	vt_cell **lmain, **lalt;
	vt_cell *bmain, *balt;
	int alt;		/* schermo alternativo in uso */
	vt_cursor c, saved, saved_alt;
	int top, bot;		/* margini di scorrimento (inclusi) */
	uint8_t *tabs;

	/* modalita' */
	uint8_t m_wrap, m_insert, m_lnm, m_appcursor, m_appkeypad, m_cursor, m_reverse;
	uint8_t m_bracket, m_mouse;

	/* storico: anello di sb_cap righe da sb_cols celle */
	vt_cell *sb;
	int sb_cap, sb_cols, sb_count, sb_head;
	uint32_t sb_pushed;	/* righe entrate nello storico da sempre */

	/*
	 * Righe cambiate dall'ultimo vt_clean(): dirty[y], colonne [dlo, dhi].
	 * Uno scorrimento in attesa (sc_n righe in su, negativo in giu', della
	 * regione [sc_top, sc_bot]) dice a chi disegna che i pixel di quelle righe
	 * vanno spostati invece che ridisegnati: i segni di "cambiata" sono gia'
	 * stati spostati con le righe. all_dirty: tutto da ridisegnare.
	 */
	uint8_t *dirty;
	int16_t *dlo, *dhi;
	int all_dirty;
	int sc_top, sc_bot, sc_n;

	/* analizzatore */
	int st;
	uint32_t ucp;
	int uneed;
	uint32_t umin;
	int par[16];
	uint8_t sub[16];
	int npar;
	char priv, inter;
	char osc[512];
	int osc_len;
	uint32_t last_cp;
	char title[256];

	/* risposte al programma (DA, DSR), titolo, campanello */
	void (*reply)(void *user, const char *s, int n);
	void (*on_title)(void *user, const char *title);
	void (*on_bell)(void *user);
	void *user;
} vt;

/* 0 se riuscito */
int vt_init(vt *t, int cols, int rows, int sb_lines);
void vt_free(vt *t);
/* cambio di dimensione: storico e righe conservati (troncate), tutto da ridisegnare */
int vt_resize(vt *t, int cols, int rows);
/* byte scritti dal programma */
void vt_write(vt *t, const uint8_t *buf, size_t n);
static inline void vt_puts(vt *t, const char *s)
{
	const char *e = s;

	while (*e)
		e++;
	vt_write(t, (const uint8_t *)s, (size_t)(e - s));
}
/* ESC c */
void vt_reset(vt *t);

/* riga y: 0..rows-1 lo schermo, -1..-sb_count lo storico (-1 la piu' recente);
 * le righe dello storico possono essere piu' corte di cols (ritorna in *n) */
const vt_cell *vt_row(const vt *t, int y, int *n);

/* tutte le righe pulite, nessuno scorrimento in attesa */
void vt_clean(vt *t);
void vt_damage_all(vt *t);
/* lo scorrimento in attesa diventa un ridisegno delle sue righe (chi disegna
 * ha gia' rifatto quei pixel dalle celle e non deve piu' spostarli) */
void vt_flush_scroll(vt *t);
/* segna le colonne [x0, x1] della riga y (es. il cursore che si sposta) */
void vt_mark(vt *t, int y, int x0, int x1);

/* larghezza di un carattere: 0 combinante, 1, 2 largo (CJK, emoji) */
int vt_wcwidth(uint32_t cp);

/* ---- tastiera -> byte per il programma ---- */

enum {
	VT_K_UP = 1, VT_K_DOWN, VT_K_RIGHT, VT_K_LEFT, VT_K_HOME, VT_K_END,
	VT_K_INSERT, VT_K_DELETE, VT_K_PGUP, VT_K_PGDN,
	VT_K_BACKSPACE, VT_K_TAB, VT_K_ENTER, VT_K_ESC,
};
enum { VT_MOD_SHIFT = 1, VT_MOD_ALT = 2, VT_MOD_CTRL = 4 };

/* tasto speciale -> sequenza (secondo DECCKM e LNM); ritorna i byte scritti (<= 16) */
int vt_key(const vt *t, int key, int mods, char *out);
/* carattere -> UTF-8; con Ctrl il codice di controllo, con Alt ESC davanti (<= 8 byte) */
int vt_char(uint32_t cp, int mods, char *out);

#endif
