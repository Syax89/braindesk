// SPDX-License-Identifier: MIT
/*
 * test/edit_fuzz.c - prova a caso l'editor delle note (view_notes_edit.c)
 * senza server X: migliaia di tasti, tocchi e cambi di larghezza, e dopo
 * ognuno controlla che
 *   - il testo sia quello di un modello ingenuo (inserisci/cancella);
 *   - le righe ricalcolate "per differenza" siano identiche a quelle di un
 *     ricalcolo completo;
 *   - cursore e scorrimento siano validi, e il disegno non esca dalla tela
 *     (sotto AddressSanitizer).
 * Si lancia con test/fuzz.sh [iterazioni [seme]].
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../view_notes_edit.c"

static char *model;
static int mlen;
static unsigned long long rng = 88172645463325252ULL;

static unsigned rnd(unsigned n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned)(rng >> 11) % n;
}

static void die(const char *what, int step)
{
	fprintf(stderr, "FALLITO al passo %d: %s (len %d, cur %d, righe %d)\n", step, what, E.len, E.cur, E.nl);
	exit(1);
}

static void model_edit(int pos, int del, const char *ins, int n)
{
	memmove(model + pos + n, model + pos + del, (size_t)(mlen - pos - del));
	memcpy(model + pos, ins, (size_t)n);
	mlen += n - del;
}

static int enc(unsigned cp, char *u)
{
	if (cp < 0x80) {
		u[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		u[0] = (char)(0xC0 | (cp >> 6));
		u[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	u[0] = (char)(0xE0 | (cp >> 12));
	u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
	u[2] = (char)(0x80 | (cp & 0x3F));
	return 3;
}

static void check(int step)
{
	int *inc, n, i;

	if (E.len != mlen || memcmp(E.b, model, (size_t)mlen))
		die("testo diverso dal modello", step);
	if (E.cur < 0 || E.cur > E.len || (E.cur < E.len && ((unsigned char)E.b[E.cur] & 0xC0) == 0x80))
		die("cursore fuori posto", step);
	if (E.sc.pos < 0 || E.sc.pos > E.sc.max)
		die("scorrimento fuori dai limiti", step);
	n = E.nl;
	inc = malloc(sizeof(int) * (size_t)n);
	memcpy(inc, E.ls, sizeof(int) * (size_t)n);
	relayout_all();
	if (n != E.nl || memcmp(inc, E.ls, sizeof(int) * (size_t)n))
		die("righe incrementali diverse dal ricalcolo completo", step);
	free(inc);
	for (i = 1; i < E.nl; i++)
		if (E.ls[i] <= E.ls[i - 1] && !(E.ls[i] == E.len && E.ls[i - 1] < E.len))
			if (E.ls[i] < E.ls[i - 1] || E.b[E.ls[i] - 1] != '\n')
				die("righe non crescenti", step);
	/* la riga del cursore deve essere in vista */
	i = line_y(line_of(E.cur));
	if (E.area.h >= LH + 2 * PAD_Y && (i < E.area.y || i + LH > E.area.y + E.area.h))
		die("cursore non in vista", step);
}

int main(int argc, char **argv)
{
	static const unsigned chars[] = { 'a', 'e', 'i', 'o', 'n', 'r', 't', 'l', 's', 'm', 'W', 'i', '.', ',',
					  0xE8, 0xE0, 0xF9, 0x20AC, ' ', ' ', ' ', ' ' };
	static const int moves[] = { BUI_K_LEFT, BUI_K_RIGHT, BUI_K_UP, BUI_K_DOWN, BUI_K_HOME, BUI_K_END,
				     BUI_K_PGUP, BUI_K_PGDN };
	int steps = argc > 1 ? atoi(argv[1]) : 200000, step, i;
	bui_canvas cv = { .w = 854, .h = 480, .stride = 854 };
	const char *seed_text = "Prima riga\n\nseconda\tcon tab\r\ne una riga lunga lunga lunga lunga lunga "
				"lunga lunga lunga lunga lunga lunga lunga lunga lunga lunga lunga lunga\n";

	if (argc > 2)
		rng ^= strtoull(argv[2], NULL, 0) * 0x9E3779B97F4A7C15ULL;
	cv.px = malloc(sizeof(bui_px) * 854 * 480);
	model = malloc(NE_MAX_BYTES + 64);
	if (ne_open(NULL, seed_text, (int)strlen(seed_text), NULL) < 0)
		return 1;
	ne_layout(bui_r(0, 96, 854, 384));
	memcpy(model, E.b, (size_t)E.len);
	mlen = E.len;
	check(-1);
	for (step = 0; step < steps; step++) {
		unsigned op = rnd(100);
		bui_key k = { 0, 0 };
		bui_pointer p = { 0 };
		char u[4];
		int n, c0 = E.cur;

		if (op < 45) {
			/* carattere (a volte una parola lunghissima senza spazi) */
			unsigned cp = chars[rnd(op < 6 ? 14 : (unsigned)(sizeof(chars) / sizeof(chars[0])))];

			k.key = (int)cp;
			n = enc(cp, u);
			model_edit(c0, 0, u, n);
			ne_key(&k);
		} else if (op < 52) {
			k.key = BUI_K_ENTER;
			model_edit(c0, 0, "\n", 1);
			ne_key(&k);
		} else if (op < 54) {
			k.key = BUI_K_TAB;
			model_edit(c0, 0, "    ", 4);
			ne_key(&k);
		} else if (op < 68) {
			k.key = BUI_K_BACKSPACE;
			if (c0 > 0)
				model_edit(u_prev(c0), c0 - u_prev(c0), "", 0);
			ne_key(&k);
		} else if (op < 74) {
			k.key = BUI_K_DELETE;
			if (c0 < E.len)
				model_edit(c0, u_next(c0) - c0, "", 0);
			ne_key(&k);
		} else if (op < 92) {
			k.key = moves[rnd(8)];
			k.mods = rnd(10) == 0 ? BUI_MOD_CTRL : 0;
			/* una raffica: su/giu' devono conservare la colonna senza uscire dal testo */
			for (i = (int)rnd(4); i >= 0; i--)
				ne_key(&k);
		} else if (op < 96) {
			/* tocco: mette il cursore; trascinamento: scorre */
			p.type = BUI_PRESS;
			p.x = E.area.x + (int)rnd((unsigned)E.area.w);
			p.y = E.area.y + (int)rnd((unsigned)E.area.h);
			p.time = (uint32_t)step * 20;
			ne_pointer(&p);
			if (rnd(3) == 0) {
				p.type = BUI_MOVE;
				p.y += (int)rnd(300) - 150;
				p.time += 16;
				ne_pointer(&p);
				p.type = BUI_RELEASE;
				p.time += 200;
				ne_pointer(&p);
				/* dopo uno scorrimento a mano il cursore puo' essere fuori vista: lo si riporta */
				k.key = BUI_K_LEFT;
				ne_key(&k);
			} else {
				p.type = BUI_RELEASE;
				p.time += 60;
				ne_pointer(&p);
			}
		} else if (op < 98) {
			/* cambio di orientamento o di dimensione */
			int w = rnd(4) == 0 ? 60 + (int)rnd(200) : rnd(2) ? 854 : 480;
			int h = w == 854 ? 384 : 60 + (int)rnd(700);

			ne_layout(bui_r(0, 96, w, h > 384 ? 384 : h));
		} else {
			/* ridisegno di un rettangolo qualsiasi */
			bui_rect clip = bui_r((int)rnd(800), 96 + (int)rnd(380), 1 + (int)rnd(854), 1 + (int)rnd(384));

			bui_rect_isect(clip, bui_r(0, 0, 854, 480), &clip);
			cv.clip = clip;
			ne_draw(&cv, clip);
		}
		if (E.len > 6000) {
			/* tiene il testo piccolo: i ricalcoli completi del controllo costano */
			k.key = BUI_K_HOME;
			k.mods = BUI_MOD_CTRL;
			ne_key(&k);
			k.key = BUI_K_DELETE;
			k.mods = 0;
			for (i = 0; i < 3000; i++) {
				model_edit(0, u_next(0), "", 0);
				ne_key(&k);
			}
		}
		check(step);
	}
	printf("edit_fuzz: %d passi senza errori (testo finale %d byte, %d righe)\n", steps, E.len, E.nl);
	ne_close();
	free(model);
	free(cv.px);
	return 0;
}
