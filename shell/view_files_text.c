// SPDX-License-Identifier: MIT
/*
 * view_files_text.c - lettore di testi di Esplora (sola lettura).
 *
 * Il filo di lavoro legge al massimo il primo MB, lo ripulisce (UTF-8 valido,
 * tabulazioni espanse, niente caratteri di controllo; i byte che non sono
 * UTF-8 vengono letti come Latin-1) e calcola dove andare a capo per la
 * larghezza richiesta. Il filo dell'interfaccia riceve il testo gia' diviso
 * in righe e lo mostra con una bui_list: una riga di testo per riga della
 * lista, quindi si disegnano solo le righe visibili.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bs_work.h"
#include "view_files.h"

#define FONT BUI_FONT_BODY
#define LINE_H 26
#define PAD 16
#define MAX_LINE_BYTES 1000

struct line {
	uint32_t off;
	uint16_t len;
};

struct text {
	char *buf;
	size_t len;
	struct line *ln;
	int nlines;
	int width;
	long long file_size;
	int truncated;
};

struct tjob {
	char path[1024];
	int width, sniff, result, gen;
	struct text t;
	void (*done)(int, void *);
	void *user;
};

static struct {
	struct text t;
	int have, job, gen;
	bui_panel panel;
	bui_list list;
	bui_rect area;
	int shown;
} T;

void bs_fmt_size(char *buf, int len, long long b)
{
	static const char *const unit[] = { "kB", "MB", "GB", "TB" };
	long long whole = b;
	int u = -1, tenth = 0;

	if (b < 1000) {
		snprintf(buf, (size_t)len, "%lld byte", b);
		return;
	}
	while (whole >= 1000 && u < 3) {
		tenth = (int)(whole % 1000) / 100;
		whole /= 1000;
		u++;
	}
	if (whole < 100)
		snprintf(buf, (size_t)len, "%lld,%d %s", whole, tenth, unit[u]);
	else
		snprintf(buf, (size_t)len, "%lld %s", whole, unit[u]);
}

/* ------------------------------------------------------------------ */
/* filo di lavoro                                                     */
/* ------------------------------------------------------------------ */

static void text_free(struct text *t)
{
	free(t->buf);
	free(t->ln);
	memset(t, 0, sizeof(*t));
}

/* lunghezza di una sequenza UTF-8 valida all'inizio di p (0 se non lo e') */
static int utf8_len(const unsigned char *p, const unsigned char *end)
{
	int n, i;

	if (*p < 0x80)
		return 1;
	if (*p >= 0xC2 && *p <= 0xDF)
		n = 2;
	else if (*p >= 0xE0 && *p <= 0xEF)
		n = 3;
	else if (*p >= 0xF0 && *p <= 0xF4)
		n = 4;
	else
		return 0;
	if (end - p < n)
		return 0;
	for (i = 1; i < n; i++)
		if ((p[i] & 0xC0) != 0x80)
			return 0;
	return n;
}

static int looks_binary(const unsigned char *p, size_t n)
{
	size_t i, odd = 0, lim = n < 4096 ? n : 4096;

	for (i = 0; i < lim; i++) {
		if (!p[i])
			return 1;
		if (p[i] < 0x20 && p[i] != '\n' && p[i] != '\r' && p[i] != '\t' && p[i] != '\f' &&
		    p[i] != 0x1B)
			odd++;
	}
	return odd * 20 > lim;	/* piu' del 5% di caratteri di controllo */
}

/* ripulisce raw in un nuovo buffer UTF-8; ritorna 0 se riuscito */
static int sanitize(struct text *t, const unsigned char *raw, size_t n)
{
	const unsigned char *p, *end = raw + n;
	size_t need = 1;
	char *o;
	int k;

	for (p = raw; p < end;) {
		if (*p == '\t') {
			need += 4;
			p++;
		} else if ((k = utf8_len(p, end)) > 0) {
			need += (size_t)k;
			p += k;
		} else {
			need += 2;
			p++;
		}
	}
	t->buf = o = malloc(need);
	if (!o)
		return -1;
	for (p = raw; p < end;) {
		if (*p == '\t') {
			memcpy(o, "    ", 4);
			o += 4;
			p++;
		} else if (*p < 0x20 || *p == 0x7F) {
			if (*p == '\n')
				*o++ = '\n';
			p++;
		} else if ((k = utf8_len(p, end)) > 0) {
			memcpy(o, p, (size_t)k);
			o += k;
			p += k;
		} else if (*p >= 0xA0) {	/* Latin-1 */
			*o++ = (char)(0xC0 | (*p >> 6));
			*o++ = (char)(0x80 | (*p & 0x3F));
			p++;
		} else {
			*o++ = '?';
			p++;
		}
	}
	*o = 0;
	t->len = (size_t)(o - t->buf);
	return 0;
}

/* avanzamento (1/64 px) del carattere che inizia in *pp; avanza *pp */
static int glyph_adv(const bui_font *f, const unsigned char **pp, const unsigned char *end)
{
	const unsigned char *p = *pp;
	unsigned cp = *p;
	int n = utf8_len(p, end), i, lo, hi;

	if (n <= 1) {
		n = 1;
	} else {
		cp &= 0xFF >> (n + 1);
		for (i = 1; i < n; i++)
			cp = (cp << 6) | (p[i] & 0x3F);
	}
	*pp = p + n;
	if (cp >= 0x20 && cp - 0x20 < f->nglyphs && f->glyphs[cp - 0x20].cp == cp)
		return f->glyphs[cp - 0x20].adv;
	for (lo = 0, hi = f->nglyphs - 1; lo <= hi;) {
		int mid = (lo + hi) >> 1;

		if (f->glyphs[mid].cp == cp)
			return f->glyphs[mid].adv;
		if (f->glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return f->glyphs['?' - 0x20].adv;	/* bui disegna '?' per i glifi mancanti */
}

static int add_line(struct text *t, int *cap, size_t off, size_t len)
{
	if (t->nlines >= BS_TEXT_MAX_LINES) {
		t->truncated = 1;
		return -1;
	}
	if (t->nlines == *cap) {
		int nc = *cap ? *cap * 2 : 256;
		struct line *nl = realloc(t->ln, sizeof(*nl) * (size_t)nc);

		if (!nl)
			return -1;
		t->ln = nl;
		*cap = nc;
	}
	t->ln[t->nlines].off = (uint32_t)off;
	t->ln[t->nlines].len = (uint16_t)len;
	t->nlines++;
	return 0;
}

/* divide il testo in righe che entrano in t->width pixel */
static void wrap(struct text *t, const volatile int *cancel)
{
	const unsigned char *b = (const unsigned char *)t->buf, *end = b + t->len;
	const unsigned char *p = b, *start = b, *brk = NULL;
	int maxw = t->width << 6, w = 0, cap = 0, word = 0;

	while (p < end && !*cancel) {
		const unsigned char *q = p;
		int adv;

		if (*p == '\n') {
			if (add_line(t, &cap, (size_t)(start - b), (size_t)(p - start)) < 0)
				return;
			start = ++p;
			brk = NULL;
			w = word = 0;
			continue;
		}
		adv = glyph_adv(FONT, &q, end);
		if ((w + adv > maxw && p > start) || q - start > MAX_LINE_BYTES) {
			const unsigned char *cut = brk ? brk : p;

			if (add_line(t, &cap, (size_t)(start - b), (size_t)(cut - start)) < 0)
				return;
			start = cut;
			while (start < end && *start == ' ')
				start++;	/* gli spazi dell'a capo non aprono la riga nuova */
			p = start;
			brk = NULL;
			w = word = 0;
			continue;
		}
		w += adv;
		if (*p != ' ')
			word = 1;
		else if (word)
			brk = p;	/* i rientri in testa alla riga non sono punti di a capo */
		p = q;
	}
	if (p > start || !t->nlines)
		add_line(t, &cap, (size_t)(start - b), (size_t)(p - start));
}

static void load_run(void *arg, const volatile int *cancel)
{
	struct tjob *j = arg;
	unsigned char *raw;
	size_t n;
	FILE *f = fopen(j->path, "rb");

	j->result = BS_TEXT_E_OPEN;
	if (!f)
		return;
	raw = malloc(BS_TEXT_MAX_BYTES + 1);
	if (!raw) {
		fclose(f);
		j->result = BS_TEXT_E_MEM;
		return;
	}
	n = fread(raw, 1, BS_TEXT_MAX_BYTES + 1, f);
	if (ferror(f)) {
		fclose(f);
		free(raw);
		return;
	}
	j->t.file_size = (long long)n;
	if (n > BS_TEXT_MAX_BYTES) {
		n = BS_TEXT_MAX_BYTES;
		j->t.truncated = 1;
		if (!fseek(f, 0, SEEK_END))
			j->t.file_size = (long long)ftell(f);
		/* non si taglia a meta' di un carattere */
		while (n > 0 && (raw[n - 1] & 0xC0) == 0x80)
			n--;
		if (n > 0 && raw[n - 1] >= 0xC0)
			n--;
	}
	fclose(f);
	if (j->sniff && looks_binary(raw, n)) {
		free(raw);
		j->result = BS_TEXT_E_BINARY;
		return;
	}
	j->t.width = j->width;
	if (*cancel || sanitize(&j->t, raw, n) < 0) {
		free(raw);
		j->result = BS_TEXT_E_MEM;
		return;
	}
	free(raw);
	wrap(&j->t, cancel);
	j->result = j->t.ln ? BS_TEXT_OK : BS_TEXT_E_MEM;
}

static void load_done(void *arg, int cancelled)
{
	struct tjob *j = arg;

	if (cancelled || j->gen != T.gen) {
		text_free(&j->t);
		free(j);
		return;
	}
	T.job = 0;
	if (j->result == BS_TEXT_OK) {
		text_free(&T.t);
		T.t = j->t;
		T.have = 1;
	} else {
		text_free(&j->t);
	}
	j->done(j->result, j->user);
	free(j);
}

/* ------------------------------------------------------------------ */
/* filo principale                                                    */
/* ------------------------------------------------------------------ */

int bs_text_width_for(int area_w)
{
	return area_w - 2 * PAD - 8;
}

void bs_text_load(const char *path, int width, int sniff, void (*done)(int, void *), void *user)
{
	struct tjob *j;

	bs_work_cancel(T.job);
	T.job = 0;
	T.gen++;
	j = calloc(1, sizeof(*j));
	if (j) {
		snprintf(j->path, sizeof(j->path), "%s", path);
		j->width = width < 64 ? 64 : width;
		j->sniff = sniff;
		j->gen = T.gen;
		j->done = done;
		j->user = user;
		T.job = bs_work_submit(load_run, load_done, j, 1);
		if (T.job)
			return;
		free(j);
	}
	done(BS_TEXT_E_MEM, user);
}

void bs_text_close(void)
{
	bs_work_cancel(T.job);
	T.job = 0;
	T.gen++;
	if (T.shown)
		bui_panel_clear(&T.panel);
	T.shown = 0;
	text_free(&T.t);
	T.have = 0;
}

static void draw_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	char tmp[MAX_LINE_BYTES + 8];
	const struct line *ln;

	(void)l;
	(void)flags;
	if (i < 0 || i >= T.t.nlines)
		return;
	ln = &T.t.ln[i];
	if (!ln->len || ln->len > MAX_LINE_BYTES + 4)
		return;
	memcpy(tmp, T.t.buf + ln->off, ln->len);
	tmp[ln->len] = 0;
	bui_text(c, FONT, r.x + PAD, r.y + (LINE_H + FONT->cap_h) / 2, tmp, BUI_C_TEXT);
}

static void build(void)
{
	int keep = T.list.sc.pos;

	bui_panel_clear(&T.panel);
	bui_panel_set_viewport(&T.panel, T.area, T.area.h);
	T.list.count = T.t.nlines;
	T.list.row_h = LINE_H;
	T.list.draw_row = draw_row;
	T.list.on_select = NULL;
	T.list.sel = -1;
	T.list.empty_text = "Il file \xC3\xA8 vuoto";
	if (T.t.nlines == 1 && !T.t.ln[0].len)
		T.list.count = 0;
	bui_add_list(&T.panel, bui_r(0, 8, T.area.w, T.area.h - 8), &T.list)->flags |= BUI_F_NOFOCUS;
	T.list.sc.pos = keep > T.list.sc.max ? T.list.sc.max : keep;
}

void bs_text_show(bui_win *win, bui_rect area)
{
	if (!T.have)
		return;
	bui_panel_init(&T.panel, win);
	memset(&T.list, 0, sizeof(T.list));
	T.area = area;
	T.shown = 1;
	build();
}

int bs_text_layout(bui_rect area)
{
	if (!T.shown)
		return 1;
	T.area = area;
	build();
	return bs_text_width_for(area.w) == T.t.width;
}

void bs_text_draw(bui_canvas *c, bui_rect clip)
{
	if (T.shown)
		bui_panel_draw(&T.panel, c, clip);
}

int bs_text_pointer(const bui_pointer *ev)
{
	return T.shown ? bui_panel_pointer(&T.panel, ev) : 0;
}

static void scroll_by(int dy)
{
	T.list.sc.fling = 0;
	if (bui_scroll_by(&T.list.sc, dy) && T.list.w)
		bui_widget_damage(T.list.w);
}

int bs_text_key(const bui_key *k)
{
	int page;

	if (!T.shown || !T.list.w)
		return 0;
	page = T.list.w->r.h - 2 * LINE_H;
	switch (k->key) {
	case BUI_K_UP:
		scroll_by(-3 * LINE_H);
		return 1;
	case BUI_K_DOWN:
		scroll_by(3 * LINE_H);
		return 1;
	case BUI_K_PGUP:
		scroll_by(-page);
		return 1;
	case BUI_K_PGDN:
	case ' ':
		scroll_by(page);
		return 1;
	case BUI_K_HOME:
		scroll_by(-T.list.sc.max);
		return 1;
	case BUI_K_END:
		scroll_by(T.list.sc.max);
		return 1;
	}
	return 0;
}

void bs_text_info(char *buf, int len)
{
	char sz[32];

	bs_fmt_size(sz, sizeof(sz), T.t.file_size);
	snprintf(buf, (size_t)len, "%s  \xC2\xB7  %s", sz,
		 T.t.truncated ? "mostrato solo l'inizio" : "sola lettura");
}
