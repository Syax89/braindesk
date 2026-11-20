// SPDX-License-Identifier: MIT
/*
 * bui_fallback.c - glifi che i font precotti non hanno, disegnati al volo.
 *
 * I font della shell (gen/bui_fonts.c) contengono solo i caratteri che
 * servono all'interfaccia. Il resto (kanji di un dizionario, nomi di file
 * cinesi, simboli rari) arriva da un font TrueType installato sul sistema,
 * per default IPAex Gothic copiato in /usr/local/share/brain/fonts/
 * (BUI_FALLBACK_FONT per un altro). Il file e' mappato in memoria solo al
 * primo carattere mancante; ogni glifo si disegna una volta per dimensione
 * e resta in una piccola cache (svuotata quando si riempie).
 */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "tools/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define DEFAULT_FONT "/usr/local/share/brain/fonts/fallback.ttf"
#define CACHE 1024		/* potenza di 2 */

struct ent {
	const bui_font *f;
	unsigned cp;
	bui_glyph g;
	uint8_t *bits;
};

static struct {
	int state;		/* 0 da provare, 1 pronto, -1 assente */
	stbtt_fontinfo info;
	struct ent e[CACHE];
	int used;
} F;

static int open_font(void)
{
	const char *path = getenv("BUI_FALLBACK_FONT");
	struct stat st;
	void *p;
	int fd;

	if (F.state)
		return F.state > 0;
	F.state = -1;
	fd = open(path && *path ? path : DEFAULT_FONT, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) < 0 || st.st_size < 1024) {
		close(fd);
		return 0;
	}
	p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return 0;
	if (!stbtt_InitFont(&F.info, p, stbtt_GetFontOffsetForIndex(p, 0))) {
		munmap(p, (size_t)st.st_size);
		return 0;
	}
	F.state = 1;
	return 1;
}

static void flush(void)
{
	int i;

	for (i = 0; i < CACHE; i++) {
		free(F.e[i].bits);
		F.e[i].f = NULL;
		F.e[i].bits = NULL;
	}
	F.used = 0;
}

static int is_bold(const bui_font *f)
{
	return f == &bui_font_bold15 || f == &bui_font_bold18 || f == &bui_font_bold22;
}

const bui_glyph *bui_fallback_glyph(const bui_font *f, unsigned cp, const uint8_t **bits)
{
	unsigned h = ((unsigned)(uintptr_t)f * 2654435761u ^ cp * 40503u) & (CACHE - 1);
	struct ent *e;
	float sc;
	int g, a, lsb, x0, y0, x1, y1, w, hh, bold, i;
	uint8_t *bm;

	if (cp < 0x80 || !open_font())
		return NULL;
	for (;;) {
		e = &F.e[h];
		if (!e->f)
			break;
		if (e->f == f && e->cp == cp) {
			*bits = e->bits;
			return e->g.adv ? &e->g : NULL;
		}
		h = (h + 1) & (CACHE - 1);
	}
	if (F.used >= CACHE * 3 / 4) {
		flush();
		return bui_fallback_glyph(f, cp, bits);
	}
	/* anche "non c'e'" va in cache: adv = 0 */
	e->f = f;
	e->cp = cp;
	e->bits = NULL;
	memset(&e->g, 0, sizeof(e->g));
	F.used++;
	g = stbtt_FindGlyphIndex(&F.info, (int)cp);
	if (!g)
		return NULL;
	bold = is_bold(f);
	sc = stbtt_ScaleForMappingEmToPixels(&F.info, (float)f->size);
	stbtt_GetGlyphHMetrics(&F.info, g, &a, &lsb);
	stbtt_GetGlyphBitmapBox(&F.info, g, sc, sc, &x0, &y0, &x1, &y1);
	w = x1 - x0 + (bold ? 1 : 0);
	hh = y1 - y0;
	e->g.cp = (uint16_t)(cp > 0xFFFF ? 0xFFFF : cp);
	e->g.adv = (uint16_t)(floorf(a * sc * 64.0f + 0.5f) + (bold ? 64 : 0));
	if (!e->g.adv)
		e->g.adv = 1;
	if (w <= 0 || hh <= 0 || w > 255 || hh > 255) {
		*bits = NULL;
		return &e->g;	/* spazio o glifo vuoto: solo avanzamento */
	}
	bm = calloc(1, (size_t)(w * hh));
	if (!bm)
		return NULL;
	stbtt_MakeGlyphBitmap(&F.info, bm, w - (bold ? 1 : 0), hh, w, sc, sc, g);
	for (i = 0; i < w * hh; i++) {
		/* come i font precotti: il testo chiaro su fondo scuro un po' piu' pieno */
		bm[i] = (uint8_t)(powf(bm[i] / 255.0f, 0.85f) * 255.0f + 0.5f);
	}
	if (bold) {
		int yy, xx;

		for (yy = 0; yy < hh; yy++)
			for (xx = w - 1; xx > 0; xx--)
				if (bm[yy * w + xx - 1] > bm[yy * w + xx])
					bm[yy * w + xx] = bm[yy * w + xx - 1];
	}
	e->g.w = (uint8_t)w;
	e->g.h = (uint8_t)hh;
	e->g.bx = (int8_t)x0;
	e->g.by = (int8_t)(-y0);
	e->bits = bm;
	*bits = bm;
	return &e->g;
}
