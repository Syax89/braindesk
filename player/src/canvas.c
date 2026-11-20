// SPDX-License-Identifier: MIT
/* canvas: clipped fills, alpha masks and text on an RGB565 buffer. */
#include "canvas.h"

#include <stddef.h>

void cv_fill(struct canvas *c, int x, int y, int w, int h, uint16_t col)
{
	int i, j;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > c->w)
		w = c->w - x;
	if (y + h > c->h)
		h = c->h - y;
	if (w <= 0 || h <= 0)
		return;
	for (j = 0; j < h; j++) {
		uint16_t *p = c->px + (size_t)(y + j) * c->stride + x;

		for (i = 0; i < w; i++)
			p[i] = col;
	}
}

/* a in 0..16 */
static inline uint16_t blend565(uint16_t d, uint16_t s, unsigned a)
{
	unsigned rb = (((d & 0xF81Fu) * (16 - a) + (s & 0xF81Fu) * a) >> 4) & 0xF81Fu;
	unsigned g = (((d & 0x07E0u) * (16 - a) + (s & 0x07E0u) * a) >> 4) & 0x07E0u;

	return (uint16_t)(rb | g);
}

void cv_mask(struct canvas *c, int x, int y, const uint8_t *a4, int w, int h, uint16_t col)
{
	int rowbytes = (w + 1) / 2;
	int i, j;

	for (j = 0; j < h; j++) {
		const uint8_t *row = a4 + (size_t)j * rowbytes;
		uint16_t *p;

		if (y + j < 0 || y + j >= c->h)
			continue;
		p = c->px + (size_t)(y + j) * c->stride;
		for (i = 0; i < w; i++) {
			unsigned a = (i & 1) ? (row[i >> 1] & 15) : (row[i >> 1] >> 4);
			int px = x + i;

			if (!a || px < 0 || px >= c->w)
				continue;
			if (a == 15)
				p[px] = col;
			else
				p[px] = blend565(p[px], col, a + (a >> 3));
		}
	}
}

void cv_icon(struct canvas *c, const struct icon *ic, int cx, int cy, uint16_t col)
{
	cv_mask(c, cx - ic->w / 2, cy - ic->h / 2, ic->data, ic->w, ic->h, col);
}

/* next code point of a UTF-8 string; malformed bytes give U+FFFD */
static unsigned utf8_next(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	unsigned c = *p++, n = 0, min = 0;

	if (c < 0x80) {
		*s = (const char *)p;
		return c;
	} else if ((c & 0xE0) == 0xC0) {
		c &= 0x1F;
		n = 1;
		min = 0x80;
	} else if ((c & 0xF0) == 0xE0) {
		c &= 0x0F;
		n = 2;
		min = 0x800;
	} else if ((c & 0xF8) == 0xF0) {
		c &= 0x07;
		n = 3;
		min = 0x10000;
	} else {
		*s = (const char *)p;
		return 0xFFFD;
	}
	while (n--) {
		if ((*p & 0xC0) != 0x80) {
			*s = (const char *)p;
			return 0xFFFD;
		}
		c = (c << 6) | (*p++ & 0x3F);
	}
	*s = (const char *)p;
	return c < min ? 0xFFFD : c;
}

static const struct glyph *find_glyph(const struct font *f, unsigned cp)
{
	int lo = 0, hi = f->n - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;

		if (f->glyphs[mid].cp == cp)
			return &f->glyphs[mid];
		if (f->glyphs[mid].cp < cp)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return NULL;
}

static const struct glyph *get_glyph(const struct font *f, unsigned cp)
{
	const struct glyph *g = find_glyph(f, cp);

	if (!g)
		g = find_glyph(f, 0x25A1);	/* white square */
	if (!g)
		g = find_glyph(f, '-');
	return g;
}

int cv_text_width(const struct font *f, const char *utf8)
{
	int w = 0;

	while (*utf8) {
		const struct glyph *g = get_glyph(f, utf8_next(&utf8));

		if (g)
			w += g->adv;
	}
	return w;
}

int cv_text(struct canvas *c, const struct font *f, int x, int baseline,
	    const char *utf8, uint16_t col, int max_w)
{
	const struct glyph *dots = find_glyph(f, 0x2026);
	int cut = max_w > 0 && cv_text_width(f, utf8) > max_w;
	int limit = cut ? max_w - (dots ? dots->adv : 0) : 0;
	int pen = 0;

	while (*utf8) {
		const struct glyph *g = get_glyph(f, utf8_next(&utf8));

		if (!g)
			continue;
		if (cut && pen + g->adv > limit) {
			g = dots;
			if (g && c)
				cv_mask(c, x + pen + g->xoff, baseline + g->yoff,
					f->data + g->off, g->w, g->h, col);
			pen += g ? g->adv : 0;
			break;
		}
		if (c && g->w)
			cv_mask(c, x + pen + g->xoff, baseline + g->yoff,
				f->data + g->off, g->w, g->h, col);
		pen += g->adv;
	}
	return pen;
}
