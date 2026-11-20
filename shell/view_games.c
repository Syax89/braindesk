// SPDX-License-Identifier: MIT
/*
 * view_games.c - Giochi: Memory, Quindici, Tris, Serpente.
 *
 * Una vista con un menu e quattro giochi piccoli, disegnati con le
 * primitive del toolkit (nessuna immagine): tutto a tocco, e da tastiera
 * dove ha senso (frecce, Invio). Il serpente avanza con bs_view_tick_every
 * solo mentre si gioca: a gioco fermo nessun timer, nessun consumo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "shell.h"

enum { G_MENU, G_MEMORY, G_FIFTEEN, G_TRIS, G_SNAKE };

#define SN_W 26
#define SN_H 13
#define SN_MAX (SN_W * SN_H)

static struct {
	bui_panel p;
	bui_rect area, board;	/* area della vista, area di gioco (coordinate della finestra) */
	int game;
	char status[96];
	/* memory */
	int card[16], shown[16], done[16], first, second, moves, pairs;
	/* quindici */
	int tile[16], fmoves;
	/* tris */
	int cell[9], over, starter;
	/* serpente */
	int sx[SN_MAX], sy[SN_MAX], slen, dir, ndir, fx, fy, score, alive, best;
	int drag_x, drag_y, dragging;
} G;

static void build(void);

static int rnd(int n)
{
	return (int)(((unsigned)rand() >> 4) % (unsigned)n);
}

static void damage_board(void)
{
	bs_view_damage(G.board);
}

static void set_status(const char *fmt, int a, int b)
{
	snprintf(G.status, sizeof(G.status), T(fmt), a, b);
	build();
	bui_panel_damage(&G.p);
}

/* ------------------------------------------------------------------ */
/* Memory                                                             */
/* ------------------------------------------------------------------ */

static const bui_px mem_col[8] = {
	BUI_RGB(0xef, 0x47, 0x6f), BUI_RGB(0xff, 0xb7, 0x03), BUI_RGB(0x06, 0xd6, 0xa0), BUI_RGB(0x11, 0x8a, 0xb2),
	BUI_RGB(0x9b, 0x5d, 0xe5), BUI_RGB(0xf1, 0x5b, 0xb5), BUI_RGB(0x8a, 0xc9, 0x26), BUI_RGB(0xff, 0x8c, 0x42),
};
static const bui_icon *mem_icon(int k)
{
	switch (k) {
	case 0: return BUI_IC(music, 40);
	case 1: return BUI_IC(clock, 40);
	case 2: return BUI_IC(calc, 40);
	case 3: return BUI_IC(folder, 40);
	case 4: return BUI_IC(gallery, 40);
	case 5: return BUI_IC(note, 40);
	case 6: return BUI_IC(games, 40);
	default: return BUI_IC(dict, 40);
	}
}

static void memory_new(void)
{
	int i;

	for (i = 0; i < 16; i++)
		G.card[i] = i / 2;
	for (i = 15; i > 0; i--) {
		int j = rnd(i + 1), t = G.card[i];

		G.card[i] = G.card[j];
		G.card[j] = t;
	}
	memset(G.shown, 0, sizeof(G.shown));
	memset(G.done, 0, sizeof(G.done));
	G.first = G.second = -1;
	G.moves = G.pairs = 0;
	snprintf(G.status, sizeof(G.status), "%s", T("Trova le coppie"));
}

static bui_rect cell_rect(int i, int cols, int rows)
{
	int s = (G.board.h - 8) / rows, w = s * cols;

	if (w > G.board.w)
		s = G.board.w / cols, w = s * cols;
	return bui_r(G.board.x + (G.board.w - w) / 2 + (i % cols) * s + 4,
		     G.board.y + (G.board.h - s * rows) / 2 + (i / cols) * s + 4, s - 8, s - 8);
}

static void memory_draw(bui_canvas *c)
{
	int i;

	for (i = 0; i < 16; i++) {
		bui_rect r = cell_rect(i, 4, 4);

		if (G.shown[i] || G.done[i]) {
			bui_rrect(c, r, 12, G.done[i] ? bui_mix(BUI_C_SURFACE, mem_col[G.card[i]], 110)
						     : mem_col[G.card[i]]);
			bui_icon_in(c, mem_icon(G.card[i]), r, BUI_RGB(0x16, 0x12, 0x24));
		} else {
			bui_rrect(c, r, 12, BUI_C_SURFACE3);
			bui_rrect_line(c, bui_rect_inset(r, 6, 6), 8, 2, BUI_C_ACCENT_BG);
		}
	}
}

static void memory_hide(bs_view *v)
{
	(void)v;
	if (G.first >= 0 && G.second >= 0) {
		G.shown[G.first] = G.shown[G.second] = 0;
		G.first = G.second = -1;
		damage_board();
	}
}

static void memory_tap(int i)
{
	if (i < 0 || G.done[i] || G.shown[i])
		return;
	if (G.second >= 0)
		memory_hide(NULL);	/* la coppia sbagliata sparisce subito al tocco dopo */
	G.shown[i] = 1;
	if (G.first < 0) {
		G.first = i;
	} else {
		G.second = i;
		G.moves++;
		if (G.card[G.first] == G.card[G.second]) {
			G.done[G.first] = G.done[G.second] = 1;
			G.first = G.second = -1;
			if (++G.pairs == 8) {
				set_status("Completato in %d mosse!", G.moves, 0);
				return;
			}
		} else {
			bs_view_tick_in(800);
		}
		set_status("Mosse: %d", G.moves, 0);
	}
	damage_board();
}

/* ------------------------------------------------------------------ */
/* Quindici                                                           */
/* ------------------------------------------------------------------ */

static int fifteen_blank(void)
{
	int i;

	for (i = 0; i < 16; i++)
		if (!G.tile[i])
			return i;
	return 15;
}

static int fifteen_move(int i, int count)
{
	int b = fifteen_blank();

	if (i < 0 || i > 15 || i == b)
		return 0;
	if (!((i / 4 == b / 4 && abs(i - b) == 1) || (i % 4 == b % 4 && abs(i - b) == 4)))
		return 0;
	G.tile[b] = G.tile[i];
	G.tile[i] = 0;
	if (count)
		G.fmoves++;
	return 1;
}

static int fifteen_solved(void)
{
	int i;

	for (i = 0; i < 15; i++)
		if (G.tile[i] != i + 1)
			return 0;
	return 1;
}

static void fifteen_new(void)
{
	int i, n = 0;

	for (i = 0; i < 16; i++)
		G.tile[i] = (i + 1) % 16;
	/* mescola con mosse valide: sempre risolvibile */
	while (n < 300 || fifteen_solved()) {
		int b = fifteen_blank(), d[4] = { -1, 1, -4, 4 }, t = b + d[rnd(4)];

		if (t >= 0 && t < 16 && (t / 4 == b / 4 || t % 4 == b % 4))
			n += fifteen_move(t, 0);
	}
	G.fmoves = 0;
	snprintf(G.status, sizeof(G.status), "%s", T("Metti in ordine i numeri"));
}

static void fifteen_draw(bui_canvas *c)
{
	int i;
	char num[4];

	for (i = 0; i < 16; i++) {
		bui_rect r = cell_rect(i, 4, 4);

		if (!G.tile[i])
			continue;
		bui_rrect(c, r, 12, G.tile[i] == i + 1 ? BUI_C_ACCENT_BG : BUI_C_SURFACE3);
		snprintf(num, sizeof(num), "%d", G.tile[i]);
		bui_text_box(c, BUI_FONT_TITLE, r, num, BUI_C_TEXT, BUI_AL_CENTER);
	}
}

static void fifteen_tap(int i)
{
	if (!fifteen_move(i, 1))
		return;
	if (fifteen_solved())
		set_status("Risolto in %d mosse!", G.fmoves, 0);
	else
		set_status("Mosse: %d", G.fmoves, 0);
	damage_board();
}

/* ------------------------------------------------------------------ */
/* Tris                                                               */
/* ------------------------------------------------------------------ */

static const int lines3[8][3] = { { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 }, { 0, 3, 6 },
				  { 1, 4, 7 }, { 2, 5, 8 }, { 0, 4, 8 }, { 2, 4, 6 } };

static int tris_winner(const int *b)
{
	int i;

	for (i = 0; i < 8; i++)
		if (b[lines3[i][0]] && b[lines3[i][0]] == b[lines3[i][1]] && b[lines3[i][1]] == b[lines3[i][2]])
			return b[lines3[i][0]];
	for (i = 0; i < 9; i++)
		if (!b[i])
			return 0;
	return 3;	/* pareggio */
}

/* minimax: 2 = Brain, 1 = giocatore */
static int minimax(int *b, int turn, int *best)
{
	int w = tris_winner(b), i, score = turn == 2 ? -10 : 10;

	if (w == 2)
		return 1;
	if (w == 1)
		return -1;
	if (w == 3)
		return 0;
	for (i = 0; i < 9; i++) {
		int s, dummy;

		if (b[i])
			continue;
		b[i] = turn;
		s = minimax(b, 3 - turn, &dummy);
		b[i] = 0;
		if ((turn == 2 && s > score) || (turn == 1 && s < score)) {
			score = s;
			if (best)
				*best = i;
		}
	}
	return score;
}

static void tris_end(void)
{
	int w = tris_winner(G.cell);

	if (!w)
		return;
	G.over = 1;
	set_status(w == 1 ? "Hai vinto!" : w == 2 ? "Ha vinto il Brain" : "Pareggio", 0, 0);
}

static void tris_brain(void)
{
	int best = -1, i, free_cells[9], n = 0;

	for (i = 0; i < 9; i++)
		if (!G.cell[i])
			free_cells[n++] = i;
	if (!n)
		return;
	/* quasi sempre la mossa migliore: ogni tanto una a caso, cosi' si puo' vincere */
	if (rnd(100) < 22)
		best = free_cells[rnd(n)];
	else
		minimax(G.cell, 2, &best);
	if (best < 0)
		best = free_cells[0];
	G.cell[best] = 2;
	tris_end();
}

static void tris_new(void)
{
	memset(G.cell, 0, sizeof(G.cell));
	G.over = 0;
	G.starter ^= 1;		/* si alterna chi comincia */
	snprintf(G.status, sizeof(G.status), "%s", T("Tu giochi con X, il Brain con O"));
	if (!G.starter)
		tris_brain();
}

static void tris_draw(bui_canvas *c)
{
	int i;

	for (i = 0; i < 9; i++) {
		bui_rect r = cell_rect(i, 3, 3);

		bui_rrect(c, r, 14, BUI_C_SURFACE2);
		if (G.cell[i] == 1) {
			/* una X: due barre diagonali fatte di quadratini */
			int k, n = r.w - 40 - 10;

			for (k = 0; k <= n; k++) {
				bui_rrect(c, bui_r(r.x + 20 + k, r.y + 20 + k, 10, 10), 5, BUI_C_ACCENT);
				bui_rrect(c, bui_r(r.x + r.w - 30 - k, r.y + 20 + k, 10, 10), 5, BUI_C_ACCENT);
			}
		} else if (G.cell[i] == 2) {
			bui_rect o = bui_rect_inset(r, 18, 18);

			bui_rrect(c, o, o.w / 2, BUI_C_TEXT);
			bui_rrect(c, bui_rect_inset(o, 7, 7), o.w / 2 - 7, BUI_C_SURFACE2);
		}
	}
}

static void tris_tap(int i)
{
	if (G.over) {
		tris_new();
		build();
		bui_panel_damage(&G.p);
		damage_board();
		return;
	}
	if (i < 0 || G.cell[i])
		return;
	G.cell[i] = 1;
	tris_end();
	if (!G.over)
		tris_brain();
	damage_board();
}

/* ------------------------------------------------------------------ */
/* Serpente                                                           */
/* ------------------------------------------------------------------ */

static void snake_food(void)
{
	int tries = 0, i, ok;

	do {
		G.fx = rnd(SN_W);
		G.fy = rnd(SN_H);
		ok = 1;
		for (i = 0; i < G.slen; i++)
			if (G.sx[i] == G.fx && G.sy[i] == G.fy)
				ok = 0;
	} while (!ok && ++tries < 500);
}

static void snake_new(void)
{
	int i;

	G.slen = 4;
	for (i = 0; i < G.slen; i++) {
		G.sx[i] = SN_W / 2 - i;
		G.sy[i] = SN_H / 2;
	}
	G.dir = G.ndir = 0;	/* 0 destra, 1 giu', 2 sinistra, 3 su */
	G.score = 0;
	G.alive = 0;		/* parte al primo tocco o tasto */
	snake_food();
	snprintf(G.status, sizeof(G.status), "%s", T("Frecce o trascina per girare \xC2\xB7 tocca per iniziare"));
}

static bui_rect snake_cell(int x, int y)
{
	int s = G.board.w / SN_W < G.board.h / SN_H ? G.board.w / SN_W : G.board.h / SN_H;
	int ox = G.board.x + (G.board.w - s * SN_W) / 2, oy = G.board.y + (G.board.h - s * SN_H) / 2;

	return bui_r(ox + x * s, oy + y * s, s, s);
}

static void snake_draw(bui_canvas *c)
{
	bui_rect a = snake_cell(0, 0), f;
	int i;

	a.w *= SN_W;
	a.h *= SN_H;
	bui_rrect(c, bui_rect_inset(a, -4, -4), 10, BUI_C_SURFACE);
	f = snake_cell(G.fx, G.fy);
	bui_rrect(c, bui_rect_inset(f, 2, 2), f.w / 2, BUI_C_DANGER);
	for (i = G.slen - 1; i >= 0; i--) {
		bui_rect r = snake_cell(G.sx[i], G.sy[i]);

		bui_rrect(c, bui_rect_inset(r, 1, 1), 5, i ? bui_mix(BUI_C_SURFACE, BUI_C_ACCENT, 200) : BUI_C_ACCENT);
	}
}

static void snake_step(void)
{
	int dx[4] = { 1, 0, -1, 0 }, dy[4] = { 0, 1, 0, -1 };
	int nx, ny, i, grow;
	bui_rect old_tail = snake_cell(G.sx[G.slen - 1], G.sy[G.slen - 1]);

	if ((G.ndir + 2) % 4 != G.dir)
		G.dir = G.ndir;
	nx = G.sx[0] + dx[G.dir];
	ny = G.sy[0] + dy[G.dir];
	if (nx < 0 || ny < 0 || nx >= SN_W || ny >= SN_H)
		goto dead;
	for (i = 0; i < G.slen - 1; i++)
		if (G.sx[i] == nx && G.sy[i] == ny)
			goto dead;
	grow = nx == G.fx && ny == G.fy;
	if (grow && G.slen < SN_MAX)
		G.slen++;
	for (i = G.slen - 1; i > 0; i--) {
		G.sx[i] = G.sx[i - 1];
		G.sy[i] = G.sy[i - 1];
	}
	G.sx[0] = nx;
	G.sy[0] = ny;
	/* si ridisegnano solo testa, collo e coda (e il cibo se mangiato) */
	bs_view_damage(old_tail);
	bs_view_damage(snake_cell(G.sx[0], G.sy[0]));
	bs_view_damage(snake_cell(G.sx[1], G.sy[1]));
	if (grow) {
		G.score++;
		snake_food();
		bs_view_damage(snake_cell(G.fx, G.fy));
		set_status("Punti: %d", G.score, 0);
		/* piu' lungo, piu' veloce (fino a ~11 passi al secondo) */
		bs_view_tick_every(G.score < 30 ? 170 - G.score * 3 : 90);
	}
	return;
dead:
	G.alive = 0;
	bs_view_tick_every(0);
	if (G.score > G.best)
		G.best = G.score;
	set_status("Fine: %d punti (record %d) \xC2\xB7 tocca per ripartire", G.score, G.best);
}

static void snake_start(void)
{
	if (G.alive)
		return;
	snake_new();
	G.alive = 1;
	set_status("Punti: %d", 0, 0);
	bs_view_tick_every(170);
	damage_board();
}

/* ------------------------------------------------------------------ */
/* menu e vista                                                       */
/* ------------------------------------------------------------------ */

static const struct {
	const char *name, *desc;
	const bui_icon *icon;
} games[] = {
	{ "Memory", "Trova le coppie di carte", BUI_IC(style, 40) },
	{ "Quindici", "Riordina le tessere da 1 a 15", BUI_IC(extension, 40) },
	{ "Tris", "Tre in fila contro il Brain", BUI_IC(grid3, 40) },
	{ "Serpente", "Mangia e cresci, senza toccarti", BUI_IC(gesture, 40) },
};

static void start_game(int g)
{
	G.game = g;
	bs_view_tick_every(0);
	switch (g) {
	case G_MEMORY: memory_new(); break;
	case G_FIFTEEN: fifteen_new(); break;
	case G_TRIS: tris_new(); break;
	case G_SNAKE: snake_new(); break;
	}
	build();
	bui_panel_damage(&G.p);
}

static void on_game(bui_widget *w)
{
	start_game(w->id);
}

static void on_back(bui_widget *w)
{
	(void)w;
	if (G.game == G_MENU) {
		bs_view_close();
		return;
	}
	bs_view_tick_every(0);
	G.game = G_MENU;
	build();
	bui_panel_damage(&G.p);
}

static void on_restart(bui_widget *w)
{
	(void)w;
	start_game(G.game);
}

static void draw_board(bui_widget *w, bui_canvas *c, bui_rect r)
{
	(void)w;
	(void)r;
	switch (G.game) {
	case G_MEMORY: memory_draw(c); break;
	case G_FIFTEEN: fifteen_draw(c); break;
	case G_TRIS: tris_draw(c); break;
	case G_SNAKE: snake_draw(c); break;
	}
}

static void build(void)
{
	bui_panel *p = &G.p;
	bui_rect a = G.area;
	bui_widget *w;
	int i, y;

	bui_panel_clear(p);
	w = bui_add_button(p, bui_r(0, 0, 64, BUI_HEADER_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND, on_back);
	w->flags |= BUI_F_NOFOCUS;
	if (G.game == G_MENU) {
		bui_add_label(p, bui_r(68, 0, a.w - 84, BUI_HEADER_H), "Giochi", BUI_LS_TITLE, BUI_AL_LEFT);
		y = BUI_HEADER_H + 16;
		for (i = 0; i < 4; i++) {
			/* quattro riquadri come quelli della home, la descrizione sotto */
			int cw = (a.w - 32 - 3 * 16) / 4, x = 16 + i * (cw + 16);

			w = bui_add_tile(p, bui_r(x, y, cw, 176), games[i].name, games[i].icon, on_game);
			w->id = i + 1;
			bui_add_label(p, bui_r(x + 4, y + 186, cw - 8, 72), games[i].desc, BUI_LS_WRAP, BUI_AL_CENTER);
		}
		bui_panel_set_viewport(p, a, a.h);
		return;
	}
	bui_add_label(p, bui_r(68, 0, 220, BUI_HEADER_H), games[G.game - 1].name, BUI_LS_TITLE, BUI_AL_LEFT);
	bui_add_label(p, bui_r(290, 0, a.w - 290 - 180, BUI_HEADER_H), G.status, BUI_LS_SMALL, BUI_AL_RIGHT);
	bui_add_button(p, bui_r(a.w - 172, 6, 160, BUI_HEADER_H - 12), "Ricomincia", BUI_IC(replay, 24),
		       BUI_BS_TONAL, on_restart)->flags |= BUI_F_NOFOCUS;
	G.board = bui_r(a.x + 8, a.y + BUI_HEADER_H, a.w - 16, a.h - BUI_HEADER_H - 8);
	bui_add_custom(p, bui_r(8, BUI_HEADER_H, a.w - 16, a.h - BUI_HEADER_H - 8), draw_board);
	bui_panel_set_viewport(p, a, a.h);
}

static int hit(int cols, int rows, int x, int y)
{
	int i;

	for (i = 0; i < cols * rows; i++) {
		bui_rect r = cell_rect(i, cols, rows);

		if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h)
			return i;
	}
	return -1;
}

static void gm_enter(bs_view *v)
{
	bui_panel_init(&G.p, v->win);
	G.game = G_MENU;
	srand((unsigned)time(NULL) ^ (unsigned)bui_now());
}

static void gm_leave(bs_view *v)
{
	(void)v;
	bs_view_tick_every(0);
	G.alive = 0;
	bui_panel_clear(&G.p);
}

static void gm_layout(bs_view *v, bui_rect a)
{
	(void)v;
	G.area = a;
	build();
}

static void gm_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&G.p, c, clip);
}

static int gm_pointer(bs_view *v, const bui_pointer *ev)
{
	int in = ev->x >= G.board.x && ev->y >= G.board.y && ev->x < G.board.x + G.board.w &&
		 ev->y < G.board.y + G.board.h;

	(void)v;
	if (G.game == G_MENU || (!in && !G.dragging))
		return bui_panel_pointer(&G.p, ev);
	if (G.game == G_SNAKE) {
		/* trascinare gira il serpente; un tocco fermo lo fa partire */
		if (ev->type == BUI_PRESS) {
			G.drag_x = ev->x;
			G.drag_y = ev->y;
			G.dragging = 1;
		} else if (ev->type == BUI_MOVE && G.dragging && G.alive) {
			int dx = ev->x - G.drag_x, dy = ev->y - G.drag_y;

			if (abs(dx) > 24 || abs(dy) > 24) {
				G.ndir = abs(dx) > abs(dy) ? (dx > 0 ? 0 : 2) : (dy > 0 ? 1 : 3);
				G.drag_x = ev->x;
				G.drag_y = ev->y;
			}
		} else if (ev->type == BUI_RELEASE) {
			if (!G.alive)
				snake_start();
			G.dragging = 0;
		}
		return 1;
	}
	if (ev->type != BUI_PRESS)
		return 1;
	switch (G.game) {
	case G_MEMORY: memory_tap(hit(4, 4, ev->x, ev->y)); break;
	case G_FIFTEEN: fifteen_tap(hit(4, 4, ev->x, ev->y)); break;
	case G_TRIS: tris_tap(hit(3, 3, ev->x, ev->y)); break;
	}
	return 1;
}

static int gm_key(bs_view *v, const bui_key *k)
{
	int ch = k->key;

	(void)v;
	if (ch == BUI_K_ESC || ch == BUI_K_BACKSPACE) {
		if (G.game == G_MENU)
			return 0;
		on_back(NULL);
		return 1;
	}
	if (G.game == G_SNAKE) {
		int d = ch == BUI_K_RIGHT || ch == 'd' ? 0 : ch == BUI_K_DOWN || ch == 's' ? 1
			: ch == BUI_K_LEFT || ch == 'a' ? 2 : ch == BUI_K_UP || ch == 'w' ? 3 : -1;

		if (d >= 0) {
			if (!G.alive)
				snake_start();
			G.ndir = d;
			return 1;
		}
		if (ch == BUI_K_ENTER || ch == ' ') {
			snake_start();
			return 1;
		}
	}
	if (G.game == G_FIFTEEN) {
		/* la freccia sposta la tessera che sta da quella parte del buco */
		int b = fifteen_blank(), t = ch == BUI_K_LEFT ? b + 1 : ch == BUI_K_RIGHT ? b - 1
					   : ch == BUI_K_UP ? b + 4 : ch == BUI_K_DOWN ? b - 4 : -1;

		if (t >= 0 && t < 16) {
			fifteen_tap(t);
			return 1;
		}
	}
	return bui_panel_key(&G.p, k);
}

static void gm_tick(bs_view *v, uint32_t now)
{
	(void)now;
	if (G.game == G_SNAKE && G.alive)
		snake_step();
	else if (G.game == G_MEMORY)
		memory_hide(v);
}

static void gm_notify(bs_view *v, int what)
{
	(void)v;
	/* schermo spento o sospeso: il serpente si ferma (niente timer a vuoto) */
	if (what == BS_EV_SCREEN && bs_screen_state() == BS_SCREEN_OFF && G.alive) {
		G.alive = 0;
		bs_view_tick_every(0);
		set_status("Partita interrotta \xC2\xB7 tocca per ricominciare", 0, 0);
	}
}

bs_view bs_view_games = {
	.title = "Giochi",
	.flags = BS_VIEW_NO_HEADER,
	.enter = gm_enter,
	.leave = gm_leave,
	.layout = gm_layout,
	.draw = gm_draw,
	.pointer = gm_pointer,
	.key = gm_key,
	.tick = gm_tick,
	.notify = gm_notify,
};
