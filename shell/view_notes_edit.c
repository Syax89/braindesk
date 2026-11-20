// SPDX-License-Identifier: MIT
/*
 * view_notes_edit.c - l'editor di testo delle note.
 *
 * Il testo e' un unico buffer UTF-8; "ls" contiene l'inizio di ogni riga
 * visibile (a capo automatico fra le parole). Una modifica ricalcola solo
 * il capoverso toccato e chiede il ridisegno delle sole righe cambiate, da
 * dove inizia la modifica in poi: mentre si scrive parte un rettangolo alto
 * una riga. Il cursore non lampeggia (a riposo la shell non si sveglia mai).
 *
 * Le larghezze vengono da bui_text_width(), lo stesso codice che disegna:
 * il cursore cade sempre esattamente fra due glifi.
 */
#include <stdlib.h>
#include <string.h>

#include "media.h"

#define FONT	BUI_FONT_BODY
#define LH	26		/* altezza di riga */
#define PAD_X	16
#define PAD_Y	8
#define BAR_W	8		/* striscia della barra di scorrimento */

static struct {
	bui_win *win;
	bui_rect area;
	char *b;
	int len, cap;
	int *ls, nl, lcap;	/* inizio delle righe visibili */
	int *tmp, tcap;		/* righe del capoverso in corso di ricalcolo */
	int *old, ocap;
	int cur;		/* posizione del cursore (byte) */
	int goal_x;		/* colonna in pixel per su/giu' (-1: da ricalcolare) */
	bui_scroll sc;
	int wrap_w, space_w;
	int press_x, press_y;
	uint8_t open, captured, dragging, stopped, warned;
	void (*changed)(void);
} E;

/* ------------------------------------------------------------------ */
/* buffer e righe                                                     */
/* ------------------------------------------------------------------ */

static int u_next(int i)
{
	if (i < E.len)
		i++;
	while (i < E.len && ((unsigned char)E.b[i] & 0xC0) == 0x80)
		i++;
	return i;
}

static int u_prev(int i)
{
	if (i > 0)
		i--;
	while (i > 0 && ((unsigned char)E.b[i] & 0xC0) == 0x80)
		i--;
	return i;
}

static int push(int **arr, int *n, int *cap, int v)
{
	if (*n == *cap) {
		int nc = *cap ? *cap * 2 : 64;
		int *na = realloc(*arr, sizeof(int) * (size_t)nc);

		if (!na)
			return -1;
		*arr = na;
		*cap = nc;
	}
	(*arr)[(*n)++] = v;
	return 0;
}

/*
 * Divide in righe il capoverso che inizia in p e ne accoda gli inizi.
 * Ritorna la posizione dopo il suo a-capo, oppure -1 se il capoverso arriva
 * alla fine del testo. Gli spazi restano in coda alla riga che chiudono.
 */
static int wrap_para(int p, int **arr, int *n, int *cap)
{
	int i = p, start = p, x = 0;

	push(arr, n, cap, p);
	for (;;) {
		int j, k, ww, fit;

		if (i >= E.len)
			return -1;
		if (E.b[i] == '\n')
			return i + 1;
		for (j = i; j < E.len && E.b[j] != ' ' && E.b[j] != '\n'; j++)
			;
		for (k = j; k < E.len && E.b[k] == ' '; k++)
			;
		ww = j > i ? bui_text_width(FONT, E.b + i, j - i) : 0;
		if (x + ww > E.wrap_w && i > start) {
			/* la parola va a capo */
			push(arr, n, cap, i);
			start = i;
			x = 0;
			continue;
		}
		if (ww > E.wrap_w) {
			/* parola piu' larga della riga: si spezza fra due caratteri */
			int q = i, w = 0;

			while (q < j) {
				int nq = u_next(q);
				int cw = bui_text_width(FONT, E.b + q, nq - q);

				if (w + cw > E.wrap_w && q > start)
					break;
				w += cw;
				q = nq;
			}
			if (q < j) {
				push(arr, n, cap, q);
				start = i = q;
				x = 0;
				continue;
			}
			ww = w;
		}
		x += ww;
		fit = (E.wrap_w - x) / E.space_w;
		if (fit < 1)
			fit = 1;
		if (k - j > fit) {
			/* una fila di spazi piu' lunga della riga */
			i = j + fit;
			push(arr, n, cap, i);
			start = i;
			x = 0;
			continue;
		}
		x += (k - j) * E.space_w;
		i = k;
	}
}

static void update_max(void)
{
	int m = 2 * PAD_Y + E.nl * LH - E.area.h;

	E.sc.max = m > 0 ? m : 0;
	if (E.sc.pos > E.sc.max)
		E.sc.pos = E.sc.max;
}

static void relayout_all(void)
{
	int p = 0, r;

	E.nl = 0;
	while ((r = wrap_para(p, &E.ls, &E.nl, &E.lcap)) >= 0)
		p = r;
	update_max();
}

/* riga visibile che contiene la posizione (l'ultima che inizia prima o li') */
static int line_of(int off)
{
	int lo = 0, hi = E.nl - 1;

	while (lo < hi) {
		int mid = (lo + hi + 1) >> 1;

		if (E.ls[mid] <= off)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

/* fine del testo della riga (a-capo escluso) */
static int line_end(int i)
{
	int e = i + 1 < E.nl ? E.ls[i + 1] : E.len;

	if (i + 1 < E.nl && e > E.ls[i] && E.b[e - 1] == '\n')
		e--;
	return e;
}

/* ultima posizione del cursore che sta ancora su questa riga */
static int line_limit(int i)
{
	int e = line_end(i);

	/* riga spezzata dall'a capo automatico: la posizione finale appartiene gia' alla successiva */
	if (i + 1 < E.nl && e == E.ls[i + 1] && e > E.ls[i])
		e = u_prev(e);
	return e;
}

static int line_y(int i)
{
	return E.area.y + PAD_Y + i * LH - E.sc.pos;
}

static int x_in_line(int line, int off)
{
	int x = E.area.x + PAD_X + bui_text_width(FONT, E.b + E.ls[line], off - E.ls[line]);
	int xmax = E.area.x + E.area.w - BAR_W - 2;

	return x > xmax ? xmax : x;
}

static int off_at(int line, int x)
{
	int lo = E.ls[line], lim = line_limit(line), p = lo, pw = 0;
	int target = x - (E.area.x + PAD_X);

	while (p < lim) {
		int q = u_next(p);
		int w = bui_text_width(FONT, E.b + lo, q - lo);

		if (w > target)
			return target - pw < w - target ? p : q;
		pw = w;
		p = q;
	}
	return lim;
}

/* ------------------------------------------------------------------ */
/* ridisegni                                                          */
/* ------------------------------------------------------------------ */

static void damage(bui_rect r)
{
	if (bui_rect_isect(r, E.area, &r))
		bui_win_damage(E.win, r);
}

static bui_rect cursor_rect(void)
{
	int line = line_of(E.cur);

	return bui_r(x_in_line(line, E.cur), line_y(line) + 2, 2, LH - 4);
}

/* porta in vista la riga del cursore; ritorna 1 se il testo e' scorso */
static int reveal_cursor(void)
{
	int top = PAD_Y + line_of(E.cur) * LH, np = E.sc.pos;

	if (top - PAD_Y < np)
		np = top - PAD_Y;
	else if (top + LH + PAD_Y > np + E.area.h)
		np = top + LH + PAD_Y - E.area.h;
	if (np > E.sc.max)
		np = E.sc.max;
	if (np < 0)
		np = 0;
	E.sc.fling = 0;
	if (np == E.sc.pos)
		return 0;
	E.sc.pos = np;
	damage(E.area);
	return 1;
}

static void move_to(int off, int keep_goal)
{
	damage(cursor_rect());
	E.cur = off;
	if (!keep_goal)
		E.goal_x = -1;
	if (!reveal_cursor())
		damage(cursor_rect());
}

/* ------------------------------------------------------------------ */
/* modifiche                                                          */
/* ------------------------------------------------------------------ */

/* dove finisce, dopo la modifica, cio' che prima stava in s */
static int remap(int s, int pos, int del, int ins)
{
	if (s <= pos)
		return s;
	return s >= pos + del ? s + ins - del : -1;
}

/*
 * Il buffer e' gia' cambiato: in pos sono spariti del byte e ne sono
 * comparsi ins. Ricalcola le righe dei capoversi toccati e chiede il
 * ridisegno del minimo.
 */
static void relayout_edit(int pos, int del, int ins)
{
	int delta = ins - del, a, b, ne, i, k, m, oldn, p, r, old_max = E.sc.max;
	int right = E.area.x + E.area.w;

	a = line_of(pos);
	while (a > 0 && E.b[E.ls[a] - 1] != '\n')
		a--;
	for (i = pos + ins; i < E.len && E.b[i] != '\n'; i++)
		;
	ne = i < E.len ? i + 1 : -1;	/* fine (nuova) dell'ultimo capoverso toccato */
	if (ne < 0)
		b = E.nl - 1;
	else
		for (b = a; b + 1 < E.nl && E.ls[b + 1] < ne - delta; b++)
			;
	oldn = b - a + 1;
	m = 0;
	p = E.ls[a];
	for (;;) {
		r = wrap_para(p, &E.tmp, &m, &E.tcap);
		if (r < 0 || (ne >= 0 && r >= ne))
			break;
		p = r;
	}
	/* le righe vecchie, portate alle posizioni nuove, per il confronto */
	for (k = 0, i = 0; k < oldn; k++)
		push(&E.old, &i, &E.ocap, remap(E.ls[a + k], pos, del, ins));
	/* sostituisce [a, b] con le m righe nuove e sposta il resto */
	if (E.nl - oldn + m > E.lcap) {
		int nc = (E.nl - oldn + m) * 2;
		int *na = realloc(E.ls, sizeof(int) * (size_t)nc);

		if (!na)
			return;
		E.ls = na;
		E.lcap = nc;
	}
	memmove(E.ls + a + m, E.ls + b + 1, sizeof(int) * (size_t)(E.nl - b - 1));
	E.nl += m - oldn;
	for (k = a + m; k < E.nl; k++)
		E.ls[k] += delta;
	memcpy(E.ls + a, E.tmp, sizeof(int) * (size_t)m);
	update_max();

	if (m == oldn) {
		for (k = 0; k < m; k++) {
			int next = k + 1 < m ? E.tmp[k + 1] : ne >= 0 ? ne : E.len + 1;
			int start_same = E.tmp[k] == E.old[k];
			int end_same = k + 1 >= m || E.tmp[k + 1] == E.old[k + 1];
			int touched = E.tmp[k] <= pos + ins && pos < next;
			int x = E.area.x;

			if (start_same && end_same && !touched)
				continue;
			/* a sinistra della modifica i glifi non si sono mossi */
			if (start_same && touched && pos > E.tmp[k])
				x = x_in_line(a + k, pos) - 6;
			damage(bui_r(x, line_y(a + k), right - x, LH));
		}
	} else {
		/* il numero di righe e' cambiato: tutto cio' che sta sotto si sposta */
		int y;

		k = line_of(pos);
		if (k > a)
			k--;	/* la prima parola puo' essere risalita alla riga sopra */
		y = line_y(k);
		damage(bui_r(E.area.x, y, E.area.w, E.area.y + E.area.h - y));
	}
	if (E.sc.max != old_max)
		damage(bui_r(right - BAR_W, E.area.y, BAR_W, E.area.h));
}

static void edit(int pos, int del, const char *ins, int n)
{
	if (E.len - del + n > NE_MAX_BYTES) {
		if (!E.warned)
			bui_toast("La nota ha raggiunto la lunghezza massima", 2500);
		E.warned = 1;
		return;
	}
	if (E.len - del + n + 1 > E.cap) {
		int nc = E.cap * 2 > E.len + n + 256 ? E.cap * 2 : E.len + n + 256;
		char *nb = realloc(E.b, (size_t)nc);

		if (!nb)
			return;
		E.b = nb;
		E.cap = nc;
	}
	damage(cursor_rect());
	memmove(E.b + pos + n, E.b + pos + del, (size_t)(E.len - pos - del));
	if (n)
		memcpy(E.b + pos, ins, (size_t)n);
	E.len += n - del;
	relayout_edit(pos, del, n);
	E.cur = pos + n;
	E.goal_x = -1;
	if (!reveal_cursor())
		damage(cursor_rect());
	if (E.changed)
		E.changed();
}

/* ------------------------------------------------------------------ */
/* interfaccia verso la vista                                         */
/* ------------------------------------------------------------------ */

/* lunghezza della sequenza UTF-8 valida che inizia in s (0 se non valida) */
static int utf8_valid(const unsigned char *s, int left)
{
	int n, i;

	if (s[0] < 0x80)
		return 1;
	if (s[0] >= 0xC2 && s[0] <= 0xDF)
		n = 2;
	else if (s[0] >= 0xE0 && s[0] <= 0xEF)
		n = 3;
	else if (s[0] >= 0xF0 && s[0] <= 0xF4)
		n = 4;
	else
		return 0;
	if (n > left)
		return 0;
	for (i = 1; i < n; i++)
		if ((s[i] & 0xC0) != 0x80)
			return 0;
	return n;
}

int ne_open(bui_win *win, const char *text, int len, void (*changed)(void))
{
	const unsigned char *s = (const unsigned char *)text;
	int i, n;

	ne_close();
	memset(&E, 0, sizeof(E));
	E.cap = len * 4 + 256;
	E.b = malloc((size_t)E.cap);
	if (!E.b)
		return -1;
	/* niente CR, tab come quattro spazi, byte non UTF-8 letti come Latin-1 */
	for (i = 0; i < len;) {
		if (s[i] == '\t') {
			memcpy(E.b + E.len, "    ", 4);
			E.len += 4;
			i++;
		} else if (s[i] == '\n' || (s[i] >= 0x20 && s[i] < 0x7F)) {
			E.b[E.len++] = (char)s[i++];
		} else if (s[i] < 0x80) {
			i++;
		} else if ((n = utf8_valid(s + i, len - i)) > 0) {
			memcpy(E.b + E.len, s + i, (size_t)n);
			E.len += n;
			i += n;
		} else {
			E.b[E.len++] = (char)(0xC0 | (s[i] >> 6));
			E.b[E.len++] = (char)(0x80 | (s[i] & 0x3F));
			i++;
		}
	}
	E.win = win;
	E.changed = changed;
	E.cur = E.len;
	E.goal_x = -1;
	E.space_w = bui_text_width(FONT, " ", 1);
	if (E.space_w < 1)
		E.space_w = 1;
	E.wrap_w = 100;
	E.open = 1;
	relayout_all();
	return 0;
}

static int ne_anim(void *user, uint32_t now)
{
	(void)user;
	if (!E.open)
		return 0;
	if (bui_scroll_step(&E.sc, now))
		damage(E.area);
	return E.sc.fling;
}

void ne_close(void)
{
	bui_anim_del(ne_anim, NULL);
	free(E.b);
	free(E.ls);
	free(E.tmp);
	free(E.old);
	memset(&E, 0, sizeof(E));
}

const char *ne_text(int *len)
{
	*len = E.len;
	return E.b;
}

void ne_layout(bui_rect area)
{
	if (!E.open)
		return;
	E.area = area;
	E.wrap_w = area.w - 2 * PAD_X - BAR_W;
	if (E.wrap_w < 40)
		E.wrap_w = 40;
	relayout_all();
	reveal_cursor();
}

void ne_draw(bui_canvas *c, bui_rect clip)
{
	bui_rect saved, k, cr;
	int i, base = (LH - (FONT->ascent + FONT->descent)) / 2 + FONT->ascent;
	char tmp[640];

	if (!E.open || !bui_rect_isect(clip, E.area, &k))
		return;
	saved = bui_clip_push(c, k);
	i = (k.y - (E.area.y + PAD_Y) + E.sc.pos) / LH;
	for (i = i < 0 ? 0 : i; i < E.nl; i++) {
		int y = line_y(i), n = line_end(i) - E.ls[i];

		if (y >= k.y + k.h)
			break;
		if (n <= 0)
			continue;
		if (n > (int)sizeof(tmp) - 1)
			n = (int)sizeof(tmp) - 1;
		memcpy(tmp, E.b + E.ls[i], (size_t)n);
		tmp[n] = 0;
		bui_text(c, FONT, E.area.x + PAD_X, y + base, tmp, BUI_C_TEXT);
	}
	if (!E.len)
		bui_text(c, FONT, E.area.x + PAD_X + 6, line_y(0) + base, "Scrivi qui\xE2\x80\xA6", BUI_C_TEXT3);
	cr = cursor_rect();
	bui_fill(c, cr, BUI_C_ACCENT);
	if (E.sc.max > 0) {
		int content = E.area.h + E.sc.max;
		int th = E.area.h * E.area.h / content;

		if (th < 24)
			th = 24;
		bui_rrect_a(c, bui_r(E.area.x + E.area.w - 6,
				     E.area.y + (E.area.h - th) * E.sc.pos / E.sc.max + 2, 4, th - 4),
			    2, BUI_C_TEXT2, 110);
	}
	bui_clip_pop(c, saved);
}

int ne_pointer(const bui_pointer *ev)
{
	int dy, line;

	if (!E.open)
		return 0;
	switch (ev->type) {
	case BUI_PRESS:
		if (!bui_rect_has(E.area, ev->x, ev->y))
			return 0;
		E.captured = 1;
		E.dragging = 0;
		E.stopped = E.sc.fling;	/* un tocco sul testo in corsa lo ferma soltanto */
		E.press_x = ev->x;
		E.press_y = ev->y;
		bui_scroll_grab(&E.sc, ev->y, ev->time);
		return 1;
	case BUI_MOVE:
		if (!E.captured)
			return 0;
		dy = ev->y - E.press_y;
		if (!E.dragging && (dy > BUI_DRAG_SLOP || dy < -BUI_DRAG_SLOP) && E.sc.max > 0) {
			E.dragging = 1;
			bui_scroll_grab(&E.sc, ev->y, ev->time);
		}
		if (E.dragging && bui_scroll_drag(&E.sc, ev->y, ev->time))
			damage(E.area);
		return 1;
	case BUI_RELEASE:
	case BUI_CANCEL:
		if (!E.captured)
			return 0;
		E.captured = 0;
		if (E.dragging) {
			if (ev->type == BUI_RELEASE && bui_scroll_release(&E.sc, ev->time))
				bui_anim_add(ne_anim, NULL);
		} else if (ev->type == BUI_RELEASE && !E.stopped) {
			/* tocco: il cursore va li' */
			line = (ev->y - E.area.y - PAD_Y + E.sc.pos) / LH;
			if (ev->y - E.area.y - PAD_Y + E.sc.pos < 0)
				line = 0;
			if (line > E.nl - 1)
				line = E.nl - 1;
			move_to(off_at(line, ev->x), 0);
		}
		return 1;
	case BUI_WHEEL:
		if (!bui_rect_has(E.area, ev->x, ev->y))
			return 0;
		if (bui_scroll_by(&E.sc, ev->dy * 3 * LH))
			damage(E.area);
		return 1;
	}
	return 0;
}

static void move_lines(int n)
{
	int line = line_of(E.cur), to = line + n;

	if (E.goal_x < 0)
		E.goal_x = x_in_line(line, E.cur);
	if (to < 0)
		to = 0;
	if (to > E.nl - 1)
		to = E.nl - 1;
	if (to == line) {
		/* gia' sulla prima o sull'ultima riga: va all'inizio o alla fine */
		move_to(n < 0 ? E.ls[line] : line_limit(line), 0);
		return;
	}
	move_to(off_at(to, E.goal_x), 1);
}

int ne_key(const bui_key *k)
{
	int line, page;
	char u[4];

	if (!E.open)
		return 0;
	line = line_of(E.cur);
	page = E.area.h / LH - 1;
	switch (k->key) {
	case BUI_K_LEFT:
		move_to(u_prev(E.cur), 0);
		return 1;
	case BUI_K_RIGHT:
		move_to(u_next(E.cur), 0);
		return 1;
	case BUI_K_UP:
		move_lines(-1);
		return 1;
	case BUI_K_DOWN:
		move_lines(1);
		return 1;
	case BUI_K_PGUP:
		move_lines(-(page > 1 ? page : 1));
		return 1;
	case BUI_K_PGDN:
		move_lines(page > 1 ? page : 1);
		return 1;
	case BUI_K_HOME:
		move_to((k->mods & BUI_MOD_CTRL) ? 0 : E.ls[line], 0);
		return 1;
	case BUI_K_END:
		move_to((k->mods & BUI_MOD_CTRL) ? E.len : line_limit(line), 0);
		return 1;
	case BUI_K_ENTER:
		edit(E.cur, 0, "\n", 1);
		return 1;
	case BUI_K_TAB:
		edit(E.cur, 0, "    ", 4);
		return 1;
	case BUI_K_BACKSPACE:
		if (E.cur > 0) {
			int p = u_prev(E.cur);

			edit(p, E.cur - p, NULL, 0);
		}
		return 1;
	case BUI_K_DELETE:
		if (E.cur < E.len)
			edit(E.cur, u_next(E.cur) - E.cur, NULL, 0);
		return 1;
	default:
		break;
	}
	if (k->key < 0x20 || k->key == 0x7F || k->key >= 0x10000)
		return 0;
	if (k->mods & (BUI_MOD_CTRL | BUI_MOD_ALT))
		return 1;
	/* carattere stampabile: il toolkit lo consegna come codice Unicode */
	if (k->key < 0x80) {
		u[0] = (char)k->key;
		edit(E.cur, 0, u, 1);
	} else if (k->key < 0x800) {
		u[0] = (char)(0xC0 | (k->key >> 6));
		u[1] = (char)(0x80 | (k->key & 0x3F));
		edit(E.cur, 0, u, 2);
	} else {
		u[0] = (char)(0xE0 | (k->key >> 12));
		u[1] = (char)(0x80 | ((k->key >> 6) & 0x3F));
		u[2] = (char)(0x80 | (k->key & 0x3F));
		edit(E.cur, 0, u, 3);
	}
	return 1;
}
