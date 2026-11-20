// SPDX-License-Identifier: MIT
/*
 * bui.c - tela RGB565 e primitive di disegno (solo aritmetica intera).
 */
#include <stdlib.h>
#include <string.h>

#include "bui.h"

/* ------------------------------------------------------------------ */
/* rettangoli                                                         */
/* ------------------------------------------------------------------ */

int bui_rect_isect(bui_rect a, bui_rect b, bui_rect *out)
{
	int x0 = a.x > b.x ? a.x : b.x;
	int y0 = a.y > b.y ? a.y : b.y;
	int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
	int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;

	if (x1 <= x0 || y1 <= y0) {
		if (out)
			*out = bui_r(0, 0, 0, 0);
		return 0;
	}
	if (out)
		*out = bui_r(x0, y0, x1 - x0, y1 - y0);
	return 1;
}

bui_rect bui_rect_union(bui_rect a, bui_rect b)
{
	int x0, y0, x1, y1;

	if (a.w <= 0 || a.h <= 0)
		return b;
	if (b.w <= 0 || b.h <= 0)
		return a;
	x0 = a.x < b.x ? a.x : b.x;
	y0 = a.y < b.y ? a.y : b.y;
	x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
	y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
	return bui_r(x0, y0, x1 - x0, y1 - y0);
}

bui_rect bui_clip_push(bui_canvas *c, bui_rect r)
{
	bui_rect old = c->clip;

	bui_rect_isect(old, r, &c->clip);
	return old;
}

/* ------------------------------------------------------------------ */
/* miscelazione                                                       */
/* ------------------------------------------------------------------ */

#define XMASK 0x07E0F81Fu

static inline uint32_t expand(bui_px p)
{
	return (p | ((uint32_t)p << 16)) & XMASK;
}

/* a: 0..32 */
static inline bui_px blend(bui_px bg, uint32_t fgx, unsigned a)
{
	uint32_t b = expand(bg);

	/* + mezzo passo in ogni campo: arrotonda invece di troncare */
	b = ((fgx * a + b * (32 - a) + 0x02008010u) >> 5) & XMASK;
	return (bui_px)(b | (b >> 16));
}

bui_px bui_mix(bui_px bg, bui_px fg, int a)
{
	return blend(bg, expand(fg), (unsigned)(a + 4) >> 3);
}

const uint8_t bui_bayer8[64] = {
	 0, 32,  8, 40,  2, 34, 10, 42,
	48, 16, 56, 24, 50, 18, 58, 26,
	12, 44,  4, 36, 14, 46,  6, 38,
	60, 28, 52, 20, 62, 30, 54, 22,
	 3, 35, 11, 43,  1, 33,  9, 41,
	51, 19, 59, 27, 49, 17, 57, 25,
	15, 47,  7, 39, 13, 45,  5, 37,
	63, 31, 55, 23, 61, 29, 53, 21,
};

/* ------------------------------------------------------------------ */
/* riempimenti                                                        */
/* ------------------------------------------------------------------ */

static inline void fill_row(bui_px *d, int n, bui_px col)
{
	while (n >= 4) {
		d[0] = col;
		d[1] = col;
		d[2] = col;
		d[3] = col;
		d += 4;
		n -= 4;
	}
	while (n-- > 0)
		*d++ = col;
}

void bui_fill(bui_canvas *c, bui_rect r, bui_px col)
{
	int y;

	if (!bui_rect_isect(r, c->clip, &r))
		return;
	for (y = r.y; y < r.y + r.h; y++)
		fill_row(c->px + y * c->stride + r.x, r.w, col);
}

void bui_fill_a(bui_canvas *c, bui_rect r, bui_px col, int alpha)
{
	unsigned a = (unsigned)(alpha + 4) >> 3;
	uint32_t fx = expand(col);
	int x, y;

	if (a >= 32) {
		bui_fill(c, r, col);
		return;
	}
	if (!a || !bui_rect_isect(r, c->clip, &r))
		return;
	for (y = r.y; y < r.y + r.h; y++) {
		bui_px *d = c->px + y * c->stride + r.x;

		for (x = 0; x < r.w; x++)
			d[x] = blend(d[x], fx, a);
	}
}

void bui_grad_v(bui_canvas *c, bui_rect r, uint32_t rgb0, uint32_t rgb1)
{
	bui_rect k;
	int r0 = (rgb0 >> 16) & 255, g0 = (rgb0 >> 8) & 255, b0 = rgb0 & 255;
	int r1 = (rgb1 >> 16) & 255, g1 = (rgb1 >> 8) & 255, b1 = rgb1 & 255;
	int y, x, den = r.h > 1 ? r.h - 1 : 1;

	if (!bui_rect_isect(r, c->clip, &k))
		return;
	for (y = k.y; y < k.y + k.h; y++) {
		int t = y - r.y;
		/* canali a 8.12 bit */
		int cr = (r0 << 12) + ((r1 - r0) * t * 4096) / den;
		int cg = (g0 << 12) + ((g1 - g0) * t * 4096) / den;
		int cb = (b0 << 12) + ((b1 - b0) * t * 4096) / den;
		bui_px pat[8];
		bui_px *d = c->px + y * c->stride;

		for (x = 0; x < 8; x++)
			pat[x] = bui_pack_dither(cr, cg, cb, x, y);
		for (x = k.x; x < k.x + k.w; x++)
			d[x] = pat[x & 7];
	}
}

/* ------------------------------------------------------------------ */
/* maschere A8                                                        */
/* ------------------------------------------------------------------ */

static void mask_blit(bui_canvas *c, int x, int y, const uint8_t *m, int w, int h, int stride,
		      bui_px col, int alpha, int flipx, int flipy)
{
	bui_rect k;
	uint32_t fx = expand(col);
	int amul = alpha + 1;
	int yy, xx;

	if (alpha <= 0 || !bui_rect_isect(bui_r(x, y, w, h), c->clip, &k))
		return;
	for (yy = k.y; yy < k.y + k.h; yy++) {
		int sy = flipy ? h - 1 - (yy - y) : yy - y;
		const uint8_t *s = m + sy * stride;
		bui_px *d = c->px + yy * c->stride;

		for (xx = k.x; xx < k.x + k.w; xx++) {
			unsigned a = s[flipx ? w - 1 - (xx - x) : xx - x];

			if (!a)
				continue;
			if (alpha < 255)
				a = (a * amul) >> 8;
			a = (a + 4) >> 3;
			if (a >= 32)
				d[xx] = col;
			else if (a)
				d[xx] = blend(d[xx], fx, a);
		}
	}
}

void bui_mask(bui_canvas *c, int x, int y, const uint8_t *a8, int w, int h, int stride,
	      bui_px col, int alpha)
{
	mask_blit(c, x, y, a8, w, h, stride, col, alpha, 0, 0);
}

void bui_blit(bui_canvas *c, int x, int y, const bui_px *src, int w, int h, int stride)
{
	bui_rect k;
	int yy;

	if (!bui_rect_isect(bui_r(x, y, w, h), c->clip, &k))
		return;
	for (yy = k.y; yy < k.y + k.h; yy++)
		memcpy(c->px + yy * c->stride + k.x, src + (yy - y) * stride + (k.x - x),
		       (size_t)k.w * sizeof(bui_px));
}

void bui_blit_a(bui_canvas *c, int x, int y, const bui_px *src, int w, int h, int stride, int alpha)
{
	unsigned a = (unsigned)(alpha + 4) >> 3;
	bui_rect k;
	int yy, xx;

	if (a >= 32) {
		bui_blit(c, x, y, src, w, h, stride);
		return;
	}
	if (!a || !bui_rect_isect(bui_r(x, y, w, h), c->clip, &k))
		return;
	for (yy = k.y; yy < k.y + k.h; yy++) {
		const bui_px *s = src + (yy - y) * stride - x;
		bui_px *d = c->px + yy * c->stride;

		for (xx = k.x; xx < k.x + k.w; xx++)
			d[xx] = blend(d[xx], expand(s[xx]), a);
	}
}

/* ------------------------------------------------------------------ */
/* rettangoli arrotondati                                             */
/* ------------------------------------------------------------------ */

static unsigned isqrt32(uint32_t v)
{
	uint32_t r = 0, b = 1u << 30;

	while (b > v)
		b >>= 2;
	while (b) {
		if (v >= r + b) {
			v -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return r;
}

/*
 * Copertura dell'angolo in alto a sinistra di un cerchio di raggio r
 * (t > 0: solo una corona spessa t). Le maschere restano in una piccola
 * cache: i raggi usati dall'interfaccia sono pochi.
 */
#define CORNER_CACHE 12
#define CORNER_MAX 64

static struct corner {
	int r, t;
	uint8_t *m;
	unsigned age;
} corners[CORNER_CACHE];
static unsigned corner_clock;

static int cover(int r32, int d32)
{
	int v = r32 + 16 - d32;		/* (r - d + 0.5) * 32 */

	if (v <= 0)
		return 0;
	if (v >= 32)
		return 255;
	return v * 255 / 32;
}

static const uint8_t *corner_mask(int r, int t)
{
	struct corner *e = &corners[0];
	int i, j;

	for (i = 0; i < CORNER_CACHE; i++) {
		if (corners[i].m && corners[i].r == r && corners[i].t == t) {
			corners[i].age = ++corner_clock;
			return corners[i].m;
		}
		if (corners[i].age < e->age)
			e = &corners[i];
	}
	free(e->m);
	e->m = malloc((size_t)r * r);
	if (!e->m) {
		e->r = 0;
		return NULL;
	}
	e->r = r;
	e->t = t;
	e->age = ++corner_clock;
	for (j = 0; j < r; j++)
		for (i = 0; i < r; i++) {
			int dx = 2 * (r - i) - 1, dy = 2 * (r - j) - 1;
			int d32 = (int)isqrt32((uint32_t)(dx * dx + dy * dy) << 8);
			int a = cover(r * 32, d32);

			if (t > 0)
				a -= cover((r - t) * 32, d32);
			e->m[j * r + i] = (uint8_t)a;
		}
	return e->m;
}

void bui_rrect_a(bui_canvas *c, bui_rect r, int rad, bui_px col, int alpha)
{
	const uint8_t *m;

	if (r.w <= 0 || r.h <= 0 || !bui_rect_isect(r, c->clip, NULL))
		return;
	if (rad > r.w / 2)
		rad = r.w / 2;
	if (rad > r.h / 2)
		rad = r.h / 2;
	if (rad > CORNER_MAX)
		rad = CORNER_MAX;
	if (rad < 2 || !(m = corner_mask(rad, 0))) {
		bui_fill_a(c, r, col, alpha);
		return;
	}
	bui_fill_a(c, bui_r(r.x + rad, r.y, r.w - 2 * rad, rad), col, alpha);
	bui_fill_a(c, bui_r(r.x, r.y + rad, r.w, r.h - 2 * rad), col, alpha);
	bui_fill_a(c, bui_r(r.x + rad, r.y + r.h - rad, r.w - 2 * rad, rad), col, alpha);
	mask_blit(c, r.x, r.y, m, rad, rad, rad, col, alpha, 0, 0);
	mask_blit(c, r.x + r.w - rad, r.y, m, rad, rad, rad, col, alpha, 1, 0);
	mask_blit(c, r.x, r.y + r.h - rad, m, rad, rad, rad, col, alpha, 0, 1);
	mask_blit(c, r.x + r.w - rad, r.y + r.h - rad, m, rad, rad, rad, col, alpha, 1, 1);
}

void bui_rrect(bui_canvas *c, bui_rect r, int rad, bui_px col)
{
	bui_rrect_a(c, r, rad, col, 255);
}

void bui_rrect_line(bui_canvas *c, bui_rect r, int rad, int t, bui_px col)
{
	const uint8_t *m;

	if (r.w <= 0 || r.h <= 0 || t <= 0 || !bui_rect_isect(r, c->clip, NULL))
		return;
	if (rad > r.w / 2)
		rad = r.w / 2;
	if (rad > r.h / 2)
		rad = r.h / 2;
	if (rad > CORNER_MAX)
		rad = CORNER_MAX;
	if (rad < t)
		rad = t;
	if (rad < 2 || !(m = corner_mask(rad, t))) {
		bui_fill(c, bui_r(r.x, r.y, r.w, t), col);
		bui_fill(c, bui_r(r.x, r.y + r.h - t, r.w, t), col);
		bui_fill(c, bui_r(r.x, r.y + t, t, r.h - 2 * t), col);
		bui_fill(c, bui_r(r.x + r.w - t, r.y + t, t, r.h - 2 * t), col);
		return;
	}
	bui_fill(c, bui_r(r.x + rad, r.y, r.w - 2 * rad, t), col);
	bui_fill(c, bui_r(r.x + rad, r.y + r.h - t, r.w - 2 * rad, t), col);
	bui_fill(c, bui_r(r.x, r.y + rad, t, r.h - 2 * rad), col);
	bui_fill(c, bui_r(r.x + r.w - t, r.y + rad, t, r.h - 2 * rad), col);
	mask_blit(c, r.x, r.y, m, rad, rad, rad, col, 255, 0, 0);
	mask_blit(c, r.x + r.w - rad, r.y, m, rad, rad, rad, col, 255, 1, 0);
	mask_blit(c, r.x, r.y + r.h - rad, m, rad, rad, rad, col, 255, 0, 1);
	mask_blit(c, r.x + r.w - rad, r.y + r.h - rad, m, rad, rad, rad, col, 255, 1, 1);
}

void bui_blit_rr(bui_canvas *c, bui_rect r, int rad, const bui_px *src, int sw, int sh, int stride)
{
	const uint8_t *m = NULL;
	bui_rect k;
	int y, x;

	if (!src || !bui_rect_isect(r, c->clip, &k) || !bui_rect_isect(k, bui_r(0, 0, sw, sh), &k))
		return;
	if (rad > r.w / 2)
		rad = r.w / 2;
	if (rad > r.h / 2)
		rad = r.h / 2;
	if (rad > CORNER_MAX)
		rad = CORNER_MAX;
	if (rad >= 2)
		m = corner_mask(rad, 0);
	for (y = k.y; y < k.y + k.h; y++) {
		const bui_px *s = src + y * stride;
		bui_px *d = c->px + y * c->stride;
		int j = y - r.y, x0 = k.x, x1 = k.x + k.w;
		const uint8_t *mr = NULL;

		if (m && j < rad)
			mr = m + j * rad;
		else if (m && j >= r.h - rad)
			mr = m + (r.h - 1 - j) * rad;
		if (mr) {
			/* gli angoli: miscela con la copertura, il resto e' una copia */
			for (x = x0; x < x1 && x < r.x + rad; x++) {
				unsigned a = (mr[x - r.x] + 4u) >> 3;

				if (a)
					d[x] = a >= 32 ? s[x] : blend(d[x], expand(s[x]), a);
			}
			for (x = r.x + r.w - rad > x0 ? r.x + r.w - rad : x0; x < x1; x++) {
				unsigned a = (mr[r.x + r.w - 1 - x] + 4u) >> 3;

				if (a)
					d[x] = a >= 32 ? s[x] : blend(d[x], expand(s[x]), a);
			}
			if (x0 < r.x + rad)
				x0 = r.x + rad;
			if (x1 > r.x + r.w - rad)
				x1 = r.x + r.w - rad;
		}
		if (x1 > x0)
			memcpy(d + x0, s + x0, (size_t)(x1 - x0) * sizeof(bui_px));
	}
}

static void bui_shadow_free(void);

int bui_corner_inset(int radius, int row)
{
	const uint8_t *m;
	int i = 0;

	if (radius < 0) {	/* richiesta speciale: libera la cache */
		for (i = 0; i < CORNER_CACHE; i++) {
			free(corners[i].m);
			memset(&corners[i], 0, sizeof(corners[i]));
		}
		bui_shadow_free();
		return 0;
	}
	if (radius < 2 || radius > CORNER_MAX || row < 0 || row >= radius || !(m = corner_mask(radius, 0)))
		return 0;
	while (i < radius && !m[row * radius + i])
		i++;
	return i;
}

/*
 * Ombre. La forma dipende solo da raggio e ampiezza: l'angolo (un quadrato
 * di lato raggio + ampiezza) e il profilo dei bordi sono calcolati una volta
 * e tenuti come livelli di miscela 0..13 (su 32). Disegnare un'ombra costa
 * poi solo la miscela dell'anello attorno al rettangolo.
 */
#define SHADOW_CACHE 4
#define SHADOW_MAX 13		/* buio massimo, su 32 */

static struct shade {
	int radius, spread;
	uint8_t *m;		/* S*S livelli dell'angolo + S del profilo del bordo */
	unsigned age;
} shades[SHADOW_CACHE];

static const uint8_t *shade_mask(int radius, int spread)
{
	struct shade *e = &shades[0];
	int S = radius + spread, i, j;

	for (i = 0; i < SHADOW_CACHE; i++) {
		if (shades[i].m && shades[i].radius == radius && shades[i].spread == spread) {
			shades[i].age = ++corner_clock;
			return shades[i].m;
		}
		if (shades[i].age < e->age)
			e = &shades[i];
	}
	free(e->m);
	e->m = malloc((size_t)S * S + (size_t)S);
	if (!e->m) {
		e->radius = e->spread = 0;
		return NULL;
	}
	e->radius = radius;
	e->spread = spread;
	e->age = ++corner_clock;
	for (j = 0; j <= S; j++)
		for (i = 0; i < S; i++) {
			/* distanza dal bordo del rettangolo arrotondato; la riga j == S
			 * e' il profilo dei lati dritti (dy = 0) */
			int dx = S - i, dy = S - j;
			int dist = (dy ? (int)isqrt32((uint32_t)(dx * dx + dy * dy)) : dx) - radius;
			int t = spread - dist, a;

			if (dist <= 0)
				a = SHADOW_MAX;
			else if (t <= 0)
				a = 0;
			else
				a = SHADOW_MAX * t * t / (spread * spread);
			e->m[j * S + i] = (uint8_t)a;
		}
	return e->m;
}

static void shade_span(bui_canvas *c, int x0, int x1, int y, unsigned a)
{
	bui_px *d;

	if (!a || y < c->clip.y || y >= c->clip.y + c->clip.h)
		return;
	if (x0 < c->clip.x)
		x0 = c->clip.x;
	if (x1 > c->clip.x + c->clip.w)
		x1 = c->clip.x + c->clip.w;
	d = c->px + y * c->stride;
	for (; x0 < x1; x0++)
		d[x0] = blend(d[x0], 0, a);
}

void bui_shadow(bui_canvas *c, bui_rect r, int radius, int spread)
{
	/* l'ombra cade un po' verso il basso */
	bui_rect s = bui_r(r.x, r.y + spread / 3, r.w, r.h), k;
	const uint8_t *m;
	int S, x, y;

	if (spread < 1 || spread > 48 ||
	    !bui_rect_isect(bui_r(s.x - spread, s.y - spread, s.w + 2 * spread, s.h + 2 * spread), c->clip, &k))
		return;
	if (radius > s.w / 2)
		radius = s.w / 2;
	if (radius > s.h / 2)
		radius = s.h / 2;
	if (radius > CORNER_MAX)
		radius = CORNER_MAX;
	if (radius < 0)
		radius = 0;
	S = radius + spread;
	m = shade_mask(radius, spread);
	if (!m)
		return;
	for (y = k.y; y < k.y + k.h; y++) {
		bui_px *d = c->px + y * c->stride;
		int j = y - (s.y - spread);		/* riga dall'alto dell'ombra */
		int jb = s.y + s.h + spread - 1 - y;	/* ... e dal basso */
		const uint8_t *row;
		/* colonne coperte dal pannello in questa riga (non serve scurirle) */
		int cx0 = 0, cx1 = 0;

		if (y >= r.y + radius && y < r.y + r.h - radius) {
			cx0 = r.x;
			cx1 = r.x + r.w;
		} else if (y >= r.y && y < r.y + r.h) {
			cx0 = r.x + radius;
			cx1 = r.x + r.w - radius;
		}
		if (j > jb)
			j = jb;
		row = m + (j < S ? j : S) * S;
		/* angolo (o bordo) sinistro e destro */
		for (x = k.x; x < s.x + radius && x < k.x + k.w; x++) {
			unsigned a = row[x - (s.x - spread)];

			if (a && (x < cx0 || x >= cx1))
				d[x] = blend(d[x], 0, a);
		}
		x = s.x + s.w - radius;
		if (x < k.x)
			x = k.x;
		for (; x < k.x + k.w; x++) {
			unsigned a = row[s.x + s.w + spread - 1 - x];

			if (a && (x < cx0 || x >= cx1))
				d[x] = blend(d[x], 0, a);
		}
		/* tratto centrale: uniforme lungo la riga; sotto il pannello non serve */
		if (cx1 <= cx0)
			shade_span(c, s.x + radius, s.x + s.w - radius, y, row[S - 1]);
	}
}

static void bui_shadow_free(void)
{
	int i;

	for (i = 0; i < SHADOW_CACHE; i++) {
		free(shades[i].m);
		memset(&shades[i], 0, sizeof(shades[i]));
	}
}

/* ------------------------------------------------------------------ */
/* testo                                                              */
/* ------------------------------------------------------------------ */

/* decodifica un carattere UTF-8; le sequenze non valide diventano '?' */
static unsigned utf8_next(const char **ps, const char *end)
{
	const unsigned char *s = (const unsigned char *)*ps;
	unsigned cp = *s++;
	int n = 0;

	if (cp >= 0xF0) {
		cp &= 7;
		n = 3;
	} else if (cp >= 0xE0) {
		cp &= 15;
		n = 2;
	} else if (cp >= 0xC0) {
		cp &= 31;
		n = 1;
	} else if (cp >= 0x80) {
		cp = '?';
	}
	while (n-- > 0) {
		if ((const char *)s >= end || (*s & 0xC0) != 0x80) {
			cp = '?';
			break;
		}
		cp = (cp << 6) | (*s++ & 0x3F);
	}
	*ps = (const char *)s;
	return cp;
}

static const bui_glyph *glyph_find(const bui_font *f, unsigned cp)
{
	int lo = 0, hi = f->nglyphs - 1;

	/* i font di testo iniziano con l'ASCII in ordine: accesso diretto */
	if (cp >= 0x20 && cp - 0x20 < f->nglyphs && f->glyphs[cp - 0x20].cp == cp)
		return &f->glyphs[cp - 0x20];
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

static int kern(const bui_font *f, unsigned a, unsigned b)
{
	int lo = 0, hi = f->nkern - 1;
	unsigned key;

	if (!f->nkern || a < 0x21 || a > 0x7E || b < 0x21 || b > 0x7E)
		return 0;
	key = (a << 8) | b;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;

		if (f->kpair[mid] == key)
			return f->kdx[mid];
		if (f->kpair[mid] < key)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

/* disegna (c != NULL) o misura; ritorna l'avanzamento in 1/64 px */
static int text_run(bui_canvas *c, const bui_font *f, int x, int y, const char *s, int len, bui_px col)
{
	const char *end = len < 0 ? s + strlen(s) : s + len;
	int pen = 0;
	unsigned prev = 0;

	while (s < end) {
		unsigned cp = utf8_next(&s, end);
		const bui_glyph *g = glyph_find(f, cp);

		if (!g && !(g = glyph_find(f, '?')))
			continue;
		pen += kern(f, prev, cp);
		if (c && g->w)
			mask_blit(c, x + ((pen + 32) >> 6) + g->bx, y - g->by, f->data + g->off,
				  g->w, g->h, g->w, col, 255, 0, 0);
		pen += g->adv;
		prev = cp;
	}
	return pen;
}

int bui_text_width(const bui_font *f, const char *s, int len)
{
	return (text_run(NULL, f, 0, 0, s, len, 0) + 63) >> 6;
}

int bui_text(bui_canvas *c, const bui_font *f, int x, int y, const char *s, bui_px col)
{
	/* fuori dal clip in verticale: niente da fare */
	if (y + f->descent < c->clip.y || y - f->ascent >= c->clip.y + c->clip.h)
		return 0;
	return (text_run(c, f, x, y, s, -1, col) + 63) >> 6;
}

static const char *ellipsis(const bui_font *f)
{
	return glyph_find(f, 0x2026) ? "\xE2\x80\xA6" : "...";
}

void bui_text_box(bui_canvas *c, const bui_font *f, bui_rect r, const char *s, bui_px col, int align)
{
	int w, x, y, len = -1;
	const char *dots = NULL;

	if (!s || !*s || !bui_rect_isect(r, c->clip, NULL))
		return;
	w = bui_text_width(f, s, -1);
	if (w > r.w) {
		/* tronca: cerca il prefisso piu' lungo che entra coi puntini */
		const char *p = s, *end = s + strlen(s), *best = s;
		int dw;

		dots = ellipsis(f);
		dw = bui_text_width(f, dots, -1);
		while (p < end) {
			const char *q = p;

			utf8_next(&q, end);
			if (bui_text_width(f, s, (int)(q - s)) + dw > r.w)
				break;
			best = p = q;
		}
		len = (int)(best - s);
		while (len > 0 && s[len - 1] == ' ')
			len--;
		w = bui_text_width(f, s, len) + dw;
	}
	if (align & BUI_AL_CENTER)
		x = r.x + (r.w - w) / 2;
	else if (align & BUI_AL_RIGHT)
		x = r.x + r.w - w;
	else
		x = r.x;
	if (align & BUI_AL_TOP)
		y = r.y + f->ascent;
	else if (align & BUI_AL_BOTTOM)
		y = r.y + r.h - f->descent;
	else
		y = r.y + (r.h + f->cap_h) / 2;
	if (dots) {
		int adv = (text_run(c, f, x, y, s, len, col) + 32) >> 6;

		text_run(c, f, x + adv, y, dots, -1, col);
	} else {
		text_run(c, f, x, y, s, -1, col);
	}
}

int bui_text_wrap(bui_canvas *c, const bui_font *f, bui_rect r, const char *s, bui_px col,
		  int line_h, int align)
{
	const char *end = s + strlen(s);
	int y = r.y;

	if (line_h <= 0)
		line_h = f->line_h;
	while (s < end) {
		const char *p = s, *brk = NULL, *stop;
		int w, x;

		/* avanza parola per parola finche' la riga entra */
		for (;;) {
			const char *q = p;

			while (q < end && *q != ' ' && *q != '\n')
				q++;
			if (bui_text_width(f, s, (int)(q - s)) > r.w && brk)
				break;
			brk = q;
			if (q >= end || *q == '\n')
				break;
			p = q + 1;
		}
		stop = brk;
		if (c && y + line_h > c->clip.y && y < c->clip.y + c->clip.h) {
			w = bui_text_width(f, s, (int)(stop - s));
			if (align & BUI_AL_CENTER)
				x = r.x + (r.w - w) / 2;
			else if (align & BUI_AL_RIGHT)
				x = r.x + r.w - w;
			else
				x = r.x;
			text_run(c, f, x, y + (line_h + f->cap_h) / 2, s, (int)(stop - s), col);
		}
		y += line_h;
		s = stop;
		if (s < end && (*s == ' ' || *s == '\n'))
			s++;
	}
	return y - r.y;
}

/* ------------------------------------------------------------------ */
/* icone                                                              */
/* ------------------------------------------------------------------ */

void bui_icon_draw(bui_canvas *c, const bui_icon *ic, int x, int y, bui_px col)
{
	if (ic)
		mask_blit(c, x + ic->ox, y + ic->oy, ic->a8, ic->w, ic->h, ic->w, col, 255, 0, 0);
}

void bui_icon_in(bui_canvas *c, const bui_icon *ic, bui_rect r, bui_px col)
{
	if (ic)
		bui_icon_draw(c, ic, r.x + (r.w - ic->size) / 2, r.y + (r.h - ic->size) / 2, col);
}
