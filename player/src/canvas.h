/* SPDX-License-Identifier: MIT */
/*
 * canvas: software drawing on an RGB565 buffer (control bar, audio view).
 * Integer only. Text and icons are 4-bit alpha masks baked by
 * tools/genassets.py into assets.c.
 */
#ifndef BRAINPLAY_CANVAS_H
#define BRAINPLAY_CANVAS_H

#include <stdint.h>

#define RGB565(r, g, b) \
	((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xFF) >> 3)))
#define HEX565(h) RGB565(((h) >> 16) & 0xFF, ((h) >> 8) & 0xFF, (h) & 0xFF)

struct rect {
	int x, y, w, h;
};

static inline int rect_has(struct rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

struct glyph {
	uint16_t cp;		/* Unicode code point */
	uint8_t w, h;		/* bitmap size */
	int8_t xoff, yoff;	/* bitmap origin relative to pen / baseline */
	uint8_t adv;		/* pen advance */
	uint32_t off;		/* offset in the font data */
};

struct font {
	int ascent, descent;
	int n;
	const struct glyph *glyphs;	/* sorted by code point */
	const uint8_t *data;
};

struct icon {
	int w, h;
	const uint8_t *data;
};

extern const struct font font_small, font_text, font_title, font_big;
extern const struct icon icon_play, icon_pause, icon_voldown, icon_volup,
			 icon_close, icon_knob, icon_note;

struct canvas {
	uint16_t *px;
	int w, h;
	int stride;		/* pixels per row */
};

void cv_fill(struct canvas *c, int x, int y, int w, int h, uint16_t col);
/* 4-bit alpha mask (rows padded to a byte, high nibble first) in colour col */
void cv_mask(struct canvas *c, int x, int y, const uint8_t *a4, int w, int h, uint16_t col);
void cv_icon(struct canvas *c, const struct icon *ic, int cx, int cy, uint16_t col);
/*
 * UTF-8 text with the pen at (x, baseline). Text wider than max_w (if > 0)
 * is cut and ends with an ellipsis. Returns the width drawn.
 */
int cv_text(struct canvas *c, const struct font *f, int x, int baseline,
	    const char *utf8, uint16_t col, int max_w);
int cv_text_width(const struct font *f, const char *utf8);

#endif
