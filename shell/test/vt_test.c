// SPDX-License-Identifier: MIT
/*
 * vt_test.c - prove dell'emulatore di terminale (vt.c), senza schermo.
 *
 *   make tests && build/debug/vt_test          (dalla cartella shell/)
 *
 * 1. Flussi registrati (test/vt/NOME.raw, vedi test/vt/record.sh): bash con
 *    prompt colorato e readline, ls --color, top, htop, vim (schermata
 *    iniziale e un file scorso con Ctrl-E/Ctrl-Y e una riga inserita), nano,
 *    less, la riga di avanzamento di apt, frammenti alla vttest. Il testo di
 *    ogni riga e la posizione del cursore devono essere quelli che mostrava
 *    tmux (NOME.txt, NOME.cur).
 * 2. Gli stessi flussi spezzati a caso: dopo ogni pezzo una "tela" finta
 *    viene aggiornata solo come farebbe la vista (scorrimento in attesa =
 *    spostamento di righe, poi le sole celle segnate) e deve coincidere con
 *    la griglia: prova che le righe cambiate sono annotate tutte.
 * 3. Sequenze scritte a mano con controlli di colori e attributi, risposte
 *    DA/DSR, titolo, tasti, UTF-8, ridimensionamento, storico.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vt.h"

static int fails, checks;

#define CHECK(cond, ...)                                       \
	do {                                                   \
		checks++;                                      \
		if (!(cond)) {                                 \
			fails++;                               \
			printf("FALLITO %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__);                   \
			printf("\n");                          \
		}                                              \
	} while (0)

/* ---- utilita' ---- */

static int utf8(uint32_t cp, char *o)
{
	if (cp < 0x80) {
		o[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		o[0] = (char)(0xC0 | (cp >> 6));
		o[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (char)(0xE0 | (cp >> 12));
		o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	o[0] = (char)(0xF0 | (cp >> 18));
	o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	o[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* tmux scrive i caratteri del set grafico DEC con la lettera originale */
static const uint16_t acs[32] = {
	0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B,
	0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
	0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

/* testo della riga y (UTF-8, senza spazi in coda); acs_like: le linee come lettere ACS */
static void row_text(const vt *t, int y, char *out, int cap, const char *expect)
{
	int n, x, len = 0, el = 0;
	const vt_cell *r = vt_row(t, y, &n);

	for (x = 0; r && x < n && len < cap - 8; x++) {
		uint32_t cp = r[x].cp;

		if (r[x].attr & VT_WCONT)
			continue;
		if (cp >= 0x80 && expect) {
			int k;

			/* stessa colonna nel testo atteso (ASCII fino a li') */
			for (k = 0; k < 32; k++)
				if (acs[k] == cp && el < (int)strlen(expect) && expect[el] == 0x5F + k)
					cp = (uint32_t)(0x5F + k);
		}
		len += utf8(cp, out + len);
		/* posizione nel testo atteso: avanza di un carattere UTF-8 */
		if (expect && expect[el]) {
			el++;
			while ((expect[el] & 0xC0) == 0x80)
				el++;
		}
	}
	while (len > 0 && out[len - 1] == ' ')
		len--;
	out[len] = 0;
}

static char *slurp(const char *path, long *n)
{
	FILE *f = fopen(path, "rb");
	char *buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)*n + 1);
	if (!buf || fread(buf, 1, (size_t)*n, f) != (size_t)*n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	buf[*n] = 0;
	fclose(f);
	return buf;
}

static char replies[256];
static int nreplies;

static void on_reply(void *user, const char *s, int n)
{
	(void)user;
	if (nreplies + n < (int)sizeof(replies)) {
		memcpy(replies + nreplies, s, (size_t)n);
		nreplies += n;
		replies[nreplies] = 0;
	}
}

static char title[256];

static void on_title(void *user, const char *s)
{
	(void)user;
	snprintf(title, sizeof(title), "%s", s);
}

/* ---- tela finta: quello che la vista avrebbe disegnato ---- */

typedef struct shadow {
	int cols, rows;
	vt_cell *c;
} shadow;

static void shadow_sync(shadow *s, vt *t)
{
	int y, x;

	if (s->cols != t->cols || s->rows != t->rows) {
		free(s->c);
		s->cols = t->cols;
		s->rows = t->rows;
		s->c = calloc((size_t)s->cols * s->rows, sizeof(vt_cell));
		t->all_dirty = 1;
	}
	if (t->all_dirty) {
		for (y = 0; y < t->rows; y++)
			memcpy(s->c + (size_t)y * s->cols, t->line[y], (size_t)t->cols * sizeof(vt_cell));
		vt_clean(t);
		return;
	}
	if (t->sc_n > 0) {
		int h = t->sc_bot - t->sc_top + 1 - t->sc_n;

		memmove(s->c + (size_t)t->sc_top * s->cols, s->c + (size_t)(t->sc_top + t->sc_n) * s->cols,
			(size_t)h * s->cols * sizeof(vt_cell));
	} else if (t->sc_n < 0) {
		int n = -t->sc_n, h = t->sc_bot - t->sc_top + 1 - n;

		memmove(s->c + (size_t)(t->sc_top + n) * s->cols, s->c + (size_t)t->sc_top * s->cols,
			(size_t)h * s->cols * sizeof(vt_cell));
	}
	for (y = 0; y < t->rows; y++)
		if (t->dirty[y])
			for (x = t->dlo[y]; x <= t->dhi[y]; x++)
				s->c[(size_t)y * s->cols + x] = t->line[y][x];
	vt_clean(t);
}

static int shadow_equal(const shadow *s, const vt *t, int *ry)
{
	int y, x;

	for (y = 0; y < t->rows; y++)
		for (x = 0; x < t->cols; x++) {
			const vt_cell *a = &s->c[(size_t)y * s->cols + x], *b = &t->line[y][x];

			if (a->cp != b->cp || a->fg != b->fg || a->bg != b->bg || a->attr != b->attr) {
				*ry = y;
				return 0;
			}
		}
	return 1;
}

/* ---- flussi registrati ---- */

static void fixture(const char *name)
{
	char path[256], *raw, *txt, *line, *save = NULL, got[1024];
	long nraw, ntxt, ncur;
	int y = 0, cx = -1, cy = -1, bad = 0, round;
	unsigned seed;
	vt t;

	snprintf(path, sizeof(path), "test/vt/%s.raw", name);
	raw = slurp(path, &nraw);
	snprintf(path, sizeof(path), "test/vt/%s.txt", name);
	txt = slurp(path, &ntxt);
	snprintf(path, sizeof(path), "test/vt/%s.cur", name);
	{
		char *cur = slurp(path, &ncur);

		if (cur)
			sscanf(cur, "%d %d", &cx, &cy);
		free(cur);
	}
	CHECK(raw && txt, "%s: file di prova mancanti (test/vt/record.sh)", name);
	if (!raw || !txt) {
		free(raw);
		free(txt);
		return;
	}
	vt_init(&t, 80, 24, 200);
	vt_write(&t, (const uint8_t *)raw, (size_t)nraw);
	for (line = txt; y < 24; y++) {
		char *nl = strchr(line, '\n');

		if (nl)
			*nl = 0;
		row_text(&t, y, got, sizeof(got), line);
		if (strcmp(got, line)) {
			if (!bad++)
				printf("%s riga %d:\n  atteso \"%s\"\n  avuto  \"%s\"\n", name, y, line, got);
		}
		line = nl ? nl + 1 : line + strlen(line);
	}
	CHECK(!bad, "%s: %d righe diverse da tmux", name, bad);
	{
		int x = t.c.x + (t.c.wrapnext ? 1 : 0);

		CHECK(cx < 0 || (x == cx && t.c.y == cy), "%s: cursore %d,%d invece di %d,%d", name, x, t.c.y,
		      cx, cy);
	}
	vt_free(&t);
	(void)save;

	/* a pezzi casuali: la tela aggiornata solo con le celle segnate */
	for (round = 0, seed = 1; round < 6; round++, seed = seed * 1103515245u + 12345u) {
		shadow s = { 0, 0, NULL };
		long off = 0;
		int ok = 1, ry = -1;
		unsigned r = seed;

		vt_init(&t, 80, 24, 50);
		while (off < nraw) {
			long len;

			r = r * 1664525u + 1013904223u;
			len = 1 + (long)((r >> 8) % (round < 3 ? 7u : 300u));
			if (len > nraw - off)
				len = nraw - off;
			vt_write(&t, (const uint8_t *)raw + off, (size_t)len);
			off += len;
			shadow_sync(&s, &t);
			if (!shadow_equal(&s, &t, &ry)) {
				ok = 0;
				break;
			}
		}
		CHECK(ok, "%s: celle cambiate non annotate (giro %d, riga %d, byte %ld)", name, round, ry, off);
		free(s.c);
		vt_free(&t);
	}
	free(raw);
	free(txt);
}

/* ---- sequenze a mano ---- */

static const vt_cell *cell(const vt *t, int x, int y)
{
	return &t->line[y][x];
}

static void text_is(const vt *t, int y, const char *want)
{
	char got[1024];

	row_text(t, y, got, sizeof(got), NULL);
	CHECK(!strcmp(got, want), "riga %d: \"%s\" invece di \"%s\"", y, got, want);
}

static void test_basic(void)
{
	vt t;

	vt_init(&t, 20, 5, 10);
	vt_puts(&t, "ciao\r\nmondo");
	text_is(&t, 0, "ciao");
	text_is(&t, 1, "mondo");
	CHECK(t.c.x == 5 && t.c.y == 1, "cursore %d,%d", t.c.x, t.c.y);
	/* a capo differito: 20 caratteri riempiono la riga senza andare a capo */
	vt_puts(&t, "\r\n01234567890123456789");
	CHECK(t.c.y == 2 && t.c.x == 19 && t.c.wrapnext, "a capo differito");
	vt_puts(&t, "X");
	text_is(&t, 3, "X");
	/* scorrimento: le righe in cima vanno nello storico */
	vt_puts(&t, "\r\na\r\nb\r\nc");
	CHECK(t.sb_count == 2, "storico %d", t.sb_count);
	text_is(&t, -1, "mondo");
	text_is(&t, -2, "ciao");
	text_is(&t, 4, "c");
	/* ED 3 svuota lo storico */
	vt_puts(&t, "\033[3J");
	CHECK(t.sb_count == 0, "storico non svuotato");
	vt_free(&t);
}

static void test_sgr(void)
{
	vt t;
	const vt_cell *c;

	vt_init(&t, 40, 4, 0);
	vt_puts(&t, "\033[1;31mA\033[0;4;38;5;208mB\033[38;2;255;0;0;48:2::0:255:0mC\033[m"
		    "\033[7;9mD\033[27;29;94;103mE\033[39;49;22mF\033[2mG\033[22;3mH\033[0m");
	c = cell(&t, 0, 0);
	CHECK(c->cp == 'A' && (c->attr & VT_BOLD) && c->fg == 1 && c->bg == VT_DEFAULT, "A rosso grassetto");
	c = cell(&t, 1, 0);
	CHECK((c->attr & VT_UNDER) && !(c->attr & VT_BOLD) && c->fg == 208, "B 208 sottolineato");
	c = cell(&t, 2, 0);
	CHECK((c->attr & VT_FG_RGB) && c->fg == 0xF800 && (c->attr & VT_BG_RGB) && c->bg == 0x07E0,
	      "C truecolor fg %04x bg %04x", c->fg, c->bg);
	c = cell(&t, 3, 0);
	CHECK((c->attr & VT_REVERSE) && (c->attr & VT_STRIKE) && c->fg == VT_DEFAULT && !(c->attr & VT_FG_RGB),
	      "D inverso barrato");
	c = cell(&t, 4, 0);
	CHECK(!(c->attr & (VT_REVERSE | VT_STRIKE)) && c->fg == 12 && c->bg == 11, "E colori chiari");
	c = cell(&t, 5, 0);
	CHECK(c->fg == VT_DEFAULT && c->bg == VT_DEFAULT, "F colori di default");
	CHECK(cell(&t, 6, 0)->attr & VT_DIM, "G attenuato");
	CHECK((cell(&t, 7, 0)->attr & VT_ITALIC) && !(cell(&t, 7, 0)->attr & VT_DIM), "H corsivo");
	/* bce: la cancellazione usa il fondo corrente */
	vt_puts(&t, "\r\n\033[44mxy\033[K\033[0m");
	c = cell(&t, 10, 1);
	CHECK(c->cp == ' ' && c->bg == 4 && c->fg == VT_DEFAULT, "bce: fondo %d", c->bg);
	vt_free(&t);
}

static void test_regions(void)
{
	vt t;
	int y;

	vt_init(&t, 10, 6, 10);
	for (y = 0; y < 6; y++) {
		char b[40];

		snprintf(b, sizeof(b), "\033[%d;1Hr%d", y + 1, y);
		vt_puts(&t, b);
	}
	/* regione 2..4: un a capo in fondo alla regione scorre solo lei */
	vt_puts(&t, "\033[2;4r\033[4;1H\n");
	text_is(&t, 0, "r0");
	text_is(&t, 1, "r2");
	text_is(&t, 2, "r3");
	text_is(&t, 3, "");
	text_is(&t, 4, "r4");
	CHECK(t.sb_count == 0, "regione senza la prima riga: niente storico");
	/* RI in cima alla regione la fa scendere */
	vt_puts(&t, "\033[2;1H\033M");
	text_is(&t, 1, "");
	text_is(&t, 2, "r2");
	/* IL e DL dentro la regione */
	vt_puts(&t, "\033[r\033[1;1H\033[2L");
	text_is(&t, 0, "");
	text_is(&t, 2, "r0");
	vt_puts(&t, "\033[1;1H\033[2M");
	text_is(&t, 0, "r0");
	/* ICH, DCH, ECH */
	vt_puts(&t, "\033[6;1H\033[2KABCDEF\033[6;2H\033[2@");
	text_is(&t, 5, "A  BCDEF");
	vt_puts(&t, "\033[6;2H\033[3P");
	text_is(&t, 5, "ACDEF");
	vt_puts(&t, "\033[6;1H\033[2X");
	text_is(&t, 5, "  DEF");
	/* modo origine: CUP relativo alla regione */
	vt_puts(&t, "\033[3;5r\033[?6h\033[1;1HO\033[?6l");
	CHECK(cell(&t, 0, 2)->cp == 'O', "DECOM");
	vt_free(&t);
}

static void test_alt_and_modes(void)
{
	vt t;
	char k[16];
	int n;

	vt_init(&t, 20, 5, 10);
	vt_puts(&t, "principale\033[3;4H");
	vt_puts(&t, "\033[?1049h\033[Halternato");
	CHECK(t.alt, "schermo alternativo");
	text_is(&t, 0, "alternato");
	vt_puts(&t, "\033[?1049l");
	CHECK(!t.alt && t.c.x == 3 && t.c.y == 2, "cursore ripristinato %d,%d", t.c.x, t.c.y);
	text_is(&t, 0, "principale");
	/* DECCKM: frecce in modo applicazione */
	n = vt_key(&t, VT_K_UP, 0, k);
	CHECK(n == 3 && !memcmp(k, "\033[A", 3), "freccia normale");
	vt_puts(&t, "\033[?1h");
	n = vt_key(&t, VT_K_UP, 0, k);
	CHECK(n == 3 && !memcmp(k, "\033OA", 3), "freccia DECCKM");
	n = vt_key(&t, VT_K_RIGHT, VT_MOD_CTRL, k);
	CHECK(n == 6 && !memcmp(k, "\033[1;5C", 6), "Ctrl+destra");
	n = vt_key(&t, VT_K_DELETE, 0, k);
	CHECK(n == 4 && !memcmp(k, "\033[3~", 4), "Canc");
	n = vt_key(&t, VT_K_PGUP, VT_MOD_SHIFT, k);
	CHECK(n == 6 && !memcmp(k, "\033[5;2~", 6), "Shift+PagSu");
	n = vt_key(&t, VT_K_BACKSPACE, 0, k);
	CHECK(n == 1 && k[0] == 0x7F, "Backspace = DEL");
	n = vt_key(&t, VT_K_TAB, VT_MOD_SHIFT, k);
	CHECK(n == 3 && !memcmp(k, "\033[Z", 3), "Shift+Tab");
	n = vt_char('c', VT_MOD_CTRL, k);
	CHECK(n == 1 && k[0] == 3, "Ctrl+c");
	n = vt_char('[', VT_MOD_CTRL, k);
	CHECK(n == 1 && k[0] == 0x1B, "Ctrl+[");
	n = vt_char('x', VT_MOD_ALT, k);
	CHECK(n == 2 && k[0] == 0x1B && k[1] == 'x', "Alt+x");
	n = vt_char(0xE8, 0, k);
	CHECK(n == 2 && (unsigned char)k[0] == 0xC3 && (unsigned char)k[1] == 0xA8, "è in UTF-8");
	/* risposte */
	t.reply = on_reply;
	nreplies = 0;
	vt_puts(&t, "\033[5;7H\033[6n\033[c\033[>c\033[5n");
	CHECK(!strcmp(replies, "\033[5;7R\033[?62;22c\033[>1;10;0c\033[0n"), "risposte: %s", replies + 1);
	/* titolo con BEL e con ST */
	t.on_title = on_title;
	vt_puts(&t, "\033]0;utente@brain: ~\007");
	CHECK(!strcmp(title, "utente@brain: ~"), "titolo %s", title);
	vt_puts(&t, "\033]2;secondo\033\\dopo");
	CHECK(!strcmp(title, "secondo"), "titolo con ST %s", title);
	CHECK(cell(&t, 6, 4)->cp == 'd', "testo dopo ST");
	/* incolla tra parentesi e mouse: annotati, nessun effetto sullo schermo */
	vt_puts(&t, "\033[?2004h\033[?1000h\033[?1006h\033P+q544e\033\\");
	CHECK(t.m_bracket && t.m_mouse, "2004/1000");
	/* inserimento */
	vt_puts(&t, "\033[1;1H\033[4hXY\033[4l");
	text_is(&t, 0, "XYprincipale");
	vt_free(&t);
}

static void test_utf8(void)
{
	vt t;
	const vt_cell *c;

	vt_init(&t, 10, 3, 0);
	vt_puts(&t, "città\xE2\x82\xAC");
	CHECK(cell(&t, 4, 0)->cp == 0xE0 && cell(&t, 5, 0)->cp == 0x20AC, "accentate ed euro");
	/* byte non validi: carattere sostitutivo */
	vt_puts(&t, "\r\n\xC3(\xFF\xE2\x82");
	CHECK(cell(&t, 0, 1)->cp == 0xFFFD && cell(&t, 1, 1)->cp == '(' && cell(&t, 2, 1)->cp == 0xFFFD,
	      "UTF-8 non valido");
	vt_puts(&t, "\033[3;1H\xE6\x97\xA5\xE6\x9C\xAC\xCC\x81!");	/* 日本 + combinante */
	c = cell(&t, 0, 2);
	CHECK((c->attr & VT_WIDE) && (cell(&t, 1, 2)->attr & VT_WCONT) && cell(&t, 2, 2)->cp == 0x672C &&
	      cell(&t, 4, 2)->cp == '!', "caratteri larghi");
	/* largo sull'ultima colonna: va a capo */
	vt_puts(&t, "\033[1;10H\xE6\x97\xA5");
	CHECK(cell(&t, 9, 0)->cp == ' ' && cell(&t, 0, 1)->cp == 0x65E5, "largo a fine riga");
	/* sovrascrivere meta' di un carattere largo svuota l'altra meta' */
	vt_puts(&t, "\033[3;2Hx");
	CHECK(cell(&t, 0, 2)->cp == ' ' && !(cell(&t, 0, 2)->attr & VT_WIDE), "meta' di un largo");
	/* set grafico DEC */
	vt_puts(&t, "\033[2;1H\033(0lqk\033(Bq");
	CHECK(cell(&t, 0, 1)->cp == 0x250C && cell(&t, 1, 1)->cp == 0x2500 && cell(&t, 3, 1)->cp == 'q',
	      "DEC grafico");
	/* SO/SI con G1 grafico */
	vt_puts(&t, "\033)0\016x\017x");
	CHECK(cell(&t, 4, 1)->cp == 0x2502 && cell(&t, 5, 1)->cp == 'x', "SO/SI");
	/* REP */
	vt_puts(&t, "\033[1;1HZ\033[3b");
	CHECK(cell(&t, 3, 0)->cp == 'Z', "REP");
	vt_free(&t);
}

static void test_resize_and_scroll_accounting(void)
{
	vt t;
	int y;

	vt_init(&t, 20, 10, 100);
	for (y = 0; y < 30; y++) {
		char b[32];

		snprintf(b, sizeof(b), "riga %d\r\n", y);
		vt_puts(&t, b);
	}
	CHECK(t.sb_count == 21, "storico %d", t.sb_count);
	vt_clean(&t);
	/* tre a capo in fondo: uno scorrimento in attesa di 3 righe, le nuove segnate */
	vt_puts(&t, "a\r\nb\r\nc\r\nd");
	CHECK(t.sc_n == 3 && t.sc_top == 0 && t.sc_bot == 9, "scorrimento sommato %d", t.sc_n);
	CHECK(t.dirty[6] && t.dirty[9] && !t.dirty[2], "righe nuove segnate");
	/* meno righe: il cursore resta in vista */
	vt_resize(&t, 12, 5);
	CHECK(t.c.y == 4 && t.cols == 12 && t.all_dirty, "ridimensionato, cursore %d", t.c.y);
	text_is(&t, 4, "d");
	text_is(&t, 0, "riga 29");
	text_is(&t, -1, "riga 28");
	/* piu' colonne: lo storico si allunga */
	vt_resize(&t, 30, 8);
	text_is(&t, -2, "riga 27");
	text_is(&t, 3, "c");
	vt_free(&t);
	/* ruotando avanti e indietro le righe dello storico restano intere */
	vt_init(&t, 20, 3, 10);
	vt_puts(&t, "0123456789abcdefghij\r\nx\r\ny\r\nz");
	vt_resize(&t, 10, 3);
	text_is(&t, -1, "0123456789abcdefghij");
	vt_resize(&t, 20, 3);
	text_is(&t, -1, "0123456789abcdefghij");
	vt_free(&t);
}

int main(void)
{
	static const char *const names[] = { "bash", "top", "htop", "vim", "vimfile", "nano", "less",
					     "apt", "frag" };
	int i;

	test_basic();
	test_sgr();
	test_regions();
	test_alt_and_modes();
	test_utf8();
	test_resize_and_scroll_accounting();
	for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
		fixture(names[i]);
	{
		/* attributi del prompt di bash registrato */
		long n;
		char *raw = slurp("test/vt/bash.raw", &n);
		vt t;

		if (raw) {
			vt_init(&t, 80, 24, 0);
			t.on_title = on_title;
			vt_write(&t, (const uint8_t *)raw, (size_t)n);
			CHECK(cell(&t, 0, 0)->fg == 2 && (cell(&t, 0, 0)->attr & VT_BOLD), "prompt verde grassetto");
			CHECK(!strncmp(title, "root@", 5), "titolo dal prompt: %s", title);
			/* ls --color: le cartelle in blu grassetto */
			CHECK(cell(&t, 0, 4)->fg == 4 && (cell(&t, 0, 4)->attr & VT_BOLD), "cartella blu");
			CHECK(cell(&t, 0, 7)->fg == 1 && (cell(&t, 0, 7)->attr & VT_BOLD), "rosso");
			CHECK(cell(&t, 6, 7)->fg == 208, "256 colori");
			CHECK((cell(&t, 10, 7)->attr & VT_FG_RGB), "truecolor");
			CHECK((cell(&t, 15, 7)->attr & VT_REVERSE), "inverso");
			vt_free(&t);
			free(raw);
		}
	}
	printf("vt_test: %d controlli, %d falliti\n", checks, fails);
	return fails != 0;
}
