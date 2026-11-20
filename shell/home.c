// SPDX-License-Identifier: MIT
/*
 * home.c - finestra "desktop": sfondo, striscia delle informazioni, griglia
 * delle app (riordinabile, filtrabile da tastiera) e ospite delle viste
 * integrate (intestazione con "indietro" + area della vista).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

#define MAX_EXTRA 40
#define MAX_APPS 48
#define STRIP_H 72		/* striscia delle informazioni sopra la griglia */
#define NP_H 56			/* riga "in riproduzione" quando non entra nella striscia */
#define TRANS_OPEN_MS 180	/* apertura di una vista */
#define TRANS_CLOSE_MS 150	/* ritorno alla griglia */
#define BADGE 30		/* segno nasconde/mostra nel modo modifica */

static struct {
	bui_win *win;
	bui_px *wall;		/* sfondo calcolato una volta sola */
	bui_px *glass;		/* sfondo sfocato e schiarito per i riquadri "vetro" */
	int wall_w, wall_h, wall_top, wall_sig;
	bui_rect area;		/* sotto la barra di stato */
	bui_panel grid;

	/* striscia */
	char clock[8], day[24], date[40];
	bui_widget *w_clock, *w_date, *w_gauges, *w_np;
	int gauge[3];		/* CPU, memoria usata, disco usato: 0..100, -1 ignoto */
	char np_title[64];
	int np_state;

	/* griglia: ordine, riquadri nascosti, filtro */
	uint8_t order[MAX_APPS];
	uint8_t hid[MAX_APPS];	/* indicizzato come bs_apps[] */
	int norder;
	char filter[16];
	int g_x0, g_y0, g_tw, g_th, g_gap, g_cols, g_ntiles;

	/* modo modifica */
	int edit;
	int cap;		/* il tocco in corso e' su un riquadro (modo modifica) */
	int drag;		/* posizione in order[] del riquadro toccato */
	int drag_on;		/* lo sta trascinando */
	int badge;		/* il tocco e' partito dal segno nasconde/mostra */
	int press_x, press_y, grab_dx, grab_dy;
	bui_rect fly;		/* riquadro trascinato, coordinate della finestra */
	char edit_hint[96];

	/* ospite delle viste */
	bs_view *cur;
	bui_panel head;
	char title[64];
	int head_cap;		/* il tocco in corso appartiene all'intestazione */
	int tick_timer;
	int stats_timer;	/* CPU/RAM ogni 2 s, solo con la griglia in vista */
	int extra;		/* riquadri finti per provare lo scorrimento */
	char extra_label[MAX_EXTRA][24];

	/* transizione fra griglia e vista */
	struct {
		int active, closing;
		uint32_t start;
		int cy;		/* riga da cui si apre / verso cui si chiude */
		int lo, hi;	/* fascia [lo, hi) che mostra la vista */
	} tr;
} H;

static void grid_build(void);
static void tr_finish(void);

/* ------------------------------------------------------------------ */
/* sfondo                                                             */
/* ------------------------------------------------------------------ */

/*
 * Lo sfondo procedurale e' una sfumatura diagonale con fino a tre aloni
 * (parametri del tema). I colori sono calcolati su una griglia larga (celle
 * di 32 px) e interpolati per pixel con sole somme; il dithering ordinato
 * evita le bande del RGB565 e, se il tema lo chiede, un po' di grana rompe
 * le superfici troppo lisce.
 */
static void wall_node(int x, int y, int w, int h, int *rgb)
{
	const bui_theme *th = &bui_th;
	int big = w > h ? w : h;
	int t = ((x * 1024) / w + (y * 1024) / h) / 2;	/* 0..1024 lungo la diagonale */
	int lo = th->grain << 12, hi = (248 - th->grain) << 12;
	int f[3], i, b;

	if (t > 1024)
		t = 1024;
	if (t < 0)
		t = 0;
	for (b = 0; b < 3; b++) {
		int bx = w * th->blob[b].x / 100, by = h * th->blob[b].y / 100;
		long long r = big * th->blob[b].r / 100;
		long long d = (long long)(x - bx) * (x - bx) + (long long)(y - by) * (y - by);

		f[b] = 0;
		if (th->blob[b].rgb && r > 0) {
			f[b] = 1024 - (int)(d * 1024 / (r * r));
			f[b] = f[b] > 0 ? (f[b] * f[b]) >> 10 : 0;
		}
	}
	for (i = 0; i < 3; i++) {
		int sh = 16 - 8 * i;
		int c0 = (int)((th->wall0 >> sh) & 255) << 12, c1 = (int)((th->wall1 >> sh) & 255) << 12;
		int v = c0 + (int)(((long long)(c1 - c0) * t) >> 10);

		for (b = 0; b < 3; b++)
			v += (int)(((long long)((int)((th->blob[b].rgb >> sh) & 255) << 12) * f[b]) >> 10);
		/* sotto 248: il ciclo per pixel puo' fare a meno dei controlli di fondo scala */
		rgb[i] = v > hi ? hi : v < lo ? lo : v;
	}
}

/* media di un blocco dell'immagine attorno a un nodo (per il "vetro" di uno
 * sfondo fotografico): 8x8 campioni bastano, verra' comunque interpolato */
static void image_node(const bui_px *img, int x, int y, int w, int h, int top, int *rgb)
{
	int sx, sy, n = 0, r = 0, g = 0, b = 0;

	for (sy = -14; sy <= 14; sy += 4)
		for (sx = -14; sx <= 14; sx += 4) {
			int px = x + sx, py = y + sy;
			unsigned v;

			if (px < 0 || py < top || px >= w || py >= h)
				continue;
			v = img[py * w + px];
			r += (int)((v >> 11) & 31) << 3;
			g += (int)((v >> 5) & 63) << 2;
			b += (int)(v & 31) << 3;
			n++;
		}
	if (!n)
		n = 1;
	rgb[0] = (r / n) << 12;
	rgb[1] = (g / n) << 12;
	rgb[2] = (b / n) << 12;
}

/* interpola i nodi (gw per riga, celle di 32 px) fino ai pixel */
static void mesh_render(bui_px *dst, int w, int h, int top, const int *node, int gw, int grain, int *row)
{
	uint32_t seed = 0x2545F491u;
	int gx, x, y, i;

	if (top > h)
		top = h;
	/* le righe sotto la barra di stato non si vedono mai: non si calcolano */
	memset(dst, 0, sizeof(bui_px) * (size_t)w * (size_t)top);
	for (y = top; y < h; y++) {
		const int *n0 = &node[(y >> 5) * gw * 3], *n1 = n0 + gw * 3;
		int fy = y & 31;
		bui_px *d = dst + y * w;
		const uint8_t *th = &bui_bayer8[(y & 7) << 3];

		for (i = 0; i < gw * 3; i++)
			row[i] = n0[i] + (((n1[i] - n0[i]) * fy) >> 5);
		for (gx = 0, x = 0; x < w; gx++) {
			int r = row[gx * 3], g = row[gx * 3 + 1], b = row[gx * 3 + 2];
			int dr = (row[gx * 3 + 3] - r) >> 5, dg = (row[gx * 3 + 4] - g) >> 5;
			int db = (row[gx * 3 + 5] - b) >> 5;
			int end = x + 32 < w ? x + 32 : w;

			/* come bui_pack_dither(), senza saturazione (i nodi sono <= 248) */
			if (!grain) {
				for (; x < end; x++) {
					int t = th[x & 7];

					d[x] = (bui_px)((((r + (t << 9)) >> 15) << 11) |
							(((g + (t << 8)) >> 14) << 5) | ((b + (t << 9)) >> 15));
					r += dr;
					g += dg;
					b += db;
				}
				continue;
			}
			for (; x < end; x++) {
				int t = th[x & 7], n;

				/* rumore uguale sui tre canali (solo luminosita'), +-grain livelli */
				seed = seed * 1664525u + 1013904223u;
				n = ((int)(((seed >> 24) * (unsigned)(2 * grain + 1)) >> 8) - grain) * 4096;
				d[x] = (bui_px)((((r + n + (t << 9)) >> 15) << 11) |
						(((g + n + (t << 8)) >> 14) << 5) | ((b + n + (t << 9)) >> 15));
				r += dr;
				g += dg;
				b += db;
			}
		}
	}
}

/* immagine dell'utente: ~/.config/brainshell/wallpaper.rgb565, grezza, grande
 * quanto lo schermo. Viene avvicinata al colore di fondo del tema perche' il
 * testo resti leggibile qualunque cosa raffiguri. */
static int wall_load_user(int w, int h, int top)
{
	const char *home = getenv("HOME");
	char path[320];
	struct stat st;
	size_t want = (size_t)w * h * sizeof(bui_px), got = 0;
	int fd, x, y;

	if (!home || snprintf(path, sizeof(path), "%s/.config/brainshell/wallpaper.rgb565", home) >=
		     (int)sizeof(path))
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) < 0 || (size_t)st.st_size != want) {
		close(fd);
		return -1;
	}
	while (got < want) {
		ssize_t n = read(fd, (char *)H.wall + got, want - got);

		if (n <= 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	if (got != want)
		return -1;
	for (y = top; y < h; y++) {
		bui_px *d = H.wall + y * w;

		for (x = 0; x < w; x++)
			d[x] = bui_mix(d[x], BUI_C_BG, 88);
	}
	return 0;
}

static int wall_signature(void)
{
	return bui_theme_current() * 8 + bui_th.tile_style + 1;
}

static void wall_render(int w, int h)
{
	int gw = w / 32 + 2, gh = h / 32 + 2;
	int *node = malloc(sizeof(int) * 3 * (size_t)gw * gh);
	int *row = malloc(sizeof(int) * 3 * (size_t)gw);
	int want_glass = bui_th.tile_style == BUI_TILE_GLASS;
	int gx, gy, i, user;

	free(H.wall);
	free(H.glass);
	H.glass = NULL;
	bui_glass_set(NULL, 0, 0, 0);
	H.wall = malloc(sizeof(bui_px) * (size_t)w * h);
	H.wall_w = w;
	H.wall_h = h;
	H.wall_sig = wall_signature();
	if (!node || !row || !H.wall) {
		free(node);
		free(row);
		free(H.wall);
		H.wall = NULL;
		return;
	}
	user = wall_load_user(w, h, H.wall_top) == 0;
	if (!user) {
		for (gy = 0; gy < gh; gy++)
			for (gx = 0; gx < gw; gx++)
				wall_node(gx * 32, gy * 32, w, h, &node[(gy * gw + gx) * 3]);
		mesh_render(H.wall, w, h, H.wall_top, node, gw, bui_th.grain, row);
	}
	if (want_glass && (H.glass = malloc(sizeof(bui_px) * (size_t)w * h))) {
		/* il vetro: lo stesso sfondo (sfocato, se e' un'immagine) portato verso
		 * il colore dei controlli, piu' un velo chiaro */
		int tint[3] = { (bui_th.surface3 >> 11) << 3, ((bui_th.surface3 >> 5) & 63) << 2,
				(bui_th.surface3 & 31) << 3 };

		for (gy = 0; gy < gh; gy++)
			for (gx = 0; gx < gw; gx++) {
				int *n = &node[(gy * gw + gx) * 3];

				if (user)
					image_node(H.wall, gx * 32, gy * 32, w, h, H.wall_top, n);
				for (i = 0; i < 3; i++) {
					int v = (n[i] * 5 + (tint[i] << 12) * 3) / 8 + (10 << 12);

					n[i] = v > (248 << 12) ? 248 << 12 : v;
				}
			}
		mesh_render(H.glass, w, h, H.wall_top, node, gw, 0, row);
		bui_glass_set(H.glass, w, h, w);
	}
	free(node);
	free(row);
}

static void grid_bg(bui_panel *p, bui_canvas *c, bui_rect clip)
{
	(void)p;
	if (H.wall)
		bui_blit(c, 0, 0, H.wall, H.wall_w, H.wall_h, H.wall_w);
	else
		bui_fill(c, clip, BUI_C_BG);
}

/* ------------------------------------------------------------------ */
/* striscia delle informazioni                                        */
/* ------------------------------------------------------------------ */

static void strip_texts(void)
{
	char full[64], *sp;
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	snprintf(H.clock, sizeof(H.clock), "%02d:%02d", tm.tm_hour, tm.tm_min);
	/* "Giovedi' 19 novembre" -> giorno della settimana sopra, data sotto */
	bs_fmt_date(full, sizeof(full), 0);
	sp = strchr(full, ' ');
	if (sp)
		*sp++ = 0;
	snprintf(H.day, sizeof(H.day), "%.*s", (int)sizeof(H.day) - 1, full);
	snprintf(H.date, sizeof(H.date), "%.*s", (int)sizeof(H.date) - 1, sp ? sp : "");
}

/* legge i valori degli indicatori; ritorna 1 se qualcosa e' cambiato */
static int gauges_read(void)
{
	long total = 0, avail = 0;
	long long dt = 0, df = 0;
	int g[3] = { bs_sys_cpu(), -1, -1 };

	if (g[0] < 0)
		g[0] = H.gauge[0];	/* prima lettura: nessun intervallo ancora */
	if (bs_sys_meminfo(&total, &avail) == 0 && total > 0)
		g[1] = (int)(100 - avail * 100 / total);
	if (bs_sys_storage(&dt, &df) == 0 && dt > 0)
		g[2] = (int)(100 - df * 100 / dt);
	if (!memcmp(g, H.gauge, sizeof(g)))
		return 0;
	memcpy(H.gauge, g, sizeof(g));
	return 1;
}

static void draw_clock(bui_widget *w, bui_canvas *c, bui_rect r)
{
	(void)w;
	bui_text(c, BUI_FONT_NUM, r.x, r.y + (r.h + BUI_FONT_NUM->cap_h) / 2, H.clock, BUI_C_TEXT);
}

static void draw_date(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int y = r.y + (r.h - 46) / 2;

	(void)w;
	bui_text_box(c, BUI_FONT_TITLE, bui_r(r.x, y, r.w, 26), H.day, BUI_C_TEXT, BUI_AL_LEFT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(r.x, y + 26, r.w, 20), H.date, BUI_C_TEXT2, BUI_AL_LEFT);
}

/* scheda semitrasparente dei widget della striscia: sopra lo sfondo, o di vetro */
static void strip_card(bui_canvas *c, bui_rect r, int pressed)
{
	if (H.glass) {
		bui_blit_rr(c, r, bui_th.r_card, H.glass, H.wall_w, H.wall_h, H.wall_w);
		if (pressed)
			bui_rrect_a(c, r, bui_th.r_card, BUI_C_TEXT, 36);
		bui_rrect_line(c, r, bui_th.r_card, 1, bui_mix(BUI_C_SURFACE3, BUI_C_TEXT, 56));
	} else if (bui_th.tile_style == BUI_TILE_OUTLINE) {
		if (pressed)
			bui_rrect(c, r, bui_th.r_card, BUI_C_SURFACE3);
		bui_rrect_line(c, r, bui_th.r_card, 1, BUI_C_SURFACE3);
	} else {
		bui_rrect_a(c, r, bui_th.r_card, pressed ? BUI_C_SURFACE3 : BUI_C_SURFACE, pressed ? 255 : 208);
	}
}

static void stats_arm(void);

static void draw_gauges(bui_widget *w, bui_canvas *c, bui_rect r)
{
	static const char *const label[3] = { "CPU", "RAM", "Disco" };
	/* la colonna dei valori deve contenere "100%" per intero */
	int i, lw = 46, vw = bui_text_width(BUI_FONT_SMALL_B, "100%", -1) + 2;

	(void)w;
	strip_card(c, r, 0);
	for (i = 0; i < 3; i++) {
		int y = r.y + 9 + i * 18, v = H.gauge[i];
		bui_rect bar = bui_r(r.x + 12 + lw, y + 6, r.w - 24 - lw - vw, 6);
		bui_px col = v >= 90 ? BUI_C_DANGER : v >= 75 ? BUI_C_WARN : BUI_C_ACCENT;
		char val[16];

		bui_text_box(c, BUI_FONT_SMALL, bui_r(r.x + 12, y, lw, 18), label[i], BUI_C_TEXT2, BUI_AL_LEFT);
		bui_rrect(c, bar, 3, BUI_C_SURFACE3);
		if (v > 0)
			bui_rrect(c, bui_r(bar.x, bar.y, bar.w * v / 100 < 6 ? 6 : bar.w * v / 100, 6), 3, col);
		if (v >= 0)
			snprintf(val, sizeof(val), "%d%%", v);
		else
			snprintf(val, sizeof(val), "\xE2\x80\x94");
		bui_text_box(c, BUI_FONT_SMALL_B, bui_r(r.x + r.w - 10 - vw, y, vw, 18), val, BUI_C_TEXT,
			     BUI_AL_RIGHT);
	}
}

static void draw_np(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int d = r.h - 16 > 40 ? 40 : r.h - 16;
	bui_rect dot = bui_r(r.x + 10, r.y + (r.h - d) / 2, d, d);
	int tx = dot.x + d + 12;

	strip_card(c, r, w->flags & BUI_F_PRESSED);
	bui_rrect(c, dot, d / 2, BUI_C_ACCENT_BG);
	bui_icon_in(c, H.np_state == BS_NP_PLAYING ? BUI_IC(music, 24) : BUI_IC(pause, 24), dot,
		    BUI_C_ACCENT);
	bui_text_box(c, BUI_FONT_BODY_B, bui_r(tx, r.y + r.h / 2 - 22, r.x + r.w - 12 - tx, 22),
		     H.np_title, BUI_C_TEXT, BUI_AL_LEFT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(tx, r.y + r.h / 2 + 1, r.x + r.w - 12 - tx, 20),
		     H.np_state == BS_NP_PLAYING ? "In riproduzione" : "In pausa", BUI_C_TEXT2, BUI_AL_LEFT);
	if (w->flags & BUI_F_FOCUS)
		bui_rrect_line(c, r, bui_th.r_card, 2, BUI_C_ACCENT);
}

static void on_np(bui_widget *w)
{
	(void)w;
	bs_open("music");
}

void bs_home_set_now_playing(const char *title, int state)
{
	int had = H.np_state != BS_NP_NONE;

	if (!title || !*title)
		state = BS_NP_NONE;
	if (state == H.np_state && (state == BS_NP_NONE || !strcmp(title, H.np_title)))
		return;
	H.np_state = state;
	snprintf(H.np_title, sizeof(H.np_title), "%s", state != BS_NP_NONE ? title : "");
	if (!H.win)
		return;
	if (had != (state != BS_NP_NONE)) {
		/* compare o sparisce: la striscia cambia disposizione */
		if (!H.edit && !H.cap) {
			grid_build();
			if (!H.cur)
				bui_win_damage(H.win, H.area);
		}
	} else if (H.w_np && !H.cur) {
		bui_widget_damage(H.w_np);
	}
}

/* casella del filtro: compare al posto di ora e data mentre si scrive */
static void draw_search(bui_widget *w, bui_canvas *c, bui_rect r)
{
	bui_rect pill = bui_r(r.x, r.y + 6, r.w, 40);
	char text[48];
	int tw;

	(void)w;
	snprintf(text, sizeof(text), T("Cerca: %s"), H.filter);
	bui_rrect(c, pill, 20, BUI_C_SURFACE2);
	bui_rrect_line(c, pill, 20, 2, BUI_C_ACCENT);
	bui_text_box(c, BUI_FONT_BODY_B, bui_r(pill.x + 18, pill.y, pill.w - 36, pill.h), text, BUI_C_TEXT,
		     BUI_AL_LEFT);
	tw = bui_text_width(BUI_FONT_BODY_B, text, -1);
	if (tw < pill.w - 40)
		bui_fill(c, bui_r(pill.x + 18 + tw + 2, pill.y + 10, 2, 20), BUI_C_ACCENT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(r.x + 6, r.y + 48, r.w - 12, 20),
		     H.g_ntiles ? "Invio apre \xC2\xB7 Esc annulla" : "Nessuna app \xC2\xB7 Esc annulla",
		     BUI_C_TEXT2, BUI_AL_LEFT);
}

/* ------------------------------------------------------------------ */
/* ordine e riquadri nascosti                                         */
/* ------------------------------------------------------------------ */

static int app_index(const char *id, int len)
{
	int i;

	for (i = 0; i < bs_napps && i < MAX_APPS; i++)
		if ((int)strlen(bs_apps[i].id) == len && !strncmp(bs_apps[i].id, id, (size_t)len))
			return i;
	return -1;
}

/* dalle preferenze ("id,id,...") all'ordine in memoria; le app che il file
 * non conosce finiscono in coda, quelle che non esistono piu' si ignorano */
static void order_load(void)
{
	uint8_t seen[MAX_APPS] = { 0 };
	const char *p;
	int i;

	H.norder = 0;
	memset(H.hid, 0, sizeof(H.hid));
	for (p = bs_pref.order; *p;) {
		int len = (int)strcspn(p, ",");

		i = app_index(p, len);
		if (i >= 0 && !seen[i] && !(bs_apps[i].flags & BS_APP_HIDDEN)) {
			seen[i] = 1;
			H.order[H.norder++] = (uint8_t)i;
		}
		p += len + (p[len] == ',');
	}
	for (i = 0; i < bs_napps && i < MAX_APPS; i++)
		if (!seen[i] && !(bs_apps[i].flags & BS_APP_HIDDEN))
			H.order[H.norder++] = (uint8_t)i;
	for (p = bs_pref.hidden; *p;) {
		int len = (int)strcspn(p, ",");

		i = app_index(p, len);
		if (i >= 0)
			H.hid[i] = 1;
		p += len + (p[len] == ',');
	}
}

static void order_save(void)
{
	int i, no = 0, nh = 0;

	bs_pref.order[0] = bs_pref.hidden[0] = 0;
	for (i = 0; i < H.norder; i++) {
		const char *id = bs_apps[H.order[i]].id;

		if (no + (int)strlen(id) + 2 < (int)sizeof(bs_pref.order))
			no += snprintf(bs_pref.order + no, sizeof(bs_pref.order) - (size_t)no, "%s%s",
				       no ? "," : "", id);
		if (H.hid[H.order[i]] && nh + (int)strlen(id) + 2 < (int)sizeof(bs_pref.hidden))
			nh += snprintf(bs_pref.hidden + nh, sizeof(bs_pref.hidden) - (size_t)nh, "%s%s",
				       nh ? "," : "", id);
	}
	bs_conf_save();
}

/* il filtro corrisponde se una parola dell'etichetta inizia cosi' (solo ASCII) */
static int filter_match(const char *label)
{
	const char *p = T(label);	/* si cerca nella lingua in cui le etichette si leggono */
	int n = (int)strlen(H.filter), i;

	if (!n)
		return 1;
	while (*p) {
		for (i = 0; i < n && p[i] && ((unsigned char)p[i] | 32) == (unsigned char)H.filter[i]; i++)
			;
		if (i == n)
			return 1;
		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* griglia                                                            */
/* ------------------------------------------------------------------ */

static void open_app(const bs_app *a, int cy);
static void edit_end(void);
static void on_long_press(bui_panel *p, bui_widget *w);

static void on_tile(bui_widget *w)
{
	bui_rect r = bui_widget_rect(w);

	if (w->user)
		open_app(w->user, r.y + r.h / 2);
}

static void on_edit_done(bui_widget *w)
{
	(void)w;
	edit_end();
}

static bui_rect slot_rect(int n)
{
	return bui_r(H.g_x0 + (n % H.g_cols) * (H.g_tw + H.g_gap), H.g_y0 + (n / H.g_cols) * (H.g_th + H.g_gap),
		     H.g_tw, H.g_th);
}

static void grid_build(void)
{
	bui_panel *p = &H.grid;
	int w = H.area.w;
	int pad = w >= 640 ? 24 : 16, gap = 16;
	int cols = w >= 800 ? 5 : w >= 620 ? 4 : 3;
	int tw = (w - 2 * pad - (cols - 1) * gap) / cols, th = 152;
	int x0 = (w - (cols * tw + (cols - 1) * gap)) / 2;
	int sx = x0 + 4, sw = w - 2 * x0 - 8;	/* striscia */
	int y0 = 8 + STRIP_H + 8, i, n = 0, rows;
	int keep = p->sc.pos, np = H.np_state != BS_NP_NONE;
	int focus_id = p->focus && p->focus_vis && p->focus->type == BUI_W_TILE ? p->focus->id : -1;
	bui_widget *t;

	bui_panel_clear(p);
	p->paint_bg = grid_bg;
	p->glass = 1;
	p->no_scrollbar = 1;	/* la griglia ha il suo indicatore di pagina */
	p->on_long_press = on_long_press;
	H.w_clock = H.w_date = H.w_gauges = H.w_np = NULL;

	if (H.edit) {
		snprintf(H.edit_hint, sizeof(H.edit_hint), "%s",
			 w >= 640 ? "Trascina per riordinare \xC2\xB7 tocca il segno per nascondere o mostrare"
				  : "Trascina \xC2\xB7 il segno nasconde o mostra");
		t = bui_add_label(p, bui_r(sx, 12, sw - 136, 28), "Modifica della home", BUI_LS_TITLE,
				  BUI_AL_LEFT);
		t = bui_add_label(p, bui_r(sx, 42, sw - 136, 24), H.edit_hint, BUI_LS_SMALL, BUI_AL_LEFT);
		t->color = BUI_C_TEXT2;
		bui_add_button(p, bui_r(sx + sw - 124, 12, 124, BUI_TOUCH), "Fine", BUI_IC(check, 24),
			       BUI_BS_FILLED, on_edit_done);
	} else if (H.filter[0]) {
		bui_add_custom(p, bui_r(sx, 4, sw < 420 ? sw : 420, STRIP_H), draw_search);
	} else {
		int cw = bui_text_width(BUI_FONT_NUM, "00:00", -1) + 4;
		int gw = sw >= 700 ? 208 : 172, npw = 250;
		int dx = sx + cw + 10, gx = sx + sw - gw;

		strip_texts();
		H.w_clock = bui_add_custom(p, bui_r(sx, 8, cw, STRIP_H), draw_clock);
		if (sw >= 700) {
			/* largo: ora, data, indicatori e a destra "in riproduzione" */
			if (np) {
				gx -= npw + 12;
				H.w_np = bui_add_custom(p, bui_r(sx + sw - npw, 8 + 4, npw, STRIP_H - 8), draw_np);
			}
		} else if (np) {
			/* stretto: "in riproduzione" su una riga sua */
			H.w_np = bui_add_custom(p, bui_r(sx, 8 + STRIP_H + 8, sw, NP_H), draw_np);
			y0 += NP_H + 8;
		}
		H.w_date = bui_add_custom(p, bui_r(dx, 8, gx - 12 - dx, STRIP_H), draw_date);
		H.w_gauges = bui_add_custom(p, bui_r(gx, 8 + 2, gw, STRIP_H - 4), draw_gauges);
		if (H.w_np) {
			H.w_np->on_click = on_np;
			H.w_np->flags |= BUI_F_NOFOCUS;
		}
	}

	H.g_x0 = x0;
	H.g_y0 = y0;
	H.g_tw = tw;
	H.g_th = th;
	H.g_gap = gap;
	H.g_cols = cols;
	for (i = 0; i < H.norder + H.extra && p->n < BUI_PANEL_MAX - 1; i++) {
		bui_rect r = slot_rect(n);

		if (i < H.norder) {
			int ai = H.order[i];
			const bs_app *a = &bs_apps[ai];

			if (!H.edit && (H.hid[ai] || !filter_match(a->label)))
				continue;
			t = bui_add_tile(p, r, a->label, a->icon, H.edit ? NULL : on_tile);
			t->user = (void *)a;
			t->id = i;
			if (bui_th.tint[0])
				t->color = bui_th.tint[ai % 6];
			if ((a->flags & BS_APP_STUB) || H.hid[ai])
				t->flags |= BUI_F_DIM;
			if (H.edit) {
				t->flags |= BUI_F_NOFOCUS;
				if (H.cap && H.drag_on && i == H.drag)
					t->flags |= BUI_F_HIDDEN;	/* e' quello in volo */
			} else if (i == focus_id) {
				bui_panel_focus(p, t, 1);
			}
		} else {
			if (H.edit || H.filter[0])
				continue;
			/* riquadri finti (BRAINSHELL_EXTRA_TILES) per provare lo scorrimento */
			snprintf(H.extra_label[i - H.norder], sizeof(H.extra_label[0]), "Prova %d",
				 i - H.norder + 1);
			t = bui_add_tile(p, r, H.extra_label[i - H.norder], BUI_IC(apps, 40), on_tile);
			t->flags |= BUI_F_DIM;
			t->id = -1;
		}
		n++;
	}
	H.g_ntiles = n;
	rows = (n + cols - 1) / cols;
	bui_panel_set_viewport(p, H.area, y0 + (rows ? rows * (th + gap) - gap : 0) + (pad > 16 ? 16 : pad));
	p->sc.pos = keep > p->sc.max ? p->sc.max : keep;
}

/* riquadro (di un'app) sotto un punto della finestra */
static bui_widget *tile_at(int x, int y)
{
	int i;

	for (i = 0; i < H.grid.n; i++) {
		bui_widget *w = &H.grid.w[i];

		if (w->type == BUI_W_TILE && w->user && !(w->flags & BUI_F_HIDDEN) &&
		    bui_rect_has(bui_widget_rect(w), x, y))
			return w;
	}
	return NULL;
}

static bui_widget *tile_by_id(int id)
{
	int i;

	for (i = 0; i < H.grid.n; i++)
		if (H.grid.w[i].type == BUI_W_TILE && H.grid.w[i].user && H.grid.w[i].id == id)
			return &H.grid.w[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* modo modifica: riordinare (trascinando) e nascondere i riquadri    */
/* ------------------------------------------------------------------ */

static void edit_damage_all(void)
{
	if (!H.cur)
		bui_win_damage(H.win, H.area);
}

static void edit_end(void)
{
	if (!H.edit)
		return;
	H.edit = 0;
	H.cap = H.drag_on = 0;
	order_save();
	grid_build();
	edit_damage_all();
}

static void edit_fly_to(int x, int y)
{
	bui_rect old = H.fly, k;

	H.fly = bui_r(x - H.grab_dx, y - H.grab_dy, H.g_tw, H.g_th);
	if (bui_rect_isect(old, H.area, &k))
		bui_win_damage(H.win, k);
	if (bui_rect_isect(H.fly, H.area, &k))
		bui_win_damage(H.win, k);
}

static void edit_drag_start(int x, int y)
{
	bui_widget *w = tile_by_id(H.drag);

	H.drag_on = 1;
	H.badge = 0;
	if (w) {
		bui_rect r = bui_widget_rect(w);

		/* tiene il riquadro per il punto toccato (o per il centro, se nel
		 * frattempo la griglia si e' ridisposta) */
		if (bui_rect_has(r, x, y)) {
			H.grab_dx = x - r.x;
			H.grab_dy = y - r.y;
		} else {
			H.grab_dx = r.w / 2;
			H.grab_dy = r.h / 2;
		}
		bui_widget_set_flag(w, BUI_F_HIDDEN, 1);
	}
	H.fly = bui_r(0, 0, 0, 0);
	edit_fly_to(x, y);
}

static void on_long_press(bui_panel *p, bui_widget *w)
{
	(void)p;
	if (H.edit || H.cur || H.filter[0] || w->type != BUI_W_TILE || !w->user || w->id < 0)
		return;
	/* entra nel modo modifica col riquadro gia' in mano */
	H.edit = 1;
	H.cap = 1;
	H.drag = w->id;
	H.drag_on = 0;
	grid_build();
	edit_drag_start(H.grid.press_x, H.grid.press_y);
	edit_damage_all();
}

/* ritorna 1 se l'evento appartiene al modo modifica */
static int edit_pointer(const bui_pointer *ev)
{
	bui_widget *w;
	bui_rect r;
	int col, row, idx, dx, dy;

	switch (ev->type) {
	case BUI_PRESS:
		H.cap = 0;
		w = tile_at(ev->x, ev->y);
		if (!w || w->id < 0)
			return 0;	/* pulsante "Fine", scorrimento: al pannello */
		r = bui_widget_rect(w);
		H.cap = 1;
		H.drag = w->id;
		H.drag_on = 0;
		H.press_x = ev->x;
		H.press_y = ev->y;
		H.badge = ev->x >= r.x + r.w - 52 && ev->y < r.y + 52;
		return 1;
	case BUI_MOVE:
		if (!H.cap)
			return 0;
		dx = ev->x - H.press_x;
		dy = ev->y - H.press_y;
		if (!H.drag_on) {
			if (dx > -BUI_DRAG_SLOP && dx < BUI_DRAG_SLOP && dy > -BUI_DRAG_SLOP && dy < BUI_DRAG_SLOP)
				return 1;
			edit_drag_start(ev->x, ev->y);
		}
		/* vicino ai bordi la griglia scorre sotto il riquadro */
		if (H.grid.sc.max > 0 && (ev->y < H.area.y + 28 || ev->y > H.area.y + H.area.h - 28) &&
		    bui_scroll_by(&H.grid.sc, ev->y < H.area.y + 28 ? -20 : 20))
			edit_damage_all();
		/* su quale casella si trova il dito? */
		col = (ev->x - H.area.x - H.g_x0 + H.g_gap / 2) / (H.g_tw + H.g_gap);
		row = (ev->y - H.area.y + H.grid.sc.pos - H.g_y0 + H.g_gap / 2) / (H.g_th + H.g_gap);
		col = col < 0 ? 0 : col >= H.g_cols ? H.g_cols - 1 : col;
		idx = (row < 0 ? 0 : row) * H.g_cols + col;
		if (idx >= H.norder)
			idx = H.norder - 1;
		if (idx != H.drag && H.drag >= 0 && H.drag < H.norder) {
			uint8_t a = H.order[H.drag];

			if (idx > H.drag)
				memmove(&H.order[H.drag], &H.order[H.drag + 1], (size_t)(idx - H.drag));
			else
				memmove(&H.order[idx + 1], &H.order[idx], (size_t)(H.drag - idx));
			H.order[idx] = a;
			H.drag = idx;
			grid_build();
			edit_damage_all();
		}
		edit_fly_to(ev->x, ev->y);
		return 1;
	case BUI_RELEASE:
	case BUI_CANCEL:
		if (!H.cap)
			return 0;
		H.cap = 0;
		if (H.drag_on) {
			H.drag_on = 0;
			order_save();
			grid_build();
			edit_damage_all();
		} else if (H.badge && ev->type == BUI_RELEASE && H.drag >= 0 && H.drag < H.norder) {
			H.hid[H.order[H.drag]] ^= 1;
			order_save();
			grid_build();
			edit_damage_all();
		}
		return 1;
	default:
		return 0;
	}
}

/* cio' che il modo modifica disegna sopra la griglia */
static void edit_overlay(bui_canvas *c, bui_rect clip)
{
	bui_rect saved = bui_clip_push(c, clip);
	int i;

	for (i = 0; i < H.grid.n; i++) {
		bui_widget *w = &H.grid.w[i];
		bui_rect r, b;
		int hidden;

		if (w->type != BUI_W_TILE || !w->user || w->id < 0)
			continue;
		r = bui_widget_rect(w);
		if (w->flags & BUI_F_HIDDEN) {
			/* la casella lasciata libera dal riquadro in volo */
			bui_rrect_line(c, bui_rect_inset(r, 2, 2), bui_th.r_tile, 2, BUI_C_ACCENT);
			continue;
		}
		hidden = H.hid[H.order[w->id]];
		b = bui_r(r.x + r.w - BADGE - 8, r.y + 8, BADGE, BADGE);
		bui_rrect(c, b, BADGE / 2, hidden ? BUI_C_ACCENT : BUI_C_SURFACE3);
		bui_icon_in(c, hidden ? BUI_IC(add, 24) : BUI_IC(remove, 24), b,
			    hidden ? BUI_C_ON_ACCENT : BUI_C_TEXT);
	}
	if (H.cap && H.drag_on && H.fly.w > 0) {
		bui_widget *w = tile_by_id(H.drag);

		if (w) {
			bui_widget tmp = *w;

			tmp.flags &= (uint16_t)~BUI_F_HIDDEN;
			bui_shadow(c, H.fly, bui_th.r_tile, 12);
			bui_widget_draw(&tmp, c, H.fly);
			bui_rrect_line(c, H.fly, bui_th.r_tile, 2, BUI_C_ACCENT);
		}
	}
	bui_clip_pop(c, saved);
}

/* indicatore di pagina della griglia: un punto per schermata, la pastiglia
 * segue lo scorrimento (e' ridisegnata con la griglia, senza fotogrammi suoi) */
static void page_indicator(bui_canvas *c, bui_rect clip)
{
	const bui_scroll *s = &H.grid.sc;
	int pages, i, h, x, y, py;
	bui_rect saved;

	if (s->max <= 0)
		return;
	pages = (H.grid.content_h + H.area.h - 1) / H.area.h;
	if (pages < 2)
		pages = 2;
	if (pages > 6)
		pages = 6;
	h = pages * 12 - 6;
	x = H.area.x + H.area.w - 11;
	y = H.area.y + (H.area.h - h) / 2;
	if (!bui_rect_isect(clip, bui_r(x - 2, y - 6, 12, h + 12), NULL))
		return;
	saved = bui_clip_push(c, clip);
	for (i = 0; i < pages; i++)
		bui_rrect_a(c, bui_r(x, y + i * 12, 6, 6), 3, BUI_C_TEXT2, 120);
	py = y + (pages - 1) * 12 * s->pos / s->max;
	bui_rrect(c, bui_r(x - 1, py - 3, 8, 12), 4, BUI_C_ACCENT);
	bui_clip_pop(c, saved);
}

/* ------------------------------------------------------------------ */
/* transizione griglia <-> vista                                      */
/* ------------------------------------------------------------------ */

/*
 * La vista si apre come una fascia orizzontale che si allarga dalla riga del
 * riquadro toccato, e si richiude verso la stessa riga. Niente istantanee e
 * niente pixel ridisegnati due volte: a ogni fotogramma si disegnano (e si
 * inviano al pannello) soltanto le righe appena scoperte, quindi l'intera
 * animazione costa quanto UN ridisegno completo, spezzato in 4-6 fette; la
 * prima, la piu' grande, parte subito al tocco. Il tempo comanda: se una
 * fetta tarda, la successiva e' semplicemente piu' alta.
 */
static void tr_band(uint32_t now, int *lo, int *hi)
{
	int t = bui_tween(H.tr.start, H.tr.closing ? TRANS_CLOSE_MS : TRANS_OPEN_MS, now);
	int e = H.tr.closing ? 1024 - bui_ease_out(t) : bui_ease_out(t);
	int top = H.area.y, bot = H.area.y + H.area.h;

	/* e = 0: solo la riga cy; e = 1024: tutta l'area */
	*lo = (H.tr.cy - (H.tr.cy - top) * e / 1024) & ~3;	/* il pannello lavora a fasce di 4 righe */
	*hi = (H.tr.cy + (bot - H.tr.cy) * e / 1024 + 3) & ~3;
	if (*lo < top)
		*lo = top;
	if (*hi > bot)
		*hi = bot;
	if (t >= 1024) {
		*lo = H.tr.closing ? H.tr.cy : top;
		*hi = H.tr.closing ? H.tr.cy : bot;
	}
}

static void tr_damage(int y0, int y1)
{
	if (y1 > y0)
		bui_win_damage(H.win, bui_r(H.area.x, y0, H.area.w, y1 - y0));
}

static int tr_step(void *user, uint32_t now)
{
	int lo, hi, done;

	(void)user;
	if (!H.tr.active)
		return 0;
	tr_band(now, &lo, &hi);
	done = bui_tween(H.tr.start, H.tr.closing ? TRANS_CLOSE_MS : TRANS_OPEN_MS, now) >= 1024;
	if (H.tr.closing) {
		/* la griglia riappare sopra e sotto; +2: il filo sul bordo della fascia */
		tr_damage(H.tr.lo, done ? H.tr.hi : lo + 2);
		tr_damage(done ? hi : hi - 2, H.tr.hi);
	} else {
		tr_damage(lo, H.tr.lo + 2);
		tr_damage(H.tr.hi - 2, hi);
	}
	H.tr.lo = lo;
	H.tr.hi = hi;
	if (done)
		H.tr.active = 0;
	return !done;
}

/* salta alla fine (un tocco, un tasto, un cambio di dimensione) */
static void tr_finish(void)
{
	if (!H.tr.active)
		return;
	H.tr.active = 0;
	bui_anim_del(tr_step, NULL);
	bui_win_damage(H.win, H.area);
}

static void tr_start(int closing, int cy)
{
	uint32_t now = bui_now();

	tr_finish();
	if (bui_anim_level() != BUI_ANIM_FULL || !H.win->mapped || bs_screen_state() == BS_SCREEN_OFF) {
		bui_win_damage(H.win, H.area);
		return;
	}
	if (cy < H.area.y + 8 || cy > H.area.y + H.area.h - 8)
		cy = H.area.y + H.area.h / 2;
	H.tr.active = 1;
	H.tr.closing = closing;
	H.tr.cy = cy & ~3;
	/* come se fosse partita un fotogramma fa: la prima fetta si vede subito */
	H.tr.start = now - BUI_FRAME_MS;
	H.tr.lo = closing ? H.area.y : H.tr.cy;
	H.tr.hi = closing ? H.area.y + H.area.h : H.tr.cy;
	bui_anim_add(tr_step, NULL);
	tr_step(NULL, now);
}

/* ------------------------------------------------------------------ */
/* ospite delle viste                                                 */
/* ------------------------------------------------------------------ */

static void on_back(bui_widget *w)
{
	(void)w;
	bs_view_close();
}

static void view_layout(void)
{
	bs_view *v = H.cur;
	bui_rect a = H.area;

	if (!(v->flags & BS_VIEW_NO_HEADER)) {
		bui_header_build(&H.head, bui_r(a.x, a.y, a.w, BUI_HEADER_H), H.title, on_back);
		a.y += BUI_HEADER_H;
		a.h -= BUI_HEADER_H;
	} else {
		bui_panel_clear(&H.head);
		bui_panel_set_viewport(&H.head, bui_r(0, 0, 0, 0), 0);
	}
	v->area = a;
	if (v->layout)
		v->layout(v, a);
}

static void tick_stop(void)
{
	if (H.tick_timer)
		bui_timer_del(H.tick_timer);
	H.tick_timer = 0;
}

static void open_app(const bs_app *a, int cy)
{
	char msg[96];
	const char *tx = getenv("BRAINSHELL_TERMINAL");

	bs_keys_recent(a);
	/* app con una vista e un programma (Terminale): BRAINSHELL_TERMINAL=x11
	 * sceglie il programma, cioe' st in una sessione X come fino alla 0.3 */
	if (a->view && a->argv && tx && !strcmp(tx, "x11")) {
		if (bs_launch(a->argv, a->label) == 0) {
			snprintf(msg, sizeof(msg), T("Apertura di %s\xE2\x80\xA6"), T(a->label));
			bui_toast(msg, 1800);
		}
		return;
	}
	if (!a->view) {
		if (!a->argv)
			return;
		if (bs_launch(a->argv, a->label) == 0) {
			snprintf(msg, sizeof(msg), T("Apertura di %s\xE2\x80\xA6"), T(a->label));
			bui_toast(msg, 1800);
		}
		return;
	}
	tr_finish();
	if (H.edit)
		edit_end();
	if (H.cur) {
		/* da una vista all'altra: niente ritorno animato alla griglia */
		bs_view *v = H.cur;

		tick_stop();
		if (v->leave)
			v->leave(v);
		H.cur = NULL;
		bui_panel_clear(&H.head);
		cy = -1;
	}
	bui_panel_fx_cancel(&H.grid);
	if (H.filter[0]) {
		H.filter[0] = 0;
		grid_build();
		cy = -1;
	}
	H.cur = a->view;
	H.cur->app = a;
	H.cur->win = H.win;
	H.head_cap = 0;
	snprintf(H.title, sizeof(H.title), "%s", H.cur->title ? H.cur->title : a->label);
	H.cur->area = bui_r(H.area.x, H.area.y + BUI_HEADER_H, H.area.w, H.area.h - BUI_HEADER_H);
	{
		uint32_t t0 = bui_now(), t1;

		if (H.cur->enter)
			H.cur->enter(H.cur);
		t1 = bui_now();
		view_layout();
		if (bui_stats_on())
			fprintf(stderr, "bui-stats: apri %s: enter %u ms, layout %u ms\n", a->label,
				t1 - t0, bui_now() - t1);
	}
	tr_start(0, cy);
}

int bs_open(const char *id)
{
	int i;

	for (i = 0; i < bs_napps; i++)
		if (!strcmp(bs_apps[i].id, id)) {
			open_app(&bs_apps[i], -1);
			return 0;
		}
	return -1;
}

void bs_view_close(void)
{
	bs_view *v = H.cur;
	bui_widget *t = NULL;
	int i, cy = -1;

	if (!v || !H.win)
		return;
	tr_finish();
	tick_stop();
	if (v->leave)
		v->leave(v);
	H.cur = NULL;
	bui_panel_clear(&H.head);
	/* la griglia torna con ora e indicatori aggiornati */
	strip_texts();
	gauges_read();
	stats_arm();
	/* si richiude verso il riquadro dell'app, se e' in vista */
	for (i = 0; i < H.grid.n; i++)
		if (H.grid.w[i].type == BUI_W_TILE && H.grid.w[i].user == (void *)v->app)
			t = &H.grid.w[i];
	if (t) {
		bui_rect r = bui_widget_rect(t);

		cy = r.y + r.h / 2;
	}
	tr_start(1, cy);
}

void bs_home_go_grid(void)
{
	bs_view_close();
	edit_end();
	if (H.filter[0]) {
		H.filter[0] = 0;
		grid_build();
		bui_win_damage(H.win, H.area);
	}
}

const char *bs_home_view_id(void)
{
	return H.cur && H.cur->app ? H.cur->app->id : "grid";
}

int bs_view_is_active(const bs_view *v)
{
	return v && H.cur == v;
}

int bs_view_settled(const bs_view *v)
{
	return v && H.win && H.cur == v && !H.tr.active && H.win->mapped;
}

int bs_home_text_input(void)
{
	return H.cur && H.cur->text_input && H.cur->text_input(H.cur);
}

static void tick_fire(void *user)
{
	bs_view *v = H.cur;

	if (!user)
		H.tick_timer = 0;	/* era una scadenza singola */
	if (v && v->tick)
		v->tick(v, bui_now());
}

void bs_view_tick_in(int ms)
{
	tick_stop();
	if (ms > 0)
		H.tick_timer = bui_timer_add(ms, 0, tick_fire, NULL);
}

void bs_view_tick_every(int ms)
{
	tick_stop();
	if (ms > 0)
		H.tick_timer = bui_timer_add(ms, ms, tick_fire, &H);
}

void bs_view_damage(bui_rect r)
{
	if (H.win)
		bui_win_damage(H.win, r);
}

void bs_view_set_title(const char *title)
{
	snprintf(H.title, sizeof(H.title), "%s", title ? title : "");
	if (H.cur && !(H.cur->flags & BS_VIEW_NO_HEADER))
		bui_panel_damage(&H.head);
}

/* ------------------------------------------------------------------ */
/* finestra                                                           */
/* ------------------------------------------------------------------ */

static void paint_grid(bui_canvas *c, bui_rect k)
{
	bui_panel_draw(&H.grid, c, k);
	if (H.edit)
		edit_overlay(c, k);
	else
		page_indicator(c, k);
}

static void paint_view(bui_canvas *c, bui_rect k)
{
	bui_rect saved = bui_clip_push(c, k), v;

	bui_fill(c, k, BUI_C_BG);
	bui_panel_draw(&H.head, c, k);
	if (H.cur->draw && bui_rect_isect(k, H.cur->area, &v)) {
		bui_rect s2 = bui_clip_push(c, v);

		H.cur->draw(H.cur, c, v);
		bui_clip_pop(c, s2);
	}
	bui_clip_pop(c, saved);
}

static void home_paint(bui_win *win, bui_canvas *c, bui_rect clip)
{
	bui_rect k, part;

	(void)win;
	/* la striscia sotto la barra di stato non si vede mai */
	if (clip.y < H.area.y)
		bui_fill(c, bui_r(clip.x, clip.y, clip.w, H.area.y - clip.y), BUI_C_BAR);
	if (!bui_rect_isect(clip, H.area, &k))
		return;
	if (H.tr.active) {
		bui_px edge = bui_mix(BUI_C_BG, BUI_C_ACCENT, 150);
		int bot = H.area.y + H.area.h;

		/* fuori dalla fascia la griglia, dentro la vista. Durante la chiusura la
		 * vista non esiste piu': dentro la fascia restano i suoi vecchi pixel */
		if (bui_rect_isect(k, bui_r(H.area.x, H.area.y, H.area.w, H.tr.lo - H.area.y), &part))
			paint_grid(c, part);
		if (bui_rect_isect(k, bui_r(H.area.x, H.tr.hi, H.area.w, bot - H.tr.hi), &part))
			paint_grid(c, part);
		if (bui_rect_isect(k, bui_r(H.area.x, H.tr.lo, H.area.w, H.tr.hi - H.tr.lo), &part)) {
			bui_rect saved = bui_clip_push(c, part);

			if (!H.tr.closing && H.cur)
				paint_view(c, part);
			/* un filo d'accento segna i due bordi della fascia mentre si muove */
			if (H.tr.lo > H.area.y)
				bui_fill(c, bui_r(H.area.x, H.tr.lo, H.area.w, 2), edge);
			if (H.tr.hi < bot)
				bui_fill(c, bui_r(H.area.x, H.tr.hi - 2, H.area.w, 2), edge);
			bui_clip_pop(c, saved);
		}
		return;
	}
	if (!H.cur)
		paint_grid(c, k);
	else
		paint_view(c, k);
}

static void home_pointer(bui_win *win, const bui_pointer *ev)
{
	(void)win;
	if (ev->type == BUI_PRESS) {
		bs_wm_focus_home();
		tr_finish();
	}
	if (!H.cur) {
		if (H.edit && edit_pointer(ev))
			return;
		bui_panel_pointer(&H.grid, ev);
		return;
	}
	if (ev->type == BUI_PRESS || ev->type == BUI_WHEEL)
		H.head_cap = bui_rect_has(H.head.vp, ev->x, ev->y);
	if (H.head_cap)
		bui_panel_pointer(&H.head, ev);	/* puo' chiudere la vista */
	else if (H.cur->pointer)
		H.cur->pointer(H.cur, ev);
}

/* il filtro e' cambiato: ricostruisce la griglia e mette il fuoco sul primo riquadro */
static void filter_apply(void)
{
	int i;

	H.grid.focus = NULL;
	H.grid.sc.pos = 0;
	grid_build();
	if (H.filter[0])
		for (i = 0; i < H.grid.n; i++)
			if (H.grid.w[i].type == BUI_W_TILE) {
				bui_panel_focus(&H.grid, &H.grid.w[i], 1);
				break;
			}
	bui_win_damage(H.win, H.area);
}

static void home_key(bui_win *win, const bui_key *k)
{
	int len = (int)strlen(H.filter), ch = k->key;

	(void)win;
	tr_finish();
	if (H.cur) {
		if (H.cur->key && H.cur->key(H.cur, k))
			return;
		if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE)
			bs_view_close();
		return;
	}
	if (H.edit) {
		if (ch == BUI_K_ESC || ch == BUI_K_ENTER || ch == BUI_K_BACKSPACE)
			edit_end();
		else
			bui_panel_key(&H.grid, k);
		return;
	}
	/* scrivere filtra i riquadri: resta chi ha una parola che inizia cosi' */
	if (ch < 0x80 && !(k->mods & (BUI_MOD_CTRL | BUI_MOD_ALT)) &&
	    (((ch | 32) >= 'a' && (ch | 32) <= 'z') || (ch >= '0' && ch <= '9'))) {
		if (len < (int)sizeof(H.filter) - 1) {
			H.filter[len] = (char)(ch >= 'A' && ch <= 'Z' ? ch | 32 : ch);
			H.filter[len + 1] = 0;
			filter_apply();
		}
		return;
	}
	if (len && (ch == BUI_K_ESC || ch == BUI_K_BACKSPACE)) {
		H.filter[ch == BUI_K_ESC ? 0 : len - 1] = 0;
		filter_apply();
		return;
	}
	bui_panel_key(&H.grid, k);
}

/*
 * La finestra chiede tutto lo schermo e resta sotto la barra; ma certi
 * gestori la spostano nell'area di lavoro (sotto la barra). L'area utile
 * e' quindi calcolata dalla posizione reale: mai sotto la barra, mai fuori
 * dallo schermo.
 */
static void relayout(void)
{
	int top = BS_BAR_H - H.win->y;
	int vis = bui_screen_h() - H.win->y;

	if (top < 0)
		top = 0;
	if (vis > H.win->h || vis <= top)
		vis = H.win->h;
	H.tr.active = 0;
	H.cap = H.drag_on = 0;
	H.area = bui_r(0, top, H.win->w, vis - top);
	if (!H.wall || H.wall_w != H.win->w || H.wall_h != H.win->h || H.wall_top != top ||
	    H.wall_sig != wall_signature()) {
		H.wall_top = top;
		wall_render(H.win->w, H.win->h);
	}
	grid_build();
	if (H.cur)
		view_layout();
}

static void home_resized(bui_win *win)
{
	(void)win;
	relayout();
}

void bs_home_screen_changed(void)
{
	if (H.win)
		bui_win_move_resize(H.win, bui_r(0, 0, bui_screen_w(), bui_screen_h()));
}

/* CPU e RAM: ogni 2 s, ma solo con la griglia in vista e lo schermo acceso;
 * altrimenti il timer si toglie da solo (nessun risveglio inutile) */
static void stats_fire(void *user)
{
	(void)user;
	if (H.cur || bs_screen_state() == BS_SCREEN_OFF || !H.win) {
		bui_timer_del(H.stats_timer);
		H.stats_timer = 0;
		return;
	}
	if (gauges_read() && H.w_gauges)
		bui_widget_damage(H.w_gauges);
}

static void stats_arm(void)
{
	if (!H.stats_timer && !H.cur && bs_screen_state() != BS_SCREEN_OFF)
		H.stats_timer = bui_timer_add(2000, 2000, stats_fire, NULL);
}

/* ora, data e indicatori: si aggiornano allo scatto del minuto (nessun
 * risveglio in piu'), e solo se la griglia e' in vista e lo schermo acceso */
static void strip_refresh(void)
{
	char oc[sizeof(H.clock)], od[sizeof(H.day) + sizeof(H.date)];
	int visible = !H.cur && bs_screen_state() != BS_SCREEN_OFF;

	if (!visible)
		return;
	stats_arm();
	memcpy(oc, H.clock, sizeof(oc));
	snprintf(od, sizeof(od), "%s%s", H.day, H.date);
	strip_texts();
	if (strcmp(oc, H.clock) && H.w_clock)
		bui_widget_damage(H.w_clock);
	if (H.w_date && (strncmp(od, H.day, strlen(H.day)) || strcmp(od + strlen(H.day), H.date)))
		bui_widget_damage(H.w_date);
	if (gauges_read() && H.w_gauges)
		bui_widget_damage(H.w_gauges);
}

void bs_home_notify(int ev)
{
	if (!H.win)
		return;
	switch (ev) {
	case BS_EV_MINUTE:
	case BS_EV_BATTERY:
		strip_refresh();
		break;
	case BS_EV_SCREEN:
		if (bs_screen_state() == BS_SCREEN_ON)
			strip_refresh();
		else if (bs_screen_state() == BS_SCREEN_OFF)
			tr_finish();
		break;
	case BS_EV_THEME:
		/* colori, raggi e sfondo nuovi: tutto da rifare (una volta) */
		relayout();
		bui_win_damage_all(H.win);
		break;
	}
	if (H.cur && H.cur->notify)
		H.cur->notify(H.cur, ev);
}

void bs_home_shutdown(void)
{
	H.tr.active = 0;
	if (H.stats_timer)
		bui_timer_del(H.stats_timer);
	H.stats_timer = 0;
	if (H.cur) {
		tick_stop();
		if (H.cur->leave)
			H.cur->leave(H.cur);
		H.cur = NULL;
	}
	bui_panel_clear(&H.head);
	bui_panel_clear(&H.grid);
	bui_glass_set(NULL, 0, 0, 0);
	free(H.wall);
	free(H.glass);
	H.wall = H.glass = NULL;
	H.win = NULL;	/* chi chiama ancora (il lettore che si ferma all'uscita) non trova nulla */
}

bui_win *bs_home_init(void)
{
	const char *extra = getenv("BRAINSHELL_EXTRA_TILES");

	H.extra = extra ? atoi(extra) : 0;
	if (H.extra < 0)
		H.extra = 0;
	if (H.extra > MAX_EXTRA)
		H.extra = MAX_EXTRA;
	H.gauge[0] = H.gauge[1] = H.gauge[2] = -1;
	bs_sys_cpu();	/* punto di partenza per la prima misura */
	order_load();
	H.win = bui_win_create(BUI_WIN_DESKTOP, bui_r(0, 0, bui_screen_w(), bui_screen_h()),
			       "brainshell-home");
	if (!H.win)
		return NULL;
	H.win->paint = home_paint;
	H.win->pointer = home_pointer;
	H.win->key = home_key;
	H.win->resized = home_resized;
	bui_panel_init(&H.grid, H.win);
	bui_panel_init(&H.head, H.win);
	bs_wm_own(H.win);
	gauges_read();
	relayout();
	bui_win_show(H.win);
	stats_arm();
	return H.win;
}
