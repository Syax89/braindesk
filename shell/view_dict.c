// SPDX-License-Identifier: MIT
/*
 * view_dict.c - Dizionario: italiano <-> inglese, inglese <-> giapponese.
 *
 * Dati: dizionari FreeDict (GPL / CC BY-SA) convertiti da
 * dict-tools/mkdict.py in /usr/local/share/brain/dict/: NOME.dict (testo),
 * NOME.idx (righe "chiave\tvoce\tposizione\tlunghezza" ordinate) e NOME.ofs
 * (posizione di ogni riga). Tutto e' mappato in memoria: la ricerca per
 * prefisso e' una ricerca binaria su .ofs, anche sulle 376.000 voci del
 * giapponese, senza caricare niente.
 *
 * Si scrive con la tastiera; per il giapponese le lettere diventano kana
 * (romaji: "inu" -> いぬ). Invio apre la prima voce, Esc torna indietro.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

#define DICT_DIR "/usr/local/share/brain/dict"
#define MAXRES 40

enum { PG_SEARCH, PG_ENTRY };

static const struct {
	const char *file, *label;
	int kana;		/* la ricerca converte il romaji in kana */
} dicts[] = {
	{ "ita-eng", "IT \xE2\x86\x92 EN", 0 },
	{ "eng-ita", "EN \xE2\x86\x92 IT", 0 },
	{ "eng-jpn", "EN \xE2\x86\x92 \xE6\x97\xA5\xE6\x9C\xAC", 0 },
	{ "jpn-eng", "\xE6\x97\xA5\xE6\x9C\xAC \xE2\x86\x92 EN", 1 },
};
#define NDICTS ((int)(sizeof(dicts) / sizeof(dicts[0])))

struct map {
	const char *p;
	size_t n;
};

static struct {
	bui_panel p;
	bui_rect area;
	int page, cur;		/* pagina, dizionario */
	struct map dict, idx, ofs;
	int ok;
	char q[64];		/* quello che si e' scritto */
	char key[192];		/* chiave di ricerca (kana per il giapponese) */
	int nres;
	char head[MAXRES][64], prev[MAXRES][96];
	uint32_t off[MAXRES], len[MAXRES];
	char title[96], *body;
	char opts[96];		/* etichette del selettore (il widget non le copia) */
} V;

/* ------------------------------------------------------------------ */
/* dati                                                               */
/* ------------------------------------------------------------------ */

static void unmap(struct map *m)
{
	if (m->p)
		munmap((void *)m->p, m->n);
	m->p = NULL;
	m->n = 0;
}

static int map_file(struct map *m, const char *name, const char *ext)
{
	char path[256];
	struct stat st;
	void *p;
	int fd;

	snprintf(path, sizeof(path), "%s/%s.%s", DICT_DIR, name, ext);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || st.st_size <= 0) {
		close(fd);
		return -1;
	}
	p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return -1;
	m->p = p;
	m->n = (size_t)st.st_size;
	return 0;
}

static void open_dict(int i)
{
	unmap(&V.dict);
	unmap(&V.idx);
	unmap(&V.ofs);
	V.cur = i;
	V.ok = map_file(&V.dict, dicts[i].file, "dict") == 0 && map_file(&V.idx, dicts[i].file, "idx") == 0 &&
	       map_file(&V.ofs, dicts[i].file, "ofs") == 0;
}

static uint32_t nlines(void)
{
	return (uint32_t)(V.ofs.n / 4);
}

static const char *line_at(uint32_t i)
{
	uint32_t o;

	memcpy(&o, V.ofs.p + (size_t)i * 4, 4);
	return o < V.idx.n ? V.idx.p + o : V.idx.p;
}

/* confronta la chiave della riga con il prefisso cercato: <0, 0 (inizia cosi'), >0 */
static int cmp_prefix(const char *line, const char *q, size_t ql)
{
	size_t i;

	for (i = 0; i < ql; i++) {
		unsigned char a = (unsigned char)line[i], b = (unsigned char)q[i];

		if (a == '\t' || a == '\n')
			return -1;	/* chiave piu' corta del prefisso */
		if (a != b)
			return a < b ? -1 : 1;
	}
	return 0;
}

/* anteprima: la prima riga della definizione dopo quella della voce */
static void preview(uint32_t off, uint32_t len, char *out, int outlen)
{
	const char *s, *e, *nl;
	int n;

	out[0] = 0;
	if ((size_t)off + len > V.dict.n)
		return;
	s = V.dict.p + off;
	e = s + len;
	nl = memchr(s, '\n', (size_t)(e - s));
	while (nl && nl + 1 < e) {
		s = nl + 1;
		nl = memchr(s, '\n', (size_t)(e - s));
		n = (int)((nl ? nl : e) - s);
		while (n && (*s == ' ' || *s == '\t'))
			s++, n--;
		/* "2. ": il numero del significato non serve nell'anteprima */
		if (n > 2 && s[0] >= '0' && s[0] <= '9') {
			int k = 0;

			while (k < n && s[k] >= '0' && s[k] <= '9')
				k++;
			if (k < n && s[k] == '.') {
				k++;
				while (k < n && s[k] == ' ')
					k++;
				s += k;
				n -= k;
			}
		}
		/* "(noun (common) ...)": categoria grammaticale, si cerca il significato */
		if (n > 0 && s[0] == '(' && s[n - 1] == ')')
			continue;
		if (n > 0) {
			if (n > outlen - 1)
				n = outlen - 1;
			/* non tagliare a meta' un carattere UTF-8 */
			while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
				n--;
			memcpy(out, s, (size_t)n);
			out[n] = 0;
			return;
		}
	}
}

static void search(void)
{
	size_t ql = strlen(V.key);
	uint32_t lo = 0, hi = nlines(), i;

	V.nres = 0;
	if (!V.ok || !ql)
		return;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (cmp_prefix(line_at(mid), V.key, ql) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (i = lo; i < nlines() && V.nres < MAXRES; i++) {
		const char *l = line_at(i), *t1, *t2;
		unsigned long o, n;

		if (cmp_prefix(l, V.key, ql) != 0)
			break;
		t1 = strchr(l, '\t');
		t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (!t2 || sscanf(t2 + 1, "%lu\t%lu", &o, &n) != 2)
			continue;
		snprintf(V.head[V.nres], sizeof(V.head[0]), "%.*s", (int)(t2 - t1 - 1), t1 + 1);
		V.off[V.nres] = (uint32_t)o;
		V.len[V.nres] = (uint32_t)n;
		preview(V.off[V.nres], V.len[V.nres], V.prev[V.nres], sizeof(V.prev[0]));
		V.nres++;
	}
}

/* ------------------------------------------------------------------ */
/* romaji -> hiragana                                                 */
/* ------------------------------------------------------------------ */

static const char *const kana[][2] = {
	{ "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" }, { "gya", "ぎゃ" }, { "gyu", "ぎゅ" },
	{ "gyo", "ぎょ" }, { "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" }, { "shi", "し" },
	{ "sya", "しゃ" }, { "syu", "しゅ" }, { "syo", "しょ" }, { "cha", "ちゃ" }, { "chu", "ちゅ" },
	{ "cho", "ちょ" }, { "chi", "ち" }, { "tya", "ちゃ" }, { "tyu", "ちゅ" }, { "tyo", "ちょ" },
	{ "tsu", "つ" }, { "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" }, { "hya", "ひゃ" },
	{ "hyu", "ひゅ" }, { "hyo", "ひょ" }, { "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" },
	{ "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" }, { "bya", "びゃ" }, { "byu", "びゅ" },
	{ "byo", "びょ" }, { "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" }, { "jya", "じゃ" },
	{ "jyu", "じゅ" }, { "jyo", "じょ" },
	{ "ja", "じゃ" }, { "ju", "じゅ" }, { "jo", "じょ" }, { "ji", "じ" }, { "fu", "ふ" },
	{ "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
	{ "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
	{ "sa", "さ" }, { "si", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
	{ "za", "ざ" }, { "zi", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
	{ "ta", "た" }, { "ti", "ち" }, { "tu", "つ" }, { "te", "て" }, { "to", "と" },
	{ "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
	{ "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
	{ "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
	{ "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
	{ "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
	{ "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
	{ "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
	{ "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
	{ "wa", "わ" }, { "wo", "を" }, { "nn", "ん" }, { "n'", "ん" },
	{ "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" }, { "-", "ー" },
};

static int is_vowel(char c)
{
	return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

/* converte cio' che si puo'; l'ultima sillaba incompleta resta in lettere */
static void romaji(const char *in, char *out, size_t outlen)
{
	size_t o = 0;

	out[0] = 0;
	while (*in && o + 8 < outlen) {
		size_t k, best = 0;
		const char *rep = NULL;

		/* consonante doppia: piccola tsu */
		if (in[0] == in[1] && !is_vowel(in[0]) && in[0] != 'n' && in[0] >= 'a' && in[0] <= 'z') {
			memcpy(out + o, "っ", 3);
			o += 3;
			in++;
			continue;
		}
		/* n davanti a consonante (non y) o in fondo dopo altre sillabe */
		if (in[0] == 'n' && in[1] && !is_vowel(in[1]) && in[1] != 'y' && in[1] != 'n' && in[1] != '\'') {
			memcpy(out + o, "ん", 3);
			o += 3;
			in++;
			continue;
		}
		for (k = 0; k < sizeof(kana) / sizeof(kana[0]); k++) {
			size_t l = strlen(kana[k][0]);

			if (l > best && !strncmp(in, kana[k][0], l)) {
				best = l;
				rep = kana[k][1];
			}
		}
		if (!rep) {
			out[o++] = *in++;	/* non (ancora) convertibile */
			continue;
		}
		memcpy(out + o, rep, strlen(rep));
		o += strlen(rep);
		in += best;
	}
	out[o] = 0;
}

static void update(void)
{
	char *s;

	if (dicts[V.cur].kana)
		romaji(V.q, V.key, sizeof(V.key));
	else
		snprintf(V.key, sizeof(V.key), "%s", V.q);
	for (s = V.key; *s; s++)
		if (*s >= 'A' && *s <= 'Z')
			*s += 32;
	search();
}

/* ------------------------------------------------------------------ */
/* pagine                                                             */
/* ------------------------------------------------------------------ */

static void build(void);

static void go(int page)
{
	V.page = page;
	build();
	bui_panel_damage(&V.p);
}

static void on_back(bui_widget *w)
{
	(void)w;
	if (V.page == PG_ENTRY)
		go(PG_SEARCH);
	else
		bs_view_close();
}

static void on_dict(bui_widget *w, int final)
{
	if (!final || w->value == V.cur)
		return;
	open_dict(w->value);
	update();
	go(PG_SEARCH);
}

/* prima riga della voce: via le etichette [ichi1] e le pronunce /.../
 * (simboli IPA che i font non hanno), spazi in eccesso compresi */
static void clean_head(char *b)
{
	char *nl = strchr(b, '\n'), *r = b, *w = b, *end = nl ? nl : b + strlen(b);

	while (r < end) {
		char close = *r == '[' ? ']' : *r == '/' ? '/' : 0;
		char *q = close ? memchr(r + 1, close, (size_t)(end - r - 1)) : NULL;

		if (q) {
			r = q + 1;
			continue;
		}
		if (*r == ' ' && (w == b || w[-1] == ' ' || r[1] == ',')) {
			r++;
			continue;
		}
		*w++ = *r++;
	}
	while (w > b && (w[-1] == ' ' || w[-1] == ','))
		w--;
	memmove(w, end, strlen(end) + 1);
	/* una riga rimasta vuota non serve */
	if (*b == '\n')
		memmove(b, b + 1, strlen(b));
}

static void open_entry(int i)
{
	size_t n;
	const char *s;

	if (i < 0 || i >= V.nres || (size_t)V.off[i] + V.len[i] > V.dict.n)
		return;
	snprintf(V.title, sizeof(V.title), "%s", V.head[i]);
	free(V.body);
	s = V.dict.p + V.off[i];
	n = V.len[i];
	V.body = malloc(n + 1);
	if (!V.body)
		return;
	memcpy(V.body, s, n);
	V.body[n] = 0;
	while (n && (V.body[n - 1] == '\n' || V.body[n - 1] == ' '))
		V.body[--n] = 0;
	clean_head(V.body);
	go(PG_ENTRY);
}

static void on_result(bui_widget *w)
{
	open_entry(w->id);
}

/* il campo di ricerca: quello che si e' scritto, con il cursore */
static void draw_field(bui_widget *w, bui_canvas *c, bui_rect r)
{
	const char *shown = dicts[V.cur].kana ? V.key : V.q;
	int tw;

	(void)w;
	bui_rrect(c, r, r.h / 2, BUI_C_SURFACE2);
	bui_rrect_line(c, r, r.h / 2, 2, BUI_C_ACCENT);
	bui_icon_in(c, BUI_IC(search, 24), bui_r(r.x + 10, r.y, 32, r.h), BUI_C_TEXT2);
	if (!shown[0]) {
		bui_text_box(c, BUI_FONT_BODY, bui_r(r.x + 48, r.y, r.w - 60, r.h),
			     dicts[V.cur].kana ? "Scrivi in romaji (inu, taberu\xE2\x80\xA6)" : "Scrivi una parola",
			     BUI_C_TEXT3, BUI_AL_LEFT);
		tw = 0;
	} else {
		bui_text_box(c, BUI_FONT_BODY, bui_r(r.x + 48, r.y, r.w - 60, r.h), shown, BUI_C_TEXT, BUI_AL_LEFT);
		tw = bui_text_width(BUI_FONT_BODY, shown, (int)strlen(shown));
	}
	if (r.x + 48 + tw + 2 < r.x + r.w - 12)
		bui_fill(c, bui_r(r.x + 48 + tw + 1, r.y + 10, 2, r.h - 20), BUI_C_ACCENT);
}

static int header(bui_panel *p, const char *title)
{
	bui_widget *w = bui_add_button(p, bui_r(0, 0, 64, BUI_HEADER_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND,
				       on_back);

	w->flags |= BUI_F_NOFOCUS;
	w = bui_add_label(p, bui_r(68, 0, V.area.w - 68 - 16, BUI_HEADER_H), title, BUI_LS_TITLE, BUI_AL_LEFT);
	return BUI_HEADER_H;
}

static void build(void)
{
	bui_panel *p = &V.p;
	bui_rect a = V.area;
	int y, i, w = a.w - 32;
	bui_widget *wd;

	bui_panel_clear(p);
	if (V.page == PG_ENTRY) {
		int h;

		y = header(p, V.title) + 4;
		h = bui_text_wrap(NULL, BUI_FONT_BODY, bui_r(0, 0, w - 8, 100000), V.body ? V.body : "", 0, 28,
				  BUI_AL_LEFT);
		bui_add_label(p, bui_r(24, y, w - 8, h), V.body ? V.body : "", BUI_LS_WRAP, BUI_AL_LEFT);
		y += h + 16;
		bui_panel_set_viewport(p, a, y > a.h ? y : a.h);
		return;
	}
	/* intestazione: titolo a sinistra, scelta del dizionario a destra */
	header(p, "Dizionario");
	V.opts[0] = 0;
	for (i = 0; i < NDICTS; i++) {
		strncat(V.opts, dicts[i].label, sizeof(V.opts) - strlen(V.opts) - 2);
		if (i < NDICTS - 1)
			strcat(V.opts, "\n");
	}
	bui_add_segs(p, bui_r(a.w - 16 - 440, 8, 440, 40), V.opts, V.cur, on_dict);
	y = BUI_HEADER_H + 4;
	bui_add_custom(p, bui_r(16, y, w, 48), draw_field);
	y += 56;
	if (!V.ok) {
		bui_add_label(p, bui_r(24, y + 20, w - 16, 60),
			      "Dizionario non installato: mancano i file in /usr/local/share/brain/dict",
			      BUI_LS_WRAP, BUI_AL_LEFT);
		y += 80;
	} else if (!V.key[0]) {
		bui_add_label(p, bui_r(24, y + 20, w - 16, 90),
			      "Scrivi con la tastiera: le voci compaiono mentre digiti. Invio apre la prima, Esc torna indietro.",
			      BUI_LS_WRAP, BUI_AL_LEFT);
		y += 110;
	} else if (!V.nres) {
		bui_add_label(p, bui_r(24, y + 20, w - 16, 30), "Nessuna voce trovata", BUI_LS_BODY, BUI_AL_LEFT);
		y += 60;
	}
	for (i = 0; V.ok && i < V.nres; i++) {
		wd = bui_add_row(p, bui_r(16, y, w, 56), NULL, V.head[i], V.prev[i], on_result);
		wd->id = i;
		wd->flags |= BUI_F_DIVIDER;
		y += 56;
	}
	bui_panel_set_viewport(p, a, y + 8 > a.h ? y + 8 : a.h);
}

/* ------------------------------------------------------------------ */
/* vista                                                              */
/* ------------------------------------------------------------------ */

static void dc_enter(bs_view *v)
{
	bui_panel_init(&V.p, v->win);
	V.page = PG_SEARCH;
	if (!V.dict.p)
		open_dict(bs_lang == BS_LANG_JA ? 3 : bs_lang == BS_LANG_EN ? 1 : 0);
	update();
}

static void dc_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&V.p);
	free(V.body);
	V.body = NULL;
	/* i file restano mappati: riaprire e' istantaneo, la memoria e' cache */
}

static void dc_layout(bs_view *v, bui_rect a)
{
	(void)v;
	V.area = a;
	build();
}

static void dc_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&V.p, c, clip);
}

static int dc_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&V.p, ev);
}

static int dc_key(bs_view *v, const bui_key *k)
{
	size_t n = strlen(V.q);
	int ch = k->key;

	(void)v;
	if (V.page == PG_ENTRY) {
		if (ch == BUI_K_ESC || ch == BUI_K_BACKSPACE) {
			go(PG_SEARCH);
			return 1;
		}
		return bui_panel_key(&V.p, k);
	}
	if (ch == BUI_K_ENTER) {
		if (V.nres)
			open_entry(0);
		return 1;
	}
	if (ch == BUI_K_BACKSPACE) {
		if (!n)
			return 0;	/* campo vuoto: torna alla home */
		/* toglie un carattere intero (anche accentato) */
		do
			n--;
		while (n && ((unsigned char)V.q[n] & 0xC0) == 0x80);
		V.q[n] = 0;
		update();
		build();
		bui_panel_damage(&V.p);
		return 1;
	}
	if (ch == BUI_K_ESC) {
		if (!n)
			return 0;
		V.q[0] = 0;
		update();
		build();
		bui_panel_damage(&V.p);
		return 1;
	}
	if (ch >= 0x20 && !(k->mods & (BUI_MOD_CTRL | BUI_MOD_ALT)) && ch < 0x110000 &&
	    (ch >= 0x80 || ch == ' ' || ch == '\'' || ch == '-' || (ch >= '0' && ch <= '9') ||
	     ((ch | 32) >= 'a' && (ch | 32) <= 'z'))) {
		char u[5];
		int l;

		if (ch < 0x80) {
			u[0] = (char)(ch >= 'A' && ch <= 'Z' ? ch | 32 : ch);
			l = 1;
		} else if (ch < 0x800) {
			u[0] = (char)(0xC0 | (ch >> 6));
			u[1] = (char)(0x80 | (ch & 0x3F));
			l = 2;
		} else {
			u[0] = (char)(0xE0 | (ch >> 12));
			u[1] = (char)(0x80 | ((ch >> 6) & 0x3F));
			u[2] = (char)(0x80 | (ch & 0x3F));
			l = 3;
		}
		if (n + (size_t)l < sizeof(V.q) - 1) {
			memcpy(V.q + n, u, (size_t)l);
			V.q[n + (size_t)l] = 0;
			update();
			build();
			bui_panel_damage(&V.p);
		}
		return 1;
	}
	return bui_panel_key(&V.p, k);
}

static int dc_text_input(bs_view *v)
{
	(void)v;
	return V.page == PG_SEARCH;
}

bs_view bs_view_dict = {
	.title = "Dizionario",
	.flags = BS_VIEW_NO_HEADER,
	.enter = dc_enter,
	.leave = dc_leave,
	.layout = dc_layout,
	.draw = dc_draw,
	.pointer = dc_pointer,
	.key = dc_key,
	.text_input = dc_text_input,
};
