// SPDX-License-Identifier: MIT
/*
 * view_term.c - Terminale integrato: PTY + emulatore (vt.c) + disegno a
 * celle, senza X.
 *
 * - La shell dell'utente ($SHELL, altrimenti /bin/bash, come shell di login)
 *   gira su un PTY (posix_openpt); il lato master e' un descrittore nel
 *   poll() del toolkit. TERM=xterm-256color (se la terminfo c'e', altrimenti
 *   xterm), dimensione con TIOCSWINSZ a ogni cambio di layout.
 * - Celle da 8x16 px (DejaVu Sans Mono 13 px, normale e grassetto): 106x24 in
 *   orizzontale, 60x44 in verticale. Linee dei riquadri, blocchi e braille
 *   sono disegnati a misura di cella (si uniscono senza buchi); i caratteri
 *   senza glifo diventano un rettangolo, quelli larghi un rettangolo su due
 *   celle.
 * - Disegno minimo: il fiotto di uscita si accumula e si disegna al massimo
 *   ~30 volte al secondo. Solo le celle cambiate vengono ridisegnate,
 *   direttamente nella tela, e inviate (bui_win_present); uno scorrimento
 *   sposta le righe della tela con memmove (una volta per fotogramma, anche
 *   se nel frattempo sono arrivate cento righe) e ridisegna solo le nuove.
 * - Storico di 1000 righe: trascinamento con inerzia, Maiusc+PagSu/PagGiu';
 *   un tasto qualsiasi torna in fondo. Nello schermo alternativo (less, vim)
 *   il trascinamento manda frecce su/giu'.
 * - Tastiera fisica -> byte (frecce secondo DECCKM, Ctrl, Alt = ESC davanti,
 *   accentate in UTF-8). Esc (Go Back) va al programma: si esce con Home o
 *   con la freccia dell'intestazione. Barra di tasti a tocco: Esc, Tab, Ctrl
 *   (resta premuto per il tasto successivo), frecce, | ~ / -.
 * - La sessione sopravvive all'uscita dalla vista; a processo finito,
 *   "[processo terminato]" e "Riavvia" (o Invio).
 * - Energia: la sospensione automatica aspetta solo se un programma in primo
 *   piano (non la shell al prompt) ha scritto negli ultimi 30 s.
 * - Ripiego X: pressione lunga sul testo -> "Apri in X" (st come prima).
 */
#define _GNU_SOURCE	/* posix_openpt, ptsname_r */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "shell.h"
#include "vt.h"

#define CW 8			/* cella */
#define CH 16
#define BASE 12			/* linea di base nella cella */
#define SB_LINES 1000
#define RENDER_MS 33		/* al massimo ~30 fotogrammi al secondo */
#define AWAKE_MS 30000		/* uscita recente in primo piano: niente sospensione */
#define LONG_MS 800
#define HEAD_H BUI_HEADER_H
#define BAR_H 48		/* barra dei tasti in verticale */
#define NKEYS 11

enum { TK_ESC, TK_TAB, TK_CTRL, TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT, TK_PIPE, TK_TILDE, TK_SLASH, TK_MINUS };

static const char *const key_labels[NKEYS] = {
	"Esc", "Tab", "Ctrl", "\xE2\x86\x91", "\xE2\x86\x93", "\xE2\x86\x90", "\xE2\x86\x92", "|", "~", "/", "-",
};

static struct {
	vt vt;
	int vt_ok;
	int pty, pid, alive, started, exited;
	int render_timer, lp_timer;
	uint32_t last_render, last_fg_output, last_key;
	int awake_ms;

	bui_win *win;
	bui_panel p;
	bui_rect area, text;	/* area della vista, area del testo (coordinate della finestra) */
	int gx, gy;		/* angolo della griglia */
	int cap_panel;
	bui_widget *w_title, *w_ctrl;
	char title[128];
	int ctrl;		/* Ctrl della barra: vale per il tasto successivo */

	/* storico */
	int view_off;		/* righe di storico mostrate sopra lo schermo */
	uint32_t seen_pushed;
	bui_scroll sc;
	int drag, press_x, press_y, alt_acc, fling;

	/* cursore come disegnato */
	int dcx, dcy, don;

	bui_px pal[256], fg, bg, cur_bg, cur_fg;
	const char *dump;	/* BRAINSHELL_TERM_DUMP: testo dello schermo per le prove */
} T = { .pty = -1 };

static void sync_screen(void);
static void rebuild(void);

/* ------------------------------------------------------------------ */
/* colori                                                             */
/* ------------------------------------------------------------------ */

static const uint32_t pal_dark[16] = {
	0x000000, 0xcd3131, 0x0dbc79, 0xe5e510, 0x2472c8, 0xbc3fbc, 0x11a8cd, 0xe5e5e5,
	0x767676, 0xf14c4c, 0x23d18b, 0xf5f543, 0x3b8eea, 0xd670d6, 0x29b8db, 0xffffff,
};
static const uint32_t pal_light[16] = {
	0x000000, 0xcd3131, 0x00a000, 0x949800, 0x0451a5, 0xbc05bc, 0x0598bc, 0x555555,
	0x666666, 0xcd3131, 0x14a014, 0x8a8f00, 0x0451a5, 0xbc05bc, 0x0598bc, 0x333333,
};

static void palette(void)
{
	static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
	const uint32_t *base = bui_th.light ? pal_light : pal_dark;
	int i;

	for (i = 0; i < 16; i++)
		T.pal[i] = BUI_HEX(base[i]);
	for (i = 16; i < 232; i++) {
		int n = i - 16;

		T.pal[i] = BUI_RGB(lv[n / 36], lv[(n / 6) % 6], lv[n % 6]);
	}
	for (i = 232; i < 256; i++) {
		int g = 8 + 10 * (i - 232);

		T.pal[i] = BUI_RGB(g, g, g);
	}
	T.fg = BUI_C_TEXT;
	T.bg = BUI_C_BG;
	T.cur_bg = BUI_C_ACCENT;
	T.cur_fg = BUI_C_ON_ACCENT;
}

static bui_px cell_fg(const vt_cell *c)
{
	if (c->attr & VT_FG_RGB)
		return c->fg;
	if (c->fg == VT_DEFAULT)
		return T.vt.m_reverse ? T.bg : T.fg;
	/* grassetto sui primi 8 colori: la versione chiara, come xterm */
	if ((c->attr & VT_BOLD) && c->fg < 8)
		return T.pal[c->fg + 8];
	return T.pal[c->fg & 255];
}

static bui_px cell_bg(const vt_cell *c)
{
	if (c->attr & VT_BG_RGB)
		return c->bg;
	if (c->bg == VT_DEFAULT)
		return T.vt.m_reverse ? T.fg : T.bg;
	return T.pal[c->bg & 255];
}

/* ------------------------------------------------------------------ */
/* glifi                                                              */
/* ------------------------------------------------------------------ */

static const bui_glyph *glyph(const bui_font *f, uint32_t cp)
{
	int lo = 0, hi = f->nglyphs - 1;

	if (cp >= 0x20 && cp < 0x7F)
		return &f->glyphs[cp - 0x20];	/* l'ASCII e' in testa, in ordine */
	if (cp > 0xFFFF)
		return NULL;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;

		if (f->glyphs[mid].cp == cp)
			return &f->glyphs[mid];
		if (f->glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

/* linee dei riquadri U+2500-257F: peso dei bracci su, destra, giu', sinistra
 * (0 niente, 1 sottile, 2 spesso, 3 doppio), due bit ciascuno */
#define A(u, r, d, l) ((u) | ((r) << 2) | ((d) << 4) | ((l) << 6))
static const uint8_t box[128] = {
	A(0,1,0,1), A(0,2,0,2), A(1,0,1,0), A(2,0,2,0), A(0,1,0,1), A(0,2,0,2), A(1,0,1,0), A(2,0,2,0),
	A(0,1,0,1), A(0,2,0,2), A(1,0,1,0), A(2,0,2,0), A(0,1,1,0), A(0,2,1,0), A(0,1,2,0), A(0,2,2,0),
	A(0,0,1,1), A(0,0,1,2), A(0,0,2,1), A(0,0,2,2), A(1,1,0,0), A(1,2,0,0), A(2,1,0,0), A(2,2,0,0),
	A(1,0,0,1), A(1,0,0,2), A(2,0,0,1), A(2,0,0,2), A(1,1,1,0), A(1,2,1,0), A(2,1,1,0), A(1,1,2,0),
	A(2,1,2,0), A(2,2,1,0), A(1,2,2,0), A(2,2,2,0), A(1,0,1,1), A(1,0,1,2), A(2,0,1,1), A(1,0,2,1),
	A(2,0,2,1), A(2,0,1,2), A(1,0,2,2), A(2,0,2,2), A(0,1,1,1), A(0,1,1,2), A(0,2,1,1), A(0,2,1,2),
	A(0,1,2,1), A(0,1,2,2), A(0,2,2,1), A(0,2,2,2), A(1,1,0,1), A(1,1,0,2), A(1,2,0,1), A(1,2,0,2),
	A(2,1,0,1), A(2,1,0,2), A(2,2,0,1), A(2,2,0,2), A(1,1,1,1), A(1,1,1,2), A(1,2,1,1), A(1,2,1,2),
	A(2,1,1,1), A(1,1,2,1), A(2,1,2,1), A(2,1,1,2), A(2,2,1,1), A(1,1,2,2), A(1,2,2,1), A(2,2,1,2),
	A(1,2,2,2), A(2,1,2,2), A(2,2,2,1), A(2,2,2,2), A(0,1,0,1), A(0,2,0,2), A(1,0,1,0), A(2,0,2,0),
	A(0,3,0,3), A(3,0,3,0), A(0,3,1,0), A(0,1,3,0), A(0,3,3,0), A(0,0,1,3), A(0,0,3,1), A(0,0,3,3),
	A(1,3,0,0), A(3,1,0,0), A(3,3,0,0), A(1,0,0,3), A(3,0,0,1), A(3,0,0,3), A(1,3,1,0), A(3,1,3,0),
	A(3,3,3,0), A(1,0,1,3), A(3,0,3,1), A(3,0,3,3), A(0,3,1,3), A(0,1,3,1), A(0,3,3,3), A(1,3,0,3),
	A(3,1,0,1), A(3,3,0,3), A(1,3,1,3), A(3,1,3,1), A(3,3,3,3), A(0,1,1,0), A(0,0,1,1), A(1,0,0,1),
	A(1,1,0,0), 0, 0, 0, A(0,0,0,1), A(1,0,0,0), A(0,1,0,0), A(0,0,1,0),
	A(0,0,0,2), A(2,0,0,0), A(0,2,0,0), A(0,0,2,0), A(0,2,0,1), A(1,0,2,0), A(0,1,0,2), A(2,0,1,0),
};
#undef A

static void hl(bui_canvas *c, int ox, int oy, int x0, int x1, int y, bui_px col)
{
	if (x1 >= x0)
		bui_fill(c, bui_r(ox + x0, oy + y, x1 - x0 + 1, 1), col);
}

static void vl(bui_canvas *c, int ox, int oy, int x, int y0, int y1, bui_px col)
{
	if (y1 >= y0)
		bui_fill(c, bui_r(ox + x, oy + y0, 1, y1 - y0 + 1), col);
}

/* estremi di un tratto di peso w centrato in m */
static void span(int w, int m, int *a, int *b)
{
	*a = w == 3 ? m - 1 : m;
	*b = w == 1 ? m : m + 1;
}

static void draw_lines(bui_canvas *c, int ox, int oy, uint8_t a, bui_px col)
{
	int u = a & 3, r = (a >> 2) & 3, d = (a >> 4) & 3, l = (a >> 6) & 3;
	int cx = CW / 2 - 1, cy = CH / 2 - 1;
	int vm = u > d ? u : d, hm = l > r ? l : r;
	int vx0 = cx, vx1 = cx, hy0 = cy, hy1 = cy, s0, s1, i;

	if (vm)
		span(vm, cx, &vx0, &vx1);
	if (hm)
		span(hm, cy, &hy0, &hy1);
	/* sinistra */
	if (l == 3) {
		hl(c, ox, oy, 0, u == 3 ? cx - 1 : d == 3 ? cx + 1 : vx1, cy - 1, col);
		hl(c, ox, oy, 0, d == 3 ? cx - 1 : u == 3 ? cx + 1 : vx1, cy + 1, col);
	} else if (l) {
		span(l, cy, &s0, &s1);
		for (i = s0; i <= s1; i++)
			hl(c, ox, oy, 0, (vm == 3 && !r) ? cx - 1 : vx1, i, col);
	}
	/* destra */
	if (r == 3) {
		hl(c, ox, oy, u == 3 ? cx + 1 : d == 3 ? cx - 1 : vx0, CW - 1, cy - 1, col);
		hl(c, ox, oy, d == 3 ? cx + 1 : u == 3 ? cx - 1 : vx0, CW - 1, cy + 1, col);
	} else if (r) {
		span(r, cy, &s0, &s1);
		for (i = s0; i <= s1; i++)
			hl(c, ox, oy, (vm == 3 && !l) ? cx + 1 : vx0, CW - 1, i, col);
	}
	/* su */
	if (u == 3) {
		vl(c, ox, oy, cx - 1, 0, l == 3 ? cy - 1 : r == 3 ? cy + 1 : hy1, col);
		vl(c, ox, oy, cx + 1, 0, r == 3 ? cy - 1 : l == 3 ? cy + 1 : hy1, col);
	} else if (u) {
		span(u, cx, &s0, &s1);
		for (i = s0; i <= s1; i++)
			vl(c, ox, oy, i, 0, (hm == 3 && !d) ? cy - 1 : hy1, col);
	}
	/* giu' */
	if (d == 3) {
		vl(c, ox, oy, cx - 1, l == 3 ? cy + 1 : r == 3 ? cy - 1 : hy0, CH - 1, col);
		vl(c, ox, oy, cx + 1, r == 3 ? cy + 1 : l == 3 ? cy - 1 : hy0, CH - 1, col);
	} else if (d) {
		span(d, cx, &s0, &s1);
		for (i = s0; i <= s1; i++)
			vl(c, ox, oy, i, (hm == 3 && !u) ? cy + 1 : hy0, CH - 1, col);
	}
}

/* disegna a misura di cella; 0 se il carattere non e' dei nostri */
static int draw_special(bui_canvas *c, int ox, int oy, uint32_t cp, bui_px fg, bui_px bg)
{
	int y;

	if (cp >= 0x2500 && cp <= 0x257F) {
		uint8_t a = box[cp - 0x2500];

		if (a) {
			draw_lines(c, ox, oy, a, fg);
			return 1;
		}
		for (y = 0; y < CH; y++) {
			int x = y * (CW - 1) / (CH - 1);

			if (cp != 0x2572)	/* / */
				bui_fill(c, bui_r(ox + CW - 1 - x, oy + y, 1, 1), fg);
			if (cp != 0x2571)	/* \ */
				bui_fill(c, bui_r(ox + x, oy + y, 1, 1), fg);
		}
		return 1;
	}
	if (cp >= 0x2580 && cp <= 0x259F) {
		int q = 0;

		switch (cp) {
		case 0x2580: bui_fill(c, bui_r(ox, oy, CW, CH / 2), fg); return 1;
		case 0x2590: bui_fill(c, bui_r(ox + CW / 2, oy, CW / 2, CH), fg); return 1;
		case 0x2591: case 0x2592: case 0x2593:
			bui_fill(c, bui_r(ox, oy, CW, CH), bui_mix(bg, fg, (int)(cp - 0x2590) * 64));
			return 1;
		case 0x2594: bui_fill(c, bui_r(ox, oy, CW, CH / 8), fg); return 1;
		case 0x2595: bui_fill(c, bui_r(ox + CW - CW / 8, oy, CW / 8, CH), fg); return 1;
		case 0x2596: q = 4; break;
		case 0x2597: q = 8; break;
		case 0x2598: q = 1; break;
		case 0x2599: q = 1 | 4 | 8; break;
		case 0x259A: q = 1 | 8; break;
		case 0x259B: q = 1 | 2 | 4; break;
		case 0x259C: q = 1 | 2 | 8; break;
		case 0x259D: q = 2; break;
		case 0x259E: q = 2 | 4; break;
		case 0x259F: q = 2 | 4 | 8; break;
		default:
			if (cp <= 0x2588) {	/* 1/8 .. 8/8 dal basso */
				int h = (int)(cp - 0x2580) * CH / 8;

				bui_fill(c, bui_r(ox, oy + CH - h, CW, h), fg);
			} else {	/* 7/8 .. 1/8 da sinistra */
				int w = (int)(0x2590 - cp) * CW / 8;

				bui_fill(c, bui_r(ox, oy, w, CH), fg);
			}
			return 1;
		}
		if (q & 1)
			bui_fill(c, bui_r(ox, oy, CW / 2, CH / 2), fg);
		if (q & 2)
			bui_fill(c, bui_r(ox + CW / 2, oy, CW / 2, CH / 2), fg);
		if (q & 4)
			bui_fill(c, bui_r(ox, oy + CH / 2, CW / 2, CH / 2), fg);
		if (q & 8)
			bui_fill(c, bui_r(ox + CW / 2, oy + CH / 2, CW / 2, CH / 2), fg);
		return 1;
	}
	if (cp >= 0x2800 && cp <= 0x28FF) {
		/* braille: 2x4 punti (grafici di htop, btop) */
		static const uint8_t bit[4][2] = { { 0x01, 0x08 }, { 0x02, 0x10 }, { 0x04, 0x20 }, { 0x40, 0x80 } };
		int i, j;

		for (i = 0; i < 4; i++)
			for (j = 0; j < 2; j++)
				if ((cp & 0xFF) & bit[i][j])
					bui_fill(c, bui_r(ox + 1 + j * 4, oy + 1 + i * 4, 2, 2), fg);
		return 1;
	}
	if (cp >= 0x23BA && cp <= 0x23BD) {
		/* linee di scansione del set grafico DEC */
		static const uint8_t sy[4] = { 1, 4, 11, 14 };

		bui_fill(c, bui_r(ox, oy + sy[cp - 0x23BA], CW, 1), fg);
		return 1;
	}
	return 0;
}

static void draw_glyph(bui_canvas *c, int ox, int oy, int w, const vt_cell *cell, bui_px fg, bui_px bg)
{
	const bui_font *f = (cell->attr & VT_BOLD) ? BUI_FONT_MONO_B : BUI_FONT_MONO;
	const bui_glyph *g;
	uint32_t cp = cell->cp;

	if (cp == ' ' || cp == 0 || cp == 0xA0)
		return;
	if (cp >= 0x2300 && draw_special(c, ox, oy, cp, fg, bg))
		return;
	g = glyph(f, cp);
	if (g && (g->w || cp == ' ')) {
		int dy = 0, x = ox + g->bx + (w - 1) * CW / 2;

		/* maiuscole accentate piu' alte della cella: un pixel in giu' se c'e' posto */
		if (g->by > BASE && g->h - g->by + (g->by - BASE) <= CH - BASE)
			dy = g->by - BASE;
		bui_mask(c, x, oy + BASE - g->by + dy, f->data + g->off, g->w, g->h, g->w, fg, 255);
		return;
	}
	if (g)
		return;		/* glifo vuoto (spazi tipografici) */
	/* senza glifo: un rettangolo grande quanto il carattere */
	bui_fill(c, bui_r(ox + 1, oy + 3, w * CW - 2, 1), fg);
	bui_fill(c, bui_r(ox + 1, oy + CH - 3, w * CW - 2, 1), fg);
	bui_fill(c, bui_r(ox + 1, oy + 3, 1, CH - 5), fg);
	bui_fill(c, bui_r(ox + w * CW - 2, oy + 3, 1, CH - 5), fg);
}

/* ------------------------------------------------------------------ */
/* disegno delle celle                                                */
/* ------------------------------------------------------------------ */

/* riga dello schermo mostrata alla riga r della griglia (con lo storico) */
static const vt_cell *display_row(int r, int *n)
{
	return vt_row(&T.vt, r - T.view_off, n);
}

/* celle [c0, c1] della riga r; il clip della tela limita gia' il disegno */
static void draw_row(bui_canvas *c, int r, int c0, int c1)
{
	int n, x, oy = T.gy + r * CH;
	const vt_cell *row = display_row(r, &n);
	int cur = T.don && r == T.dcy + T.view_off && T.view_off == 0;

	if (c1 >= T.vt.cols)
		c1 = T.vt.cols - 1;
	if (!row) {
		bui_fill(c, bui_r(T.gx + c0 * CW, oy, (c1 - c0 + 1) * CW, CH), T.bg);
		return;
	}
	/* un carattere largo comincia una cella prima */
	if (c0 > 0 && c0 < n && (row[c0].attr & VT_WCONT))
		c0--;
	for (x = c0; x <= c1; x++) {
		vt_cell cl;
		bui_px fg, bg;
		int w = 1, ox = T.gx + x * CW;

		if (x >= n) {
			bui_fill(c, bui_r(ox, oy, (c1 - x + 1) * CW, CH), T.bg);
			break;
		}
		cl = row[x];
		if (cl.attr & VT_WCONT)
			continue;	/* gia' disegnata con la prima meta' */
		if ((cl.attr & VT_WIDE) && x + 1 < n)
			w = 2;
		fg = cell_fg(&cl);
		bg = cell_bg(&cl);
		if (cl.attr & VT_REVERSE) {
			bui_px t = fg;

			fg = bg;
			bg = t;
		}
		if (cur && (x == T.dcx || (w == 2 && x + 1 == T.dcx))) {
			fg = T.cur_fg;
			bg = T.cur_bg;
		}
		if (cl.attr & VT_DIM)
			fg = bui_mix(bg, fg, 150);
		if (cl.attr & VT_INVIS)
			fg = bg;
		bui_fill(c, bui_r(ox, oy, w * CW, CH), bg);
		draw_glyph(c, ox, oy, w, &cl, fg, bg);
		if (cl.attr & VT_UNDER)
			bui_fill(c, bui_r(ox, oy + BASE + 2, w * CW, 1), fg);
		if (cl.attr & VT_STRIKE)
			bui_fill(c, bui_r(ox, oy + BASE - 4, w * CW, 1), fg);
		if (w == 2)
			x++;
	}
}

/* tutto cio' che del testo cade in r (coordinate della finestra) */
static void draw_text(bui_canvas *c, bui_rect r)
{
	int r0, r1, c0, c1, y;
	bui_rect grid = bui_r(T.gx, T.gy, T.vt.cols * CW, T.vt.rows * CH), k;

	/* i margini attorno alla griglia */
	bui_fill(c, r, T.bg);
	if (!T.vt_ok || !bui_rect_isect(r, grid, &k))
		return;
	r0 = (k.y - T.gy) / CH;
	r1 = (k.y + k.h - 1 - T.gy) / CH;
	c0 = (k.x - T.gx) / CW;
	c1 = (k.x + k.w - 1 - T.gx) / CW;
	for (y = r0; y <= r1; y++)
		draw_row(c, y, c0, c1);
	if (T.view_off > 0) {
		/* nello storico: una riga d'accento in fondo e quante righe mancano */
		char s[48];
		int w;

		snprintf(s, sizeof(s), T("storico \xE2\x88\x92%d"), T.view_off);
		w = bui_text_width(BUI_FONT_SMALL_B, s, -1) + 20;
		bui_fill(c, bui_r(T.text.x, T.text.y + T.text.h - 2, T.text.w, 2), BUI_C_ACCENT);
		bui_rrect(c, bui_r(T.text.x + T.text.w - w - 8, T.text.y + T.text.h - 30, w, 24), 12, BUI_C_ACCENT);
		bui_text_box(c, BUI_FONT_SMALL_B, bui_r(T.text.x + T.text.w - w - 8, T.text.y + T.text.h - 30, w, 24),
			     s, BUI_C_ON_ACCENT, BUI_AL_CENTER);
	}
}

static void damage_text(void)
{
	if (bs_view_is_active(&bs_view_term))
		bs_view_damage(T.text);
}

/* ------------------------------------------------------------------ */
/* sincronizzazione: dalle celle cambiate alla tela                   */
/* ------------------------------------------------------------------ */

static void dump_screen(void)
{
	FILE *f;
	char tmp[600], line[2048];
	int y, x, n;

	if (!T.dump || !T.vt_ok)
		return;
	snprintf(tmp, sizeof(tmp), "%s.tmp", T.dump);
	f = fopen(tmp, "w");
	if (!f)
		return;
	for (y = 0; y < T.vt.rows; y++) {
		const vt_cell *r = vt_row(&T.vt, y, &n);
		int len = 0;

		for (x = 0; x < n && len < (int)sizeof(line) - 8; x++) {
			if (r[x].attr & VT_WCONT)
				continue;
			len += vt_char(r[x].cp, 0, line + len);
		}
		while (len > 0 && line[len - 1] == ' ')
			len--;
		line[len] = 0;
		fprintf(f, "%s\n", line);
	}
	fprintf(f, "cursore %d %d\n", T.vt.c.x, T.vt.c.y);
	fclose(f);
	rename(tmp, T.dump);
}

/* la tela sotto il testo e' quella che abbiamo disegnato? (nessun rettangolo
 * della finestra in attesa di ridisegno, nessuna transizione) */
static int canvas_ours(void)
{
	int i;

	if (!bs_view_settled(&bs_view_term))
		return 0;
	for (i = 0; i < T.win->ndirty; i++) {
		bui_rect k;

		if (bui_rect_isect(T.win->dirty[i], T.text, &k))
			return 0;
	}
	return 1;
}

static void move_rows(int top, int bot, int n)
{
	bui_canvas *c = &T.win->cv;
	int h = (bot - top + 1 - (n > 0 ? n : -n)) * CH, x = T.gx, w = T.vt.cols * CW, y;
	int y0 = T.gy + top * CH;

	if (h <= 0)
		return;
	if (n > 0) {
		for (y = 0; y < h; y++)
			memmove(c->px + (size_t)(y0 + y) * c->stride + x,
				c->px + (size_t)(y0 + y + n * CH) * c->stride + x, (size_t)w * sizeof(bui_px));
	} else {
		for (y = h - 1; y >= 0; y--)
			memmove(c->px + (size_t)(y0 + y - n * CH) * c->stride + x,
				c->px + (size_t)(y0 + y) * c->stride + x, (size_t)w * sizeof(bui_px));
	}
	bui_win_present(T.win, bui_r(x, y0, w, (bot - top + 1) * CH));
}

/* il cursore che draw() disegna diventa quello di adesso (senza segnare nulla) */
static void cursor_take(void)
{
	T.don = T.alive && T.vt.m_cursor;
	T.dcx = T.vt.c.x;
	T.dcy = T.vt.c.y;
}

static void sync_screen(void)
{
	vt *t = &T.vt;
	int y, direct, on;

	T.render_timer = 0;
	T.last_render = bui_now();
	if (!T.vt_ok || !bs_view_is_active(&bs_view_term))
		return;		/* le celle restano segnate: si disegna al rientro */
	if (T.view_off > 0) {
		/* nello storico: la vista resta ferma sulle stesse righe (che salgono
		 * nello storico man mano che il programma scrive) */
		int changed = t->sb_pushed != T.seen_pushed;

		if (changed) {
			T.view_off += (int)(t->sb_pushed - T.seen_pushed);
			if (T.view_off > t->sb_count)
				T.view_off = t->sb_count;
			T.seen_pushed = t->sb_pushed;
		}
		for (y = 0; y < t->rows && !changed; y++)
			if (t->dirty[y] && y + T.view_off < t->rows)
				changed = 1;	/* righe dello schermo ancora in vista */
		if (changed)
			damage_text();
		vt_clean(t);
		dump_screen();
		return;
	}
	/* il cursore si e' mosso: le due celle */
	on = T.alive && t->m_cursor;
	if (on != T.don || t->c.x != T.dcx || t->c.y != T.dcy) {
		int oy = T.dcy;

		/* il vecchio cursore e' nella tela: se le righe stanno per essere
		 * spostate, si sposta con loro (o esce dalla regione) */
		if (t->sc_n && oy >= t->sc_top && oy <= t->sc_bot) {
			oy -= t->sc_n;
			if (oy < t->sc_top || oy > t->sc_bot)
				oy = -1;
		}
		if (T.don && oy >= 0)
			vt_mark(t, oy, T.dcx - 1, T.dcx + 1);
		T.don = on;
		T.dcx = t->c.x;
		T.dcy = t->c.y;
		if (on)
			vt_mark(t, T.dcy, T.dcx - 1, T.dcx + 1);
	}
	if (t->all_dirty) {
		cursor_take();
		damage_text();
		vt_clean(t);
		dump_screen();
		return;
	}
	direct = canvas_ours();
	if (t->sc_n) {
		if (direct)
			move_rows(t->sc_top, t->sc_bot, t->sc_n);
		else
			vt_flush_scroll(t);
	}
	for (y = 0; y < t->rows; y++) {
		bui_rect r;

		if (!t->dirty[y])
			continue;
		r = bui_r(T.gx + t->dlo[y] * CW, T.gy + y * CH, (t->dhi[y] - t->dlo[y] + 1) * CW, CH);
		if (direct) {
			bui_canvas *c = &T.win->cv;
			bui_rect saved = bui_clip_push(c, r);

			draw_row(c, y, t->dlo[y], t->dhi[y]);
			bui_clip_pop(c, saved);
			bui_win_present(T.win, r);
		} else {
			bs_view_damage(r);
		}
	}
	vt_clean(t);
	dump_screen();
}

static void render_fire(void *user)
{
	(void)user;
	T.render_timer = 0;
	sync_screen();
}

/* un fotogramma al massimo ogni RENDER_MS mentre l'uscita scorre */
static void schedule_render(void)
{
	int d;

	if (T.render_timer)
		return;
	d = (int)(T.last_render + RENDER_MS - bui_now());
	if (d < 0 || d > RENDER_MS)
		d = 0;
	T.render_timer = bui_timer_add(d, 0, render_fire, NULL);
}

/* ------------------------------------------------------------------ */
/* PTY e processo                                                     */
/* ------------------------------------------------------------------ */

static void pty_write(const char *s, int n)
{
	while (n > 0 && T.pty >= 0 && T.alive) {
		ssize_t w = write(T.pty, s, (size_t)n);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			break;		/* coda piena: il resto si perde, come una tastiera troppo veloce */
		s += w;
		n -= (int)w;
	}
}

static void on_reply(void *user, const char *s, int n)
{
	(void)user;
	pty_write(s, n);
}

static void on_title(void *user, const char *s)
{
	int i, j = 0;

	(void)user;
	/* solo testo stampabile; i caratteri senza glifo li salta il disegno */
	for (i = 0; s[i] && j < (int)sizeof(T.title) - 1; i++)
		T.title[j++] = (unsigned char)s[i] < 0x20 ? ' ' : s[i];
	T.title[j] = 0;
	if (T.w_title && bs_view_is_active(&bs_view_term))
		bui_widget_damage(T.w_title);
}

static void set_winsize(void)
{
	struct winsize ws;

	if (T.pty < 0 || !T.vt_ok)
		return;
	memset(&ws, 0, sizeof(ws));
	ws.ws_col = (unsigned short)T.vt.cols;
	ws.ws_row = (unsigned short)T.vt.rows;
	ws.ws_xpixel = (unsigned short)(T.vt.cols * CW);
	ws.ws_ypixel = (unsigned short)(T.vt.rows * CH);
	ioctl(T.pty, TIOCSWINSZ, &ws);
}

static void pty_close(void)
{
	if (T.pty < 0)
		return;
	bui_fd_del(T.pty);
	close(T.pty);
	T.pty = -1;
}

static void feed(void)
{
	uint8_t buf[8192];
	int total = 0;

	while (T.pty >= 0 && total < 65536) {
		ssize_t n = read(T.pty, buf, sizeof(buf));

		if (n > 0) {
			vt_write(&T.vt, buf, (size_t)n);
			total += (int)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		/* EIO o fine: il lato slave e' chiuso (il processo e' uscito) */
		bui_fd_del(T.pty);
		break;
	}
	if (total) {
		uint32_t now = bui_now();
		pid_t fg = T.pty >= 0 ? tcgetpgrp(T.pty) : -1;

		/* un programma che lavora: in primo piano al posto della shell, oppure
		 * uscita che non e' l'eco di un tasto appena premuto */
		if ((fg > 0 && fg != T.pid && (int32_t)(now - T.last_key) > 300) ||
		    (int32_t)(now - T.last_key) > 1000)
			T.last_fg_output = now ? now : 1;
		schedule_render();
	}
}

static void pty_readable(int fd, void *user)
{
	(void)fd;
	(void)user;
	feed();
}

static void finished(int status, void *user)
{
	char msg[96];

	(void)user;
	feed();		/* l'ultima uscita */
	pty_close();
	T.alive = 0;
	T.pid = 0;
	T.exited = 1;
	T.last_fg_output = 0;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
		snprintf(msg, sizeof(msg), "\r\n\033[0;1;31m[impossibile avviare la shell]\033[0m");
	else
		snprintf(msg, sizeof(msg), "\r\n\033[0;2m[processo terminato]\033[0m  Invio per riavviare");
	vt_puts(&T.vt, msg);
	if (bs_view_is_active(&bs_view_term)) {
		rebuild();
		bui_panel_damage(&T.p);
		schedule_render();
	}
}

static const char *pick_term(void)
{
	static const char *const dirs[] = { "/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo",
					    "/usr/lib/terminfo" };
	char path[128];
	unsigned i;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/x/xterm-256color", dirs[i]);
		if (access(path, R_OK) == 0)
			return "xterm-256color";
	}
	return "xterm";
}

static void child(const char *slave)
{
	const char *sh = getenv("BRAINSHELL_TERM_SHELL"), *home = getenv("HOME"), *base;
	struct sigaction sa;
	sigset_t none;
	char arg0[64];
	int s, fd;

	setsid();
	s = open(slave, O_RDWR);
	if (s < 0)
		_exit(127);
	ioctl(s, TIOCSCTTY, 0);
	dup2(s, 0);
	dup2(s, 1);
	dup2(s, 2);
	for (fd = 3; fd < 1024; fd++)
		close(fd);
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	for (fd = 1; fd < 32; fd++)
		sigaction(fd, &sa, NULL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setenv("TERM", pick_term(), 1);
	setenv("COLORTERM", "truecolor", 1);
	unsetenv("COLUMNS");
	unsetenv("LINES");
	if (!getenv("LANG"))
		setenv("LANG", "C.UTF-8", 1);
	if (home && chdir(home) < 0) {
		/* si resta dove si e' */
	}
	if (!sh || !*sh) {
		sh = getenv("SHELL");
		if (!sh || sh[0] != '/' || access(sh, X_OK) < 0)
			sh = access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
	}
	/* shell di login: argv[0] con il trattino davanti */
	base = strrchr(sh, '/');
	snprintf(arg0, sizeof(arg0), "-%s", base ? base + 1 : sh);
	execl(sh, arg0, (char *)NULL);
	execl("/bin/sh", "-sh", (char *)NULL);
	_exit(127);
}

static int spawn(void)
{
	char slave[128];
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	pid_t pid;

	if (m < 0)
		goto fail;
	if (grantpt(m) < 0 || unlockpt(m) < 0 || ptsname_r(m, slave, sizeof(slave)) != 0) {
		close(m);
		goto fail;
	}
	fcntl(m, F_SETFD, FD_CLOEXEC);
	fcntl(m, F_SETFL, O_NONBLOCK);
	T.pty = m;
	set_winsize();
	pid = fork();
	if (pid < 0) {
		pty_close();
		goto fail;
	}
	if (pid == 0)
		child(slave);
	T.pid = pid;
	T.alive = 1;
	T.exited = 0;
	T.started = 1;
	bui_fd_add(m, pty_readable, NULL);
	bs_child_watch(pid, finished, NULL);
	return 0;
fail:
	vt_puts(&T.vt, "\033[0;1;31m[impossibile aprire un terminale]\033[0m\r\n");
	T.started = 1;
	T.exited = 1;
	return -1;
}

static void restart(void)
{
	if (T.alive)
		return;
	vt_puts(&T.vt, "\033[0m\r\n");
	spawn();
	rebuild();
	bui_panel_damage(&T.p);
	schedule_render();
}

int bs_term_keep_awake(void)
{
	return T.alive && T.last_fg_output && (int32_t)(bui_now() - T.last_fg_output) < T.awake_ms;
}

const char *bs_term_state(void)
{
	return T.alive ? "running" : T.exited ? "exited" : "none";
}

/* ------------------------------------------------------------------ */
/* ingresso                                                           */
/* ------------------------------------------------------------------ */

static void set_view_off(int n)
{
	if (n < 0)
		n = 0;
	if (n > T.vt.sb_count)
		n = T.vt.sb_count;
	if (n == T.view_off)
		return;
	if (!T.view_off)
		T.seen_pushed = T.vt.sb_pushed;
	T.view_off = n;
	/* tutto il testo si ridisegna dalle celle: niente da spostare o da segnare */
	cursor_take();
	vt_clean(&T.vt);
	damage_text();
}

static void ctrl_set(int on)
{
	T.ctrl = on;
	if (T.w_ctrl)
		bui_widget_set_flag(T.w_ctrl, BUI_F_SELECTED, on);
}

static int vt_mods(unsigned m)
{
	return ((m & BUI_MOD_SHIFT) ? VT_MOD_SHIFT : 0) | ((m & BUI_MOD_ALT) ? VT_MOD_ALT : 0) |
	       ((m & BUI_MOD_CTRL) ? VT_MOD_CTRL : 0);
}

/* un tasto (della tastiera o della barra) al programma */
static void send_key(int key, unsigned mods)
{
	char buf[24];
	int n = 0, vk = 0, m;

	if (T.ctrl) {
		mods |= BUI_MOD_CTRL;
		ctrl_set(0);
	}
	m = vt_mods(mods);
	switch (key) {
	case BUI_K_UP: vk = VT_K_UP; break;
	case BUI_K_DOWN: vk = VT_K_DOWN; break;
	case BUI_K_LEFT: vk = VT_K_LEFT; break;
	case BUI_K_RIGHT: vk = VT_K_RIGHT; break;
	case BUI_K_HOME: vk = VT_K_HOME; break;
	case BUI_K_END: vk = VT_K_END; break;
	case BUI_K_INSERT: vk = VT_K_INSERT; break;
	case BUI_K_DELETE: vk = VT_K_DELETE; break;
	case BUI_K_PGUP: vk = VT_K_PGUP; break;
	case BUI_K_PGDN: vk = VT_K_PGDN; break;
	case BUI_K_BACKSPACE: vk = VT_K_BACKSPACE; break;
	case BUI_K_TAB: vk = VT_K_TAB; break;
	case BUI_K_ENTER: vk = VT_K_ENTER; break;
	case BUI_K_ESC: vk = VT_K_ESC; break;
	default: break;
	}
	if (vk)
		n = vt_key(&T.vt, vk, m, buf);
	else if (key >= 0 && key < 0x110000)
		n = vt_char((uint32_t)key, m, buf);
	if (n <= 0)
		return;
	set_view_off(0);	/* un tasto riporta in fondo */
	T.last_key = bui_now();
	pty_write(buf, n);
}

static void on_tool(bui_widget *w)
{
	static const int keys[NKEYS] = { BUI_K_ESC, BUI_K_TAB, 0, BUI_K_UP, BUI_K_DOWN, BUI_K_LEFT,
					 BUI_K_RIGHT, '|', '~', '/', '-' };

	if (w->id == TK_CTRL) {
		ctrl_set(!T.ctrl);
		return;
	}
	send_key(keys[w->id], 0);
}

static void on_back(bui_widget *w)
{
	(void)w;
	bs_view_close();
}

static void on_restart(bui_widget *w)
{
	(void)w;
	restart();
}

static void x_done(int button, void *user)
{
	const bs_app *a = user;

	if (button == 1 && a && a->argv && bs_launch(a->argv, a->label) == 0)
		bui_toast("Apertura del terminale X\xE2\x80\xA6", 1800);
}

static void long_fire(void *user)
{
	static const bui_dialog d = {
		.title = "Aprire in X?",
		.message = "Il terminale st in una sessione X a tutto schermo (lento ad aprirsi). "
			   "Questa sessione resta aperta.",
		.buttons = { "Annulla", "Apri in X" },
		.styles = { BUI_BS_TONAL, BUI_BS_FILLED },
		.cancel = 0,
		.done = x_done,
	};
	bui_dialog dd = d;

	(void)user;
	T.lp_timer = 0;
	if (T.drag || !bs_view_is_active(&bs_view_term))
		return;
	T.drag = 2;	/* il resto del tocco non scorre */
	dd.user = (void *)bs_view_term.app;
	bui_dialog_open(&dd);
}

static int fling_step(void *user, uint32_t now)
{
	int max = T.vt.sb_count * CH;

	(void)user;
	if (!T.fling)
		return 0;
	if (bui_scroll_step(&T.sc, now))
		set_view_off((max - T.sc.pos + CH / 2) / CH);
	T.fling = T.sc.fling;
	return T.fling;
}

static int tm_pointer(bs_view *v, const bui_pointer *ev)
{
	int max = T.vt.sb_count * CH;

	(void)v;
	if (ev->type == BUI_PRESS || ev->type == BUI_WHEEL)
		T.cap_panel = !bui_rect_has(T.text, ev->x, ev->y);
	if (T.cap_panel)
		return bui_panel_pointer(&T.p, ev);
	switch (ev->type) {
	case BUI_PRESS:
		T.fling = 0;
		bui_anim_del(fling_step, NULL);
		T.drag = 0;
		T.alt_acc = 0;
		T.press_x = ev->x;
		T.press_y = ev->y;
		T.sc.max = max;
		T.sc.pos = max - T.view_off * CH;
		T.sc.elastic = 0;
		bui_scroll_grab(&T.sc, ev->y, ev->time);
		bui_timer_del(T.lp_timer);
		T.lp_timer = bui_timer_add(LONG_MS, 0, long_fire, NULL);
		break;
	case BUI_MOVE:
		if (T.drag == 2)
			break;
		if (!T.drag && (abs(ev->y - T.press_y) > BUI_DRAG_SLOP || abs(ev->x - T.press_x) > BUI_DRAG_SLOP)) {
			T.drag = 1;
			bui_timer_del(T.lp_timer);
			T.lp_timer = 0;
			T.alt_acc = T.press_y;
		}
		if (T.drag != 1)
			break;
		if (T.vt.alt) {
			/* less, vim, man: il dito che sale manda giu' (come lo scorrimento) */
			while (ev->y - T.alt_acc >= CH) {
				send_key(BUI_K_UP, 0);
				T.alt_acc += CH;
			}
			while (T.alt_acc - ev->y >= CH) {
				send_key(BUI_K_DOWN, 0);
				T.alt_acc -= CH;
			}
		} else if (bui_scroll_drag(&T.sc, ev->y, ev->time)) {
			set_view_off((max - T.sc.pos + CH / 2) / CH);
		}
		break;
	case BUI_RELEASE:
		bui_timer_del(T.lp_timer);
		T.lp_timer = 0;
		if (T.drag == 1 && !T.vt.alt && bui_scroll_release(&T.sc, ev->time)) {
			T.fling = 1;
			bui_anim_add(fling_step, NULL);
		}
		T.drag = 0;
		break;
	case BUI_CANCEL:
		bui_timer_del(T.lp_timer);
		T.lp_timer = 0;
		T.drag = 0;
		break;
	case BUI_WHEEL:
		if (T.vt.alt)
			send_key(ev->dy < 0 ? BUI_K_UP : BUI_K_DOWN, 0);
		else
			set_view_off(T.view_off - ev->dy * 3);
		break;
	}
	return 1;
}

static int tm_key(bs_view *v, const bui_key *k)
{
	(void)v;
	if (!T.alive) {
		if (k->key == BUI_K_ENTER) {
			restart();
			return 1;
		}
		return 0;	/* Esc / Backspace chiudono la vista come altrove */
	}
	/* Maiusc+PagSu/PagGiu': lo storico, come in xterm */
	if ((k->mods & BUI_MOD_SHIFT) && !T.vt.alt && (k->key == BUI_K_PGUP || k->key == BUI_K_PGDN)) {
		set_view_off(T.view_off + (k->key == BUI_K_PGUP ? 1 : -1) * (T.vt.rows - 1));
		return 1;
	}
	send_key(k->key, k->mods);
	return 1;
}

static int tm_text_input(bs_view *v)
{
	(void)v;
	return T.alive;		/* i tasti del dizionario vanno al programma */
}

/* ------------------------------------------------------------------ */
/* vista                                                              */
/* ------------------------------------------------------------------ */

/* tasto della barra: come un pulsante, ma il testo usa tutta la larghezza
 * (in verticale i tasti sono larghi 41 px) */
static void draw_tool(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int sel = w->flags & BUI_F_SELECTED, rad = r.h / 2 < bui_th.r_btn ? r.h / 2 : bui_th.r_btn;
	bui_px bg = (w->flags & BUI_F_PRESSED) ? BUI_C_SURFACE3 : sel ? BUI_C_ACCENT_BG : BUI_C_SURFACE2;
	const bui_font *f = w->id >= TK_UP && w->id <= TK_RIGHT ? BUI_FONT_BODY_B :
			    r.w < 52 ? BUI_FONT_SMALL : BUI_FONT_SMALL_B;

	bui_rrect(c, r, rad, bg);
	bui_text_box(c, f, r, w->text, sel ? BUI_C_ACCENT : BUI_C_TEXT, BUI_AL_CENTER);
}

static void draw_title(bui_widget *w, bui_canvas *c, bui_rect r)
{
	(void)w;
	bui_text_box(c, BUI_FONT_TITLE, r, T.title[0] ? T.title : "Terminale", BUI_C_TEXT, BUI_AL_LEFT);
}

/* intestazione con la freccia e il titolo; i tasti a destra (orizzontale)
 * o su una riga loro (verticale) */
static void rebuild(void)
{
	bui_panel *p = &T.p;
	bui_rect a = T.area;
	int wide = a.w >= 640, vp_h = wide ? HEAD_H : HEAD_H + BAR_H;
	int kw, kx, ky, kh, i, tw;
	bui_widget *w;

	bui_panel_clear(p);
	T.w_title = T.w_ctrl = NULL;
	bui_panel_set_viewport(p, bui_r(a.x, a.y, a.w, vp_h), vp_h);
	p->no_scrollbar = 1;
	w = bui_add_button(p, bui_r(0, 0, 64, HEAD_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND, on_back);
	w->flags |= BUI_F_NOFOCUS;
	if (wide) {
		kw = 56;
		kh = 44;
		kx = a.w - 8 - NKEYS * (kw + 2) + 2;
		ky = (HEAD_H - kh) / 2;
	} else {
		kw = (a.w - 8 - (NKEYS - 1) * 2) / NKEYS;
		kh = BAR_H - 6;
		kx = 4;
		ky = HEAD_H + 2;
	}
	tw = (wide ? kx - 8 : a.w - 8) - 68;
	if (!T.alive) {
		/* processo finito: al posto dei tasti, "Riavvia" */
		int bw = 160;

		w = bui_add_button(p, bui_r(a.w - 8 - bw, (HEAD_H - 44) / 2, bw, 44), "Riavvia",
				   BUI_IC(restart, 24), BUI_BS_FILLED, on_restart);
		w->flags |= BUI_F_NOFOCUS;
		tw = a.w - 8 - bw - 8 - 68;
	} else {
		for (i = 0; i < NKEYS; i++) {
			w = bui_add_custom(p, bui_r(kx + i * (kw + 2), ky, kw, kh), draw_tool);
			w->text = key_labels[i];
			w->on_click = on_tool;
			w->id = i;
			w->flags |= BUI_F_NOFOCUS;
			if (i == TK_CTRL) {
				T.w_ctrl = w;
				if (T.ctrl)
					w->flags |= BUI_F_SELECTED;
			}
		}
	}
	T.w_title = bui_add_custom(p, bui_r(68, 0, tw, HEAD_H), draw_title);
	T.w_title->flags |= BUI_F_NOFOCUS;
}

static void tm_layout(bs_view *v, bui_rect a)
{
	int wide = a.w >= 640, top = wide ? HEAD_H : HEAD_H + BAR_H, cols, rows;

	(void)v;
	T.area = a;
	palette();
	T.text = bui_r(a.x, a.y + top, a.w, a.h - top);
	cols = T.text.w / CW;
	rows = T.text.h / CH;
	if (cols < 10)
		cols = 10;
	if (rows < 4)
		rows = 4;
	T.gx = T.text.x + (T.text.w - cols * CW) / 2;
	T.gy = T.text.y + (T.text.h - rows * CH) / 2;
	if (!T.vt_ok) {
		if (vt_init(&T.vt, cols, rows, SB_LINES) == 0) {
			T.vt_ok = 1;
			T.vt.reply = on_reply;
			T.vt.on_title = on_title;
		}
	} else if (cols != T.vt.cols || rows != T.vt.rows) {
		vt_resize(&T.vt, cols, rows);
		T.view_off = 0;
	}
	if (T.vt_ok && !T.started)
		spawn();
	set_winsize();
	rebuild();
	if (T.vt_ok)
		vt_damage_all(&T.vt);
	schedule_render();
}

static void tm_enter(bs_view *v)
{
	T.win = v->win;
	bui_panel_init(&T.p, v->win);
	T.dump = getenv("BRAINSHELL_TERM_DUMP");
	/* BRAINSHELL_TERM_AWAKE_MS: al posto dei 30 s, solo per le prove */
	T.awake_ms = getenv("BRAINSHELL_TERM_AWAKE_MS") ? atoi(getenv("BRAINSHELL_TERM_AWAKE_MS")) : AWAKE_MS;
	T.view_off = 0;
	T.drag = 0;
}

static void tm_leave(bs_view *v)
{
	(void)v;
	/* la sessione resta: il PTY continua a essere letto in sottofondo */
	bui_timer_del(T.render_timer);
	bui_timer_del(T.lp_timer);
	T.render_timer = T.lp_timer = 0;
	T.fling = 0;
	bui_anim_del(fling_step, NULL);
	ctrl_set(0);
	bui_panel_clear(&T.p);
	T.w_title = T.w_ctrl = NULL;
	T.view_off = 0;
}

static void tm_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	bui_rect k;

	(void)v;
	bui_panel_draw(&T.p, c, clip);
	if (bui_rect_isect(clip, T.text, &k)) {
		/* questi pixel vengono dalle celle di adesso: uno scorrimento ancora
		 * in attesa non va piu' applicato a loro */
		if (T.vt_ok)
			vt_flush_scroll(&T.vt);
		draw_text(c, k);
	}
}

bs_view bs_view_term = {
	.flags = BS_VIEW_NO_HEADER,
	.enter = tm_enter,
	.leave = tm_leave,
	.layout = tm_layout,
	.draw = tm_draw,
	.pointer = tm_pointer,
	.key = tm_key,
	.text_input = tm_text_input,
};
