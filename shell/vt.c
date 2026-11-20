// SPDX-License-Identifier: MIT
/*
 * vt.c - emulatore VT100/xterm (vedi vt.h). Solo interi.
 *
 * Lo schermo e' un array di puntatori a righe: scorrere vuol dire ruotare
 * puntatori, non copiare celle. Lo storico e' un anello di righe copiate.
 * Ogni modifica annota le colonne cambiate della riga (dirty/dlo/dhi);
 * gli scorrimenti consecutivi della stessa regione si sommano in uno solo
 * (sc_n), cosi' chi disegna sposta i pixel una volta sola per fotogramma.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vt.h"

enum { S_GROUND, S_ESC, S_ESC_INT, S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC };

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ------------------------------------------------------------------ */
/* larghezza dei caratteri                                            */
/* ------------------------------------------------------------------ */

static const uint32_t zero_w[][2] = {
	{ 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD }, { 0x0610, 0x061A },
	{ 0x064B, 0x065F }, { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
	{ 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F }, { 0x2028, 0x202E },
	{ 0x2060, 0x2064 }, { 0x20D0, 0x20FF }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F },
	{ 0xFEFF, 0xFEFF }, { 0x1F3FB, 0x1F3FF }, { 0xE0000, 0xE0FFF },
};

static const uint32_t wide[][2] = {
	{ 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x23E9, 0x23EC },
	{ 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE }, { 0x2614, 0x2615 },
	{ 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
	{ 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE },
	{ 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA }, { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 },
	{ 0x26FA, 0x26FA }, { 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B },
	{ 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E }, { 0x2753, 0x2755 },
	{ 0x2757, 0x2757 }, { 0x2795, 0x2797 }, { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF },
	{ 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0x2E80, 0x303E },
	{ 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF }, { 0xA000, 0xA4CF },
	{ 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 },
	{ 0xFE30, 0xFE6F }, { 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 }, { 0x16FE0, 0x16FE4 },
	{ 0x17000, 0x18CFF }, { 0x1B000, 0x1B2FF }, { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF },
	{ 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A }, { 0x1F200, 0x1F251 }, { 0x1F300, 0x1F320 },
	{ 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA },
	{ 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 }, { 0x1F3F8, 0x1F43E },
	{ 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC }, { 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E },
	{ 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 }, { 0x1F5A4, 0x1F5A4 },
	{ 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC }, { 0x1F6D0, 0x1F6D2 },
	{ 0x1F6D5, 0x1F6D7 }, { 0x1F6DC, 0x1F6DF }, { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6FC },
	{ 0x1F7E0, 0x1F7EB }, { 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A }, { 0x1F93C, 0x1F945 },
	{ 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FAFF }, { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};

static int in_table(uint32_t cp, const uint32_t (*tab)[2], int n)
{
	int lo = 0, hi = n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) >> 1;

		if (cp < tab[mid][0])
			hi = mid - 1;
		else if (cp > tab[mid][1])
			lo = mid + 1;
		else
			return 1;
	}
	return 0;
}

int vt_wcwidth(uint32_t cp)
{
	if (cp < 0x300)
		return 1;
	if (in_table(cp, zero_w, (int)(sizeof(zero_w) / sizeof(zero_w[0]))))
		return 0;
	if (cp >= 0x1100 && in_table(cp, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
		return 2;
	return 1;
}

/* ------------------------------------------------------------------ */
/* celle, righe cambiate                                              */
/* ------------------------------------------------------------------ */

/* cella cancellata: colore di fondo corrente (bce), nessun attributo */
static vt_cell blank(const vt *t)
{
	vt_cell b;

	b.cp = ' ';
	b.fg = VT_DEFAULT;
	b.bg = t->c.pen.bg;
	b.attr = t->c.pen.attr & VT_BG_RGB;
	b.pad = 0;
	return b;
}

static void fill(vt_cell *p, int n, vt_cell b)
{
	while (n-- > 0)
		*p++ = b;
}

static void mark(vt *t, int y, int x0, int x1)
{
	if (y < 0 || y >= t->rows)
		return;
	x0 = MAX(x0, 0);
	x1 = MIN(x1, t->cols - 1);
	if (x0 > x1)
		return;
	if (!t->dirty[y]) {
		t->dirty[y] = 1;
		t->dlo[y] = (int16_t)x0;
		t->dhi[y] = (int16_t)x1;
		return;
	}
	if (x0 < t->dlo[y])
		t->dlo[y] = (int16_t)x0;
	if (x1 > t->dhi[y])
		t->dhi[y] = (int16_t)x1;
}

static void mark_rows(vt *t, int y0, int y1)
{
	int y;

	for (y = MAX(y0, 0); y <= y1 && y < t->rows; y++) {
		t->dirty[y] = 1;
		t->dlo[y] = 0;
		t->dhi[y] = (int16_t)(t->cols - 1);
	}
}

void vt_clean(vt *t)
{
	memset(t->dirty, 0, (size_t)t->rows);
	t->all_dirty = 0;
	t->sc_n = 0;
}

void vt_flush_scroll(vt *t)
{
	if (t->sc_n)
		mark_rows(t, t->sc_top, t->sc_bot);
	t->sc_n = 0;
}

void vt_mark(vt *t, int y, int x0, int x1)
{
	mark(t, y, x0, x1);
}

void vt_damage_all(vt *t)
{
	t->all_dirty = 1;
	t->sc_n = 0;
	mark_rows(t, 0, t->rows - 1);
}

/*
 * La regione [top, bot] e' scorsa di n righe (n > 0 in su). I segni delle
 * righe si spostano con le righe; quelle nuove sono tutte da disegnare.
 */
static void note_scroll(vt *t, int top, int bot, int n)
{
	int h = bot - top + 1, y;

	if (t->all_dirty)
		return;
	if (t->sc_n && (t->sc_top != top || t->sc_bot != bot)) {
		/* un'altra regione: lo scorrimento in attesa diventa un ridisegno */
		mark_rows(t, t->sc_top, t->sc_bot);
		t->sc_n = 0;
	}
	if (n > 0) {
		for (y = top; y <= bot - n; y++) {
			t->dirty[y] = t->dirty[y + n];
			t->dlo[y] = t->dlo[y + n];
			t->dhi[y] = t->dhi[y + n];
		}
		mark_rows(t, bot - n + 1, bot);
	} else {
		for (y = bot; y >= top - n; y--) {
			t->dirty[y] = t->dirty[y + n];
			t->dlo[y] = t->dlo[y + n];
			t->dhi[y] = t->dhi[y + n];
		}
		mark_rows(t, top, top - n - 1);
	}
	t->sc_top = top;
	t->sc_bot = bot;
	t->sc_n += n;
	if (t->sc_n >= h || t->sc_n <= -h) {
		mark_rows(t, top, bot);
		t->sc_n = 0;
	}
}

/* ------------------------------------------------------------------ */
/* storico                                                            */
/* ------------------------------------------------------------------ */

static void sb_push(vt *t, const vt_cell *row)
{
	vt_cell *dst;
	int n = MIN(t->cols, t->sb_cols);

	if (!t->sb_cap)
		return;
	dst = t->sb + (size_t)t->sb_head * t->sb_cols;
	memcpy(dst, row, (size_t)n * sizeof(vt_cell));
	if (n < t->sb_cols) {
		vt_cell b = { ' ', VT_DEFAULT, VT_DEFAULT, 0, 0 };

		fill(dst + n, t->sb_cols - n, b);
	}
	t->sb_head = (t->sb_head + 1) % t->sb_cap;
	if (t->sb_count < t->sb_cap)
		t->sb_count++;
	t->sb_pushed++;
}

const vt_cell *vt_row(const vt *t, int y, int *n)
{
	if (y >= 0) {
		if (n)
			*n = t->cols;
		return y < t->rows ? t->line[y] : NULL;
	}
	if (-y > t->sb_count)
		return NULL;
	if (n)
		*n = t->sb_cols;
	return t->sb + (size_t)((t->sb_head + y + t->sb_cap) % t->sb_cap) * t->sb_cols;
}

/* ------------------------------------------------------------------ */
/* scorrimento                                                        */
/* ------------------------------------------------------------------ */

static void scroll_up(vt *t, int top, int bot, int n, int save)
{
	vt_cell *tmp[8], **keep;
	vt_cell b = blank(t);
	int h = bot - top + 1, i;

	if (n <= 0 || top < 0 || bot >= t->rows || top > bot)
		return;
	n = MIN(n, h);
	if (save && !t->alt && top == 0)
		for (i = 0; i < n; i++)
			sb_push(t, t->line[top + i]);
	keep = n <= 8 ? tmp : malloc((size_t)n * sizeof(*keep));
	if (!keep)
		return;
	memcpy(keep, &t->line[top], (size_t)n * sizeof(*keep));
	memmove(&t->line[top], &t->line[top + n], (size_t)(h - n) * sizeof(*keep));
	memcpy(&t->line[bot - n + 1], keep, (size_t)n * sizeof(*keep));
	for (i = bot - n + 1; i <= bot; i++)
		fill(t->line[i], t->cols, b);
	if (keep != tmp)
		free(keep);
	note_scroll(t, top, bot, n);
}

static void scroll_down(vt *t, int top, int bot, int n)
{
	vt_cell *tmp[8], **keep;
	vt_cell b = blank(t);
	int h = bot - top + 1, i;

	if (n <= 0 || top < 0 || bot >= t->rows || top > bot)
		return;
	n = MIN(n, h);
	keep = n <= 8 ? tmp : malloc((size_t)n * sizeof(*keep));
	if (!keep)
		return;
	memcpy(keep, &t->line[bot - n + 1], (size_t)n * sizeof(*keep));
	memmove(&t->line[top + n], &t->line[top], (size_t)(h - n) * sizeof(*keep));
	memcpy(&t->line[top], keep, (size_t)n * sizeof(*keep));
	for (i = top; i < top + n; i++)
		fill(t->line[i], t->cols, b);
	if (keep != tmp)
		free(keep);
	note_scroll(t, top, bot, -n);
}

static void index_down(vt *t)
{
	if (t->c.y == t->bot)
		scroll_up(t, t->top, t->bot, 1, 1);
	else if (t->c.y < t->rows - 1)
		t->c.y++;
}

static void index_up(vt *t)
{
	if (t->c.y == t->top)
		scroll_down(t, t->top, t->bot, 1);
	else if (t->c.y > 0)
		t->c.y--;
}

/* ------------------------------------------------------------------ */
/* allocazione, reset                                                 */
/* ------------------------------------------------------------------ */

static int alloc_screen(int cols, int rows, vt_cell **buf, vt_cell ***lines)
{
	vt_cell b = { ' ', VT_DEFAULT, VT_DEFAULT, 0, 0 };
	int y;

	*buf = malloc((size_t)cols * rows * sizeof(vt_cell));
	*lines = malloc((size_t)rows * sizeof(vt_cell *));
	if (!*buf || !*lines) {
		free(*buf);
		free(*lines);
		*buf = NULL;
		*lines = NULL;
		return -1;
	}
	fill(*buf, cols * rows, b);
	for (y = 0; y < rows; y++)
		(*lines)[y] = *buf + (size_t)y * cols;
	return 0;
}

static void reset_tabs(vt *t)
{
	int x;

	for (x = 0; x < t->cols; x++)
		t->tabs[x] = x > 0 && x % 8 == 0;
}

static void reset_cursor(vt_cursor *c)
{
	memset(c, 0, sizeof(*c));
	c->pen.cp = ' ';
	c->pen.fg = c->pen.bg = VT_DEFAULT;
	c->cs[0] = c->cs[1] = 'B';
}

static void clear_screen(vt *t, vt_cell **lines)
{
	vt_cell b = blank(t);
	int y;

	for (y = 0; y < t->rows; y++)
		fill(lines[y], t->cols, b);
}

void vt_reset(vt *t)
{
	reset_cursor(&t->c);
	reset_cursor(&t->saved);
	reset_cursor(&t->saved_alt);
	t->alt = 0;
	t->line = t->lmain;
	clear_screen(t, t->lmain);
	clear_screen(t, t->lalt);
	t->top = 0;
	t->bot = t->rows - 1;
	t->m_wrap = 1;
	t->m_insert = t->m_lnm = t->m_appcursor = t->m_appkeypad = t->m_reverse = 0;
	t->m_bracket = t->m_mouse = 0;
	t->m_cursor = 1;
	t->st = S_GROUND;
	t->uneed = 0;
	t->last_cp = ' ';
	reset_tabs(t);
	vt_damage_all(t);
}

int vt_init(vt *t, int cols, int rows, int sb_lines)
{
	memset(t, 0, sizeof(*t));
	if (cols < 2)
		cols = 2;
	if (rows < 2)
		rows = 2;
	t->cols = cols;
	t->rows = rows;
	if (alloc_screen(cols, rows, &t->bmain, &t->lmain) < 0 ||
	    alloc_screen(cols, rows, &t->balt, &t->lalt) < 0)
		goto fail;
	t->tabs = calloc((size_t)cols, 1);
	t->dirty = calloc((size_t)rows, 1);
	t->dlo = calloc((size_t)rows, sizeof(int16_t));
	t->dhi = calloc((size_t)rows, sizeof(int16_t));
	if (!t->tabs || !t->dirty || !t->dlo || !t->dhi)
		goto fail;
	if (sb_lines > 0) {
		t->sb = malloc((size_t)sb_lines * cols * sizeof(vt_cell));
		if (!t->sb)
			goto fail;
		t->sb_cap = sb_lines;
		t->sb_cols = cols;
	}
	vt_reset(t);
	return 0;
fail:
	vt_free(t);
	return -1;
}

void vt_free(vt *t)
{
	free(t->bmain);
	free(t->balt);
	free(t->lmain);
	free(t->lalt);
	free(t->tabs);
	free(t->dirty);
	free(t->dlo);
	free(t->dhi);
	free(t->sb);
	memset(t, 0, sizeof(*t));
}

static void copy_screen(vt_cell **dst, int dcols, int drows, vt_cell **src, int scols, int srows, int skip)
{
	int y, n = MIN(dcols, scols);

	for (y = 0; y < drows && y + skip < srows; y++)
		memcpy(dst[y], src[y + skip], (size_t)n * sizeof(vt_cell));
}

static void clamp_cursor(vt_cursor *c, int cols, int rows)
{
	c->x = MIN(MAX(c->x, 0), cols - 1);
	c->y = MIN(MAX(c->y, 0), rows - 1);
	c->wrapnext = 0;
}

int vt_resize(vt *t, int cols, int rows)
{
	vt_cell *bm, *ba, **lm, **la, *sb = NULL;
	uint8_t *tabs, *dirty;
	int16_t *dlo, *dhi;
	int skip = 0, i;

	if (cols < 2)
		cols = 2;
	if (rows < 2)
		rows = 2;
	if (cols == t->cols && rows == t->rows)
		return 0;
	if (alloc_screen(cols, rows, &bm, &lm) < 0)
		return -1;
	if (alloc_screen(cols, rows, &ba, &la) < 0) {
		free(bm);
		free(lm);
		return -1;
	}
	tabs = calloc((size_t)cols, 1);
	dirty = calloc((size_t)rows, 1);
	dlo = calloc((size_t)rows, sizeof(int16_t));
	dhi = calloc((size_t)rows, sizeof(int16_t));
	/* lo storico non si accorcia mai (ruotando avanti e indietro le righe
	 * restano intere): si allunga se servono piu' colonne */
	if (t->sb_cap && cols > t->sb_cols)
		sb = malloc((size_t)t->sb_cap * cols * sizeof(vt_cell));
	if (!tabs || !dirty || !dlo || !dhi || (t->sb_cap && cols > t->sb_cols && !sb)) {
		free(bm);
		free(lm);
		free(ba);
		free(la);
		free(tabs);
		free(dirty);
		free(dlo);
		free(dhi);
		free(sb);
		return -1;
	}
	/* righe di meno: la riga del cursore resta in vista, quelle sopra vanno nello storico */
	if (!t->alt && t->c.y >= rows)
		skip = t->c.y - rows + 1;
	else if (t->alt && t->saved.y >= rows)
		skip = t->saved.y - rows + 1;
	for (i = 0; i < skip; i++)
		sb_push(t, t->lmain[i]);
	if (sb) {
		/* lo storico alla nuova larghezza (righe troncate o allungate) */
		vt_cell b = { ' ', VT_DEFAULT, VT_DEFAULT, 0, 0 };
		int n = MIN(cols, t->sb_cols);

		for (i = 0; i < t->sb_cap; i++) {
			memcpy(sb + (size_t)i * cols, t->sb + (size_t)i * t->sb_cols, (size_t)n * sizeof(vt_cell));
			if (n < cols)
				fill(sb + (size_t)i * cols + n, cols - n, b);
		}
		free(t->sb);
		t->sb = sb;
		t->sb_cols = cols;
	}
	copy_screen(lm, cols, rows, t->lmain, t->cols, t->rows, skip);
	copy_screen(la, cols, rows, t->lalt, t->cols, t->rows, t->alt ? MAX(0, t->c.y - rows + 1) : 0);
	if (t->alt)
		t->c.y -= MAX(0, t->c.y - rows + 1);
	else
		t->c.y -= skip;
	t->saved.y -= skip;
	free(t->bmain);
	free(t->balt);
	free(t->lmain);
	free(t->lalt);
	free(t->tabs);
	free(t->dirty);
	free(t->dlo);
	free(t->dhi);
	t->bmain = bm;
	t->balt = ba;
	t->lmain = lm;
	t->lalt = la;
	t->line = t->alt ? la : lm;
	t->tabs = tabs;
	t->dirty = dirty;
	t->dlo = dlo;
	t->dhi = dhi;
	t->cols = cols;
	t->rows = rows;
	/* le celle larghe tagliate a meta' sull'ultima colonna */
	for (i = 0; i < rows; i++) {
		if (lm[i][cols - 1].attr & VT_WIDE) {
			lm[i][cols - 1].cp = ' ';
			lm[i][cols - 1].attr &= (uint16_t)~VT_WIDE;
		}
		if (la[i][cols - 1].attr & VT_WIDE) {
			la[i][cols - 1].cp = ' ';
			la[i][cols - 1].attr &= (uint16_t)~VT_WIDE;
		}
	}
	t->top = 0;
	t->bot = rows - 1;
	clamp_cursor(&t->c, cols, rows);
	clamp_cursor(&t->saved, cols, rows);
	clamp_cursor(&t->saved_alt, cols, rows);
	reset_tabs(t);
	vt_damage_all(t);
	return 0;
}

/* ------------------------------------------------------------------ */
/* scrittura dei caratteri                                            */
/* ------------------------------------------------------------------ */

/* la cella (x, y) sta per essere sovrascritta: se era meta' di un carattere
 * largo, l'altra meta' diventa vuota */
static void break_wide(vt *t, int y, int x)
{
	vt_cell *l = t->line[y];

	if (x < 0 || x >= t->cols)
		return;
	if ((l[x].attr & VT_WCONT) && x > 0) {
		l[x - 1].cp = ' ';
		l[x - 1].attr &= (uint16_t)~VT_WIDE;
		mark(t, y, x - 1, x - 1);
	}
	if ((l[x].attr & VT_WIDE) && x + 1 < t->cols) {
		l[x + 1].cp = ' ';
		l[x + 1].attr &= (uint16_t)~VT_WCONT;
		mark(t, y, x + 1, x + 1);
	}
}

static void insert_blanks(vt *t, int n)
{
	vt_cell *l = t->line[t->c.y];
	int x = t->c.x;

	n = MIN(n, t->cols - x);
	if (n <= 0)
		return;
	break_wide(t, t->c.y, x);
	memmove(l + x + n, l + x, (size_t)(t->cols - x - n) * sizeof(vt_cell));
	fill(l + x, n, blank(t));
	if (l[t->cols - 1].attr & VT_WIDE) {
		l[t->cols - 1].cp = ' ';
		l[t->cols - 1].attr &= (uint16_t)~VT_WIDE;
	}
	mark(t, t->c.y, x, t->cols - 1);
}

static const uint16_t dec_graphics[32] = {
	0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,	/* _ ` a b c d e f */
	0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,	/* g h i j k l m n */
	0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,	/* o p q r s t u v */
	0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,	/* w x y z { | } ~ */
};

static void put_char(vt *t, uint32_t cp)
{
	vt_cell *l;
	int w, x;
	uint8_t cs = t->c.cs[t->c.gl];

	if (cp < 0x80) {
		if (cs == '0' && cp >= 0x5F && cp <= 0x7E)
			cp = dec_graphics[cp - 0x5F];
		else if (cs == 'A' && cp == '#')
			cp = 0xA3;
	}
	w = vt_wcwidth(cp);
	if (w == 0)
		return;		/* combinante: niente da comporre con i nostri glifi */
	if (w == 2 && t->cols < 2)
		w = 1;
	if (t->c.wrapnext && t->m_wrap) {
		t->c.x = 0;
		index_down(t);
	}
	t->c.wrapnext = 0;
	if (w == 2 && t->c.x == t->cols - 1) {
		if (t->m_wrap) {
			/* un carattere largo non entra nell'ultima colonna: va a capo */
			break_wide(t, t->c.y, t->c.x);
			t->line[t->c.y][t->c.x] = blank(t);
			mark(t, t->c.y, t->c.x, t->c.x);
			t->c.x = 0;
			index_down(t);
		} else {
			t->c.x = t->cols - 2;
		}
	}
	if (t->m_insert)
		insert_blanks(t, w);
	x = t->c.x;
	l = t->line[t->c.y];
	break_wide(t, t->c.y, x);
	if (w == 2)
		break_wide(t, t->c.y, x + 1);
	l[x] = t->c.pen;
	l[x].cp = cp;
	l[x].attr &= (uint16_t)~(VT_WIDE | VT_WCONT);
	if (w == 2) {
		l[x].attr |= VT_WIDE;
		l[x + 1] = l[x];
		l[x + 1].cp = 0;
		l[x + 1].attr = (uint16_t)((l[x].attr & ~VT_WIDE) | VT_WCONT);
	}
	mark(t, t->c.y, x, x + w - 1);
	t->last_cp = cp;
	if (x + w >= t->cols) {
		t->c.x = t->cols - 1;
		t->c.wrapnext = t->m_wrap;
	} else {
		t->c.x = x + w;
	}
}

/* ------------------------------------------------------------------ */
/* cancellazioni                                                      */
/* ------------------------------------------------------------------ */

static void erase(vt *t, int y, int x0, int x1)
{
	if (y < 0 || y >= t->rows)
		return;
	x0 = MAX(x0, 0);
	x1 = MIN(x1, t->cols - 1);
	if (x0 > x1)
		return;
	break_wide(t, y, x0);
	break_wide(t, y, x1);
	fill(t->line[y] + x0, x1 - x0 + 1, blank(t));
	mark(t, y, x0, x1);
}

static void erase_display(vt *t, int mode)
{
	int y;

	switch (mode) {
	case 0:
		erase(t, t->c.y, t->c.x, t->cols - 1);
		for (y = t->c.y + 1; y < t->rows; y++)
			erase(t, y, 0, t->cols - 1);
		break;
	case 1:
		for (y = 0; y < t->c.y; y++)
			erase(t, y, 0, t->cols - 1);
		erase(t, t->c.y, 0, t->c.x);
		break;
	case 2:
		for (y = 0; y < t->rows; y++)
			erase(t, y, 0, t->cols - 1);
		break;
	case 3:
		t->sb_count = 0;	/* "clear" di ncurses: via anche lo storico */
		break;
	}
}

static void delete_chars(vt *t, int n)
{
	vt_cell *l = t->line[t->c.y];
	int x = t->c.x;

	n = MIN(n, t->cols - x);
	if (n <= 0)
		return;
	break_wide(t, t->c.y, x);
	break_wide(t, t->c.y, x + n - 1);
	memmove(l + x, l + x + n, (size_t)(t->cols - x - n) * sizeof(vt_cell));
	fill(l + t->cols - n, n, blank(t));
	if (l[x].attr & VT_WCONT) {
		l[x].cp = ' ';
		l[x].attr &= (uint16_t)~VT_WCONT;
	}
	mark(t, t->c.y, x, t->cols - 1);
}

/* ------------------------------------------------------------------ */
/* cursore                                                            */
/* ------------------------------------------------------------------ */

static void move_to(vt *t, int x, int y)
{
	int lo = t->c.origin ? t->top : 0, hi = t->c.origin ? t->bot : t->rows - 1;

	t->c.x = MIN(MAX(x, 0), t->cols - 1);
	t->c.y = MIN(MAX(y, lo), hi);
	t->c.wrapnext = 0;
}

/* CUP: coordinate relative alla regione in modo origine */
static void move_abs(vt *t, int x, int y)
{
	move_to(t, x, y + (t->c.origin ? t->top : 0));
}

static void save_cursor(vt *t)
{
	if (t->alt)
		t->saved_alt = t->c;
	else
		t->saved = t->c;
}

static void restore_cursor(vt *t)
{
	t->c = t->alt ? t->saved_alt : t->saved;
	clamp_cursor(&t->c, t->cols, t->rows);
}

static void set_alt(vt *t, int on, int clear)
{
	if (on == t->alt)
		return;
	t->alt = on;
	t->line = on ? t->lalt : t->lmain;
	if (on && clear)
		clear_screen(t, t->lalt);
	vt_damage_all(t);
}

/* ------------------------------------------------------------------ */
/* SGR                                                                */
/* ------------------------------------------------------------------ */

static uint16_t rgb565(int r, int g, int b)
{
	r = MIN(MAX(r, 0), 255);
	g = MIN(MAX(g, 0), 255);
	b = MIN(MAX(b, 0), 255);
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static int par(const vt *t, int i, int def)
{
	return i < t->npar && t->par[i] > 0 ? t->par[i] : def;
}

static int par0(const vt *t, int i)
{
	return i < t->npar && t->par[i] >= 0 ? t->par[i] : 0;
}

/* colore esteso da 38/48/58 a partire dall'indice i; ritorna l'ultimo indice usato */
static int ext_color(vt *t, int i, int which)
{
	int kind, r, g, b, n = 0, j;
	vt_cell *p = &t->c.pen;

	if (i + 1 < t->npar && t->sub[i + 1]) {
		/* forma coi due punti: 38:5:N, 38:2:R:G:B o 38:2::R:G:B (colorspace) */
		for (j = i + 1; j < t->npar && t->sub[j]; j++)
			n++;
		kind = par0(t, i + 1);
		if (kind == 5 && n >= 2) {
			r = par0(t, i + 2);
			if (which == 38) {
				p->fg = (uint16_t)(r & 255);
				p->attr &= (uint16_t)~VT_FG_RGB;
			} else if (which == 48) {
				p->bg = (uint16_t)(r & 255);
				p->attr &= (uint16_t)~VT_BG_RGB;
			}
		} else if (kind == 2 && n >= 4) {
			int o = n >= 5 ? i + 3 : i + 2;

			r = par0(t, o);
			g = par0(t, o + 1);
			b = par0(t, o + 2);
			if (which == 38) {
				p->fg = rgb565(r, g, b);
				p->attr |= VT_FG_RGB;
			} else if (which == 48) {
				p->bg = rgb565(r, g, b);
				p->attr |= VT_BG_RGB;
			}
		}
		return i + n;
	}
	kind = par0(t, i + 1);
	if (kind == 5) {
		r = par0(t, i + 2);
		if (which == 38) {
			p->fg = (uint16_t)(r & 255);
			p->attr &= (uint16_t)~VT_FG_RGB;
		} else if (which == 48) {
			p->bg = (uint16_t)(r & 255);
			p->attr &= (uint16_t)~VT_BG_RGB;
		}
		return i + 2;
	}
	if (kind == 2) {
		r = par0(t, i + 2);
		g = par0(t, i + 3);
		b = par0(t, i + 4);
		if (which == 38) {
			p->fg = rgb565(r, g, b);
			p->attr |= VT_FG_RGB;
		} else if (which == 48) {
			p->bg = rgb565(r, g, b);
			p->attr |= VT_BG_RGB;
		}
		return i + 4;
	}
	return i + 1;
}

static void sgr(vt *t)
{
	vt_cell *p = &t->c.pen;
	int i;

	if (!t->npar) {
		t->npar = 1;
		t->par[0] = 0;
	}
	for (i = 0; i < t->npar; i++) {
		int v = t->par[i] < 0 ? 0 : t->par[i];

		if (t->sub[i])
			continue;	/* sotto-parametro non usato (es. 4:3) */
		switch (v) {
		case 0:
			p->attr = 0;
			p->fg = p->bg = VT_DEFAULT;
			break;
		case 1: p->attr |= VT_BOLD; break;
		case 2: p->attr |= VT_DIM; break;
		case 3: p->attr |= VT_ITALIC; break;
		case 4:
			if (i + 1 < t->npar && t->sub[i + 1] && t->par[i + 1] == 0)
				p->attr &= (uint16_t)~VT_UNDER;
			else
				p->attr |= VT_UNDER;
			break;
		case 5: case 6: p->attr |= VT_BLINK; break;
		case 7: p->attr |= VT_REVERSE; break;
		case 8: p->attr |= VT_INVIS; break;
		case 9: p->attr |= VT_STRIKE; break;
		case 21: p->attr |= VT_UNDER; break;
		case 22: p->attr &= (uint16_t)~(VT_BOLD | VT_DIM); break;
		case 23: p->attr &= (uint16_t)~VT_ITALIC; break;
		case 24: p->attr &= (uint16_t)~VT_UNDER; break;
		case 25: p->attr &= (uint16_t)~VT_BLINK; break;
		case 27: p->attr &= (uint16_t)~VT_REVERSE; break;
		case 28: p->attr &= (uint16_t)~VT_INVIS; break;
		case 29: p->attr &= (uint16_t)~VT_STRIKE; break;
		case 38: case 48: case 58:
			i = ext_color(t, i, v);
			break;
		case 39:
			p->fg = VT_DEFAULT;
			p->attr &= (uint16_t)~VT_FG_RGB;
			break;
		case 49:
			p->bg = VT_DEFAULT;
			p->attr &= (uint16_t)~VT_BG_RGB;
			break;
		default:
			if (v >= 30 && v <= 37) {
				p->fg = (uint16_t)(v - 30);
				p->attr &= (uint16_t)~VT_FG_RGB;
			} else if (v >= 40 && v <= 47) {
				p->bg = (uint16_t)(v - 40);
				p->attr &= (uint16_t)~VT_BG_RGB;
			} else if (v >= 90 && v <= 97) {
				p->fg = (uint16_t)(v - 90 + 8);
				p->attr &= (uint16_t)~VT_FG_RGB;
			} else if (v >= 100 && v <= 107) {
				p->bg = (uint16_t)(v - 100 + 8);
				p->attr &= (uint16_t)~VT_BG_RGB;
			}
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* modalita'                                                          */
/* ------------------------------------------------------------------ */

static void set_mode(vt *t, int on)
{
	int i;

	for (i = 0; i < t->npar; i++) {
		int m = t->par[i];

		if (t->priv == '?') {
			switch (m) {
			case 1: t->m_appcursor = (uint8_t)on; break;
			case 5:
				if (t->m_reverse != on) {
					t->m_reverse = (uint8_t)on;
					vt_damage_all(t);
				}
				break;
			case 6:
				t->c.origin = (uint8_t)on;
				move_abs(t, 0, 0);
				break;
			case 7: t->m_wrap = (uint8_t)on; if (!on) t->c.wrapnext = 0; break;
			case 25:
				if (t->m_cursor != on) {
					t->m_cursor = (uint8_t)on;
					mark(t, t->c.y, t->c.x, t->c.x);
				}
				break;
			case 47:
				set_alt(t, on, 0);
				break;
			case 1047:
				if (!on && t->alt)
					clear_screen(t, t->lalt);
				set_alt(t, on, 0);
				break;
			case 1048:
				if (on)
					save_cursor(t);
				else
					restore_cursor(t);
				break;
			case 1049:
				if (on) {
					t->saved = t->c;
					set_alt(t, 1, 1);
					t->c = t->saved;
				} else if (t->alt) {
					set_alt(t, 0, 0);
					t->c = t->saved;
					clamp_cursor(&t->c, t->cols, t->rows);
				}
				break;
			case 2004: t->m_bracket = (uint8_t)on; break;
			case 9: case 1000: case 1002: case 1003: case 1005: case 1006: case 1015:
				t->m_mouse = (uint8_t)on;
				break;
			default:
				break;
			}
		} else if (!t->priv) {
			if (m == 4)
				t->m_insert = (uint8_t)on;
			else if (m == 20)
				t->m_lnm = (uint8_t)on;
		}
	}
}

/* ------------------------------------------------------------------ */
/* CSI                                                                */
/* ------------------------------------------------------------------ */

static void reply(vt *t, const char *s)
{
	if (t->reply)
		t->reply(t->user, s, (int)strlen(s));
}

static void tab_forward(vt *t, int n)
{
	while (n-- > 0) {
		int x = t->c.x + 1;

		while (x < t->cols - 1 && !t->tabs[x])
			x++;
		t->c.x = MIN(x, t->cols - 1);
	}
	t->c.wrapnext = 0;
}

static void tab_back(vt *t, int n)
{
	while (n-- > 0 && t->c.x > 0) {
		int x = t->c.x - 1;

		while (x > 0 && !t->tabs[x])
			x--;
		t->c.x = x;
	}
	t->c.wrapnext = 0;
}

static void soft_reset(vt *t)
{
	t->m_insert = 0;
	t->m_appcursor = t->m_appkeypad = 0;
	t->m_wrap = 1;
	t->m_cursor = 1;
	t->c.origin = 0;
	t->top = 0;
	t->bot = t->rows - 1;
	t->c.pen.attr = 0;
	t->c.pen.fg = t->c.pen.bg = VT_DEFAULT;
	t->c.cs[0] = t->c.cs[1] = 'B';
	t->c.gl = 0;
	t->saved = t->c;
}

static void csi(vt *t, int f)
{
	char buf[48];
	int n, y;

	if (t->inter) {
		if (t->inter == '!' && f == 'p')
			soft_reset(t);
		return;		/* forma del cursore, DECRQM, ...: ignorati */
	}
	if (t->priv && t->priv != '?') {
		if (t->priv == '>' && f == 'c')
			reply(t, "\033[>1;10;0c");	/* DA2: un VT220, nessuna versione di xterm */
		return;
	}
	switch (f) {
	case '@':
		insert_blanks(t, par(t, 0, 1));
		break;
	case 'A':
		n = par(t, 0, 1);
		y = t->c.y >= t->top ? MAX(t->c.y - n, t->top) : MAX(t->c.y - n, 0);
		t->c.y = y;
		t->c.wrapnext = 0;
		break;
	case 'B': case 'e':
		n = par(t, 0, 1);
		y = t->c.y <= t->bot ? MIN(t->c.y + n, t->bot) : MIN(t->c.y + n, t->rows - 1);
		t->c.y = y;
		t->c.wrapnext = 0;
		break;
	case 'C': case 'a':
		t->c.x = MIN(t->c.x + par(t, 0, 1), t->cols - 1);
		t->c.wrapnext = 0;
		break;
	case 'D':
		t->c.x = MAX(t->c.x - par(t, 0, 1), 0);
		t->c.wrapnext = 0;
		break;
	case 'E':
		n = par(t, 0, 1);
		t->c.y = t->c.y <= t->bot ? MIN(t->c.y + n, t->bot) : MIN(t->c.y + n, t->rows - 1);
		t->c.x = 0;
		t->c.wrapnext = 0;
		break;
	case 'F':
		n = par(t, 0, 1);
		t->c.y = t->c.y >= t->top ? MAX(t->c.y - n, t->top) : MAX(t->c.y - n, 0);
		t->c.x = 0;
		t->c.wrapnext = 0;
		break;
	case 'G': case '`':
		t->c.x = MIN(par(t, 0, 1) - 1, t->cols - 1);
		t->c.wrapnext = 0;
		break;
	case 'H': case 'f':
		move_abs(t, par(t, 1, 1) - 1, par(t, 0, 1) - 1);
		break;
	case 'I':
		tab_forward(t, par(t, 0, 1));
		break;
	case 'J':
		erase_display(t, par0(t, 0));
		break;
	case 'K':
		switch (par0(t, 0)) {
		case 0: erase(t, t->c.y, t->c.x, t->cols - 1); break;
		case 1: erase(t, t->c.y, 0, t->c.x); break;
		case 2: erase(t, t->c.y, 0, t->cols - 1); break;
		}
		break;
	case 'L':
		if (t->c.y >= t->top && t->c.y <= t->bot) {
			scroll_down(t, t->c.y, t->bot, par(t, 0, 1));
			t->c.x = 0;
			t->c.wrapnext = 0;
		}
		break;
	case 'M':
		if (t->c.y >= t->top && t->c.y <= t->bot) {
			scroll_up(t, t->c.y, t->bot, par(t, 0, 1), 0);
			t->c.x = 0;
			t->c.wrapnext = 0;
		}
		break;
	case 'P':
		delete_chars(t, par(t, 0, 1));
		break;
	case 'S':
		if (!t->priv)
			scroll_up(t, t->top, t->bot, par(t, 0, 1), 1);
		break;
	case 'T':
		if (t->npar <= 1)
			scroll_down(t, t->top, t->bot, par(t, 0, 1));
		break;
	case 'X':
		n = par(t, 0, 1);
		erase(t, t->c.y, t->c.x, t->c.x + n - 1);
		break;
	case 'Z':
		tab_back(t, par(t, 0, 1));
		break;
	case 'b':
		n = MIN(par(t, 0, 1), 65535);
		while (n-- > 0)
			put_char(t, t->last_cp);
		break;
	case 'c':
		if (!t->priv && par0(t, 0) == 0)
			reply(t, "\033[?62;22c");
		break;
	case 'd':
		move_abs(t, t->c.x, par(t, 0, 1) - 1);
		break;
	case 'g':
		if (par0(t, 0) == 0)
			t->tabs[t->c.x] = 0;
		else if (par0(t, 0) == 3)
			memset(t->tabs, 0, (size_t)t->cols);
		break;
	case 'h':
		set_mode(t, 1);
		break;
	case 'l':
		set_mode(t, 0);
		break;
	case 'm':
		if (!t->priv)
			sgr(t);
		break;
	case 'n':
		if (par0(t, 0) == 5) {
			reply(t, "\033[0n");
		} else if (par0(t, 0) == 6) {
			snprintf(buf, sizeof(buf), "\033[%s%d;%dR", t->priv == '?' ? "?" : "",
				 t->c.y - (t->c.origin ? t->top : 0) + 1, t->c.x + 1);
			reply(t, buf);
		}
		break;
	case 'r':
		if (!t->priv) {
			int top = par(t, 0, 1) - 1, bot = par(t, 1, t->rows) - 1;

			bot = MIN(bot, t->rows - 1);
			if (top < bot) {
				t->top = top;
				t->bot = bot;
				move_abs(t, 0, 0);
			}
		}
		break;
	case 's':
		if (!t->priv)
			save_cursor(t);
		break;
	case 'u':
		if (!t->priv)
			restore_cursor(t);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* ESC, OSC                                                           */
/* ------------------------------------------------------------------ */

static void esc(vt *t, int f)
{
	switch (f) {
	case '7': save_cursor(t); break;
	case '8': restore_cursor(t); break;
	case 'D': index_down(t); t->c.wrapnext = 0; break;
	case 'E': t->c.x = 0; index_down(t); t->c.wrapnext = 0; break;
	case 'M': index_up(t); t->c.wrapnext = 0; break;
	case 'H': t->tabs[t->c.x] = 1; break;
	case 'c': vt_reset(t); break;
	case '=': t->m_appkeypad = 1; break;
	case '>': t->m_appkeypad = 0; break;
	case 'Z': reply(t, "\033[?62;22c"); break;
	default: break;
	}
}

static void esc_int(vt *t, int f)
{
	int y;

	switch (t->inter) {
	case '(':
	case ')':
		t->c.cs[t->inter == ')'] = (uint8_t)(f == '0' || f == 'A' ? f : 'B');
		break;
	case '#':
		if (f == '8') {
			/* DECALN: schermo pieno di E (vttest) */
			vt_cell e = { 'E', VT_DEFAULT, VT_DEFAULT, 0, 0 };

			for (y = 0; y < t->rows; y++)
				fill(t->line[y], t->cols, e);
			t->top = 0;
			t->bot = t->rows - 1;
			move_to(t, 0, 0);
			mark_rows(t, 0, t->rows - 1);
		}
		break;
	default:
		break;
	}
}

static void osc_end(vt *t)
{
	char *s = t->osc;
	int n = 0;

	t->osc[t->osc_len] = 0;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + (*s++ - '0');
	if (*s != ';' || (n != 0 && n != 1 && n != 2))
		return;
	s++;
	snprintf(t->title, sizeof(t->title), "%s", s);
	if (t->on_title)
		t->on_title(t->user, t->title);
}

/* ------------------------------------------------------------------ */
/* analizzatore                                                       */
/* ------------------------------------------------------------------ */

static void control(vt *t, int ch)
{
	switch (ch) {
	case 0x07:
		if (t->on_bell)
			t->on_bell(t->user);
		break;
	case 0x08:
		if (t->c.x > 0)
			t->c.x--;
		t->c.wrapnext = 0;
		break;
	case 0x09:
		tab_forward(t, 1);
		break;
	case 0x0A: case 0x0B: case 0x0C:
		index_down(t);
		if (t->m_lnm)
			t->c.x = 0;
		t->c.wrapnext = 0;
		break;
	case 0x0D:
		t->c.x = 0;
		t->c.wrapnext = 0;
		break;
	case 0x0E:
		t->c.gl = 1;
		break;
	case 0x0F:
		t->c.gl = 0;
		break;
	default:
		break;
	}
}

static void csi_byte(vt *t, int ch)
{
	if (ch >= '0' && ch <= '9') {
		if (!t->npar) {
			t->npar = 1;
			t->par[0] = -1;
			t->sub[0] = 0;
		}
		if (t->par[t->npar - 1] < 0)
			t->par[t->npar - 1] = 0;
		if (t->par[t->npar - 1] < 100000)
			t->par[t->npar - 1] = t->par[t->npar - 1] * 10 + (ch - '0');
	} else if (ch == ';' || ch == ':') {
		if (!t->npar) {
			t->npar = 1;
			t->par[0] = -1;
			t->sub[0] = 0;
		}
		if (t->npar < 16) {
			t->par[t->npar] = -1;
			t->sub[t->npar] = ch == ':';
			t->npar++;
		}
	} else if (ch >= 0x3C && ch <= 0x3F) {
		if (!t->npar && !t->priv)
			t->priv = (char)ch;
	} else if (ch >= 0x20 && ch <= 0x2F) {
		t->inter = (char)ch;
	} else if (ch >= 0x40 && ch <= 0x7E) {
		t->st = S_GROUND;
		csi(t, ch);
	}
}

static void ground_cp(vt *t, uint32_t cp)
{
	put_char(t, cp);
}

static void byte(vt *t, uint8_t ch)
{
	/* controlli validi quasi ovunque */
	if (ch == 0x18 || ch == 0x1A) {		/* CAN, SUB */
		t->st = S_GROUND;
		t->uneed = 0;
		return;
	}
	if (ch == 0x1B) {
		if (t->uneed) {
			t->uneed = 0;
			ground_cp(t, 0xFFFD);
		}
		if (t->st == S_OSC) {
			t->st = S_OSC_ESC;
			return;
		}
		if (t->st == S_STR) {
			t->st = S_STR_ESC;
			return;
		}
		t->st = S_ESC;
		t->inter = 0;
		return;
	}
	switch (t->st) {
	case S_OSC:
		if (ch == 0x07) {
			osc_end(t);
			t->st = S_GROUND;
		} else if (ch >= 0x20 && t->osc_len < (int)sizeof(t->osc) - 1) {
			t->osc[t->osc_len++] = (char)ch;
		}
		return;
	case S_OSC_ESC:
		osc_end(t);
		t->st = S_GROUND;
		if (ch != '\\')
			byte(t, ch);
		return;
	case S_STR:
		return;
	case S_STR_ESC:
		t->st = ch == '\\' ? S_GROUND : S_STR;
		return;
	default:
		break;
	}
	if (ch < 0x20 || ch == 0x7F) {
		if (t->uneed) {
			t->uneed = 0;
			ground_cp(t, 0xFFFD);
		}
		if (ch != 0x7F)
			control(t, ch);
		return;
	}
	switch (t->st) {
	case S_ESC:
		t->st = S_GROUND;
		switch (ch) {
		case '[':
			t->st = S_CSI;
			t->npar = 0;
			t->priv = t->inter = 0;
			break;
		case ']':
			t->st = S_OSC;
			t->osc_len = 0;
			break;
		case 'P': case 'X': case '^': case '_':
			t->st = S_STR;
			break;
		case '(': case ')': case '*': case '+': case '#': case ' ': case '%':
			t->inter = (char)ch;
			t->st = S_ESC_INT;
			break;
		default:
			if (ch >= 0x30 && ch <= 0x7E)
				esc(t, ch);
			break;
		}
		return;
	case S_ESC_INT:
		if (ch >= 0x30 && ch <= 0x7E) {
			t->st = S_GROUND;
			esc_int(t, ch);
		}
		return;
	case S_CSI:
		if (ch >= 0x80)
			return;
		csi_byte(t, ch);
		return;
	default:
		break;
	}

	/* testo: UTF-8 */
	if (t->uneed) {
		if ((ch & 0xC0) == 0x80) {
			t->ucp = (t->ucp << 6) | (ch & 0x3F);
			if (--t->uneed == 0) {
				uint32_t cp = t->ucp;

				if (cp < t->umin || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
					cp = 0xFFFD;
				ground_cp(t, cp);
			}
			return;
		}
		t->uneed = 0;
		ground_cp(t, 0xFFFD);
	}
	if (ch < 0x80) {
		ground_cp(t, ch);
	} else if (ch >= 0xC2 && ch <= 0xDF) {
		t->ucp = ch & 0x1F;
		t->uneed = 1;
		t->umin = 0x80;
	} else if (ch >= 0xE0 && ch <= 0xEF) {
		t->ucp = ch & 0x0F;
		t->uneed = 2;
		t->umin = 0x800;
	} else if (ch >= 0xF0 && ch <= 0xF4) {
		t->ucp = ch & 0x07;
		t->uneed = 3;
		t->umin = 0x10000;
	} else {
		ground_cp(t, 0xFFFD);
	}
}

void vt_write(vt *t, const uint8_t *buf, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		uint8_t ch = buf[i];

		/* scorciatoia per il testo semplice, il caso piu' comune */
		if (t->st == S_GROUND && !t->uneed && ch >= 0x20 && ch < 0x7F && t->c.cs[t->c.gl] == 'B')
			put_char(t, ch);
		else
			byte(t, ch);
	}
}

/* ------------------------------------------------------------------ */
/* tastiera                                                           */
/* ------------------------------------------------------------------ */

int vt_key(const vt *t, int key, int mods, char *out)
{
	int m = (mods & 7) ? 1 + ((mods & VT_MOD_SHIFT) ? 1 : 0) + ((mods & VT_MOD_ALT) ? 2 : 0) +
				     ((mods & VT_MOD_CTRL) ? 4 : 0) : 0;
	const char *fin = NULL;
	int tilde = 0, n = 0;

	switch (key) {
	case VT_K_UP: fin = "A"; break;
	case VT_K_DOWN: fin = "B"; break;
	case VT_K_RIGHT: fin = "C"; break;
	case VT_K_LEFT: fin = "D"; break;
	case VT_K_HOME: fin = "H"; break;
	case VT_K_END: fin = "F"; break;
	case VT_K_INSERT: tilde = 2; break;
	case VT_K_DELETE: tilde = 3; break;
	case VT_K_PGUP: tilde = 5; break;
	case VT_K_PGDN: tilde = 6; break;
	case VT_K_BACKSPACE:
		if (mods & VT_MOD_ALT)
			out[n++] = 0x1B;
		out[n++] = (mods & VT_MOD_CTRL) ? 0x08 : 0x7F;
		return n;
	case VT_K_TAB:
		if (mods & VT_MOD_SHIFT)
			return sprintf(out, "\033[Z");
		if (mods & VT_MOD_ALT)
			out[n++] = 0x1B;
		out[n++] = '\t';
		return n;
	case VT_K_ENTER:
		if (mods & VT_MOD_ALT)
			out[n++] = 0x1B;
		out[n++] = '\r';
		if (t && t->m_lnm)
			out[n++] = '\n';
		return n;
	case VT_K_ESC:
		if (mods & VT_MOD_ALT)
			out[n++] = 0x1B;
		out[n++] = 0x1B;
		return n;
	default:
		return 0;
	}
	if (tilde)
		return m ? sprintf(out, "\033[%d;%d~", tilde, m) : sprintf(out, "\033[%d~", tilde);
	if (m)
		return sprintf(out, "\033[1;%d%s", m, fin);
	return sprintf(out, "\033%c%s", t && t->m_appcursor ? 'O' : '[', fin);
}

int vt_char(uint32_t cp, int mods, char *out)
{
	int n = 0;

	if (mods & VT_MOD_ALT)
		out[n++] = 0x1B;
	if (mods & VT_MOD_CTRL) {
		if (cp >= 'a' && cp <= 'z')
			cp -= 'a' - 'A';
		if (cp >= '@' && cp <= '_') {
			out[n++] = (char)(cp - '@');
			return n;
		}
		if (cp == ' ' || cp == '2') {
			out[n++] = 0;
			return n;
		}
		if (cp == '?' || cp == '8') {
			out[n++] = 0x7F;
			return n;
		}
		if (cp >= '3' && cp <= '7') {
			out[n++] = (char)(0x1B + (cp - '3'));	/* Ctrl+3..7: ESC, FS, GS, RS, US */
			return n;
		}
		if (cp == '/') {
			out[n++] = 0x1F;
			return n;
		}
		if (cp < 0x20) {
			out[n++] = (char)cp;
			return n;
		}
	}
	if (cp < 0x80) {
		out[n++] = (char)cp;
	} else if (cp < 0x800) {
		out[n++] = (char)(0xC0 | (cp >> 6));
		out[n++] = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out[n++] = (char)(0xE0 | (cp >> 12));
		out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[n++] = (char)(0x80 | (cp & 0x3F));
	} else {
		out[n++] = (char)(0xF0 | (cp >> 18));
		out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[n++] = (char)(0x80 | (cp & 0x3F));
	}
	return n;
}
