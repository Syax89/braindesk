// SPDX-License-Identifier: MIT
/*
 * bs_img.c - decodifica e riduzione delle immagini (vedi bs_img.h).
 *
 * Due decodificatori incorporati (third_party/):
 *  - TJpgDec per i JPEG "baseline" (quasi tutte le foto): lavora a blocchi
 *    con pochi kB di memoria e sa uscire gia' ridotto a 1/2, 1/4, 1/8 (a 1/8
 *    usa solo il coefficiente DC di ogni blocco, senza IDCT). Una foto da
 *    12 MP non viene mai tenuta in memoria per intero: le righe passano
 *    direttamente nel filtro di riduzione.
 *  - stb_image per PNG, BMP, GIF e per i JPEG che TJpgDec non legge
 *    (progressivi, campionamenti insoliti): decodifica tutta l'immagine in
 *    memoria, quindi solo fino a BS_IMG_MAX_PIXELS_FULL.
 *
 * La riduzione e' una media d'area esatta in aritmetica intera (ogni pixel
 * di origine pesa per la parte che cade nel pixel di destinazione), una riga
 * alla volta; l'uscita e' RGB565 con dithering ordinato (bui_pack_dither).
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bs_img.h"

/*
 * I decodificatori incorporati fanno conti su dati che arrivano dal file: con
 * un file rovinato una somma puo' uscire dai 32 bit. Qui il traboccamento
 * con segno e' definito (complemento a due), come con -fwrapv: il risultato
 * e' un pixel sbagliato in un'immagine gia' rovinata, mai un comportamento
 * indefinito.
 */
#pragma GCC optimize("wrapv")

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 32768
#include "third_party/stb_image.h"
#include "third_party/tjpgd.c"
#pragma GCC diagnostic pop

/* fondo sotto le parti trasparenti: la superficie del tema corrente */
#define BG_R ((int)(BUI_C_SURFACE >> 11) << 3)
#define BG_G ((int)((BUI_C_SURFACE >> 5) & 63) << 2)
#define BG_B ((int)(BUI_C_SURFACE & 31) << 3)

int bs_img_is_image(const char *name)
{
	const char *e = strrchr(name, '.');

	if (!e || e == name)
		return 0;
	e++;
	return !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "png") ||
	       !strcasecmp(e, "bmp") || !strcasecmp(e, "gif");
}

const char *bs_img_strerror(int err)
{
	switch (err) {
	case BS_IMG_OK:
		return "";
	case BS_IMG_E_OPEN:
		return "Impossibile leggere il file";
	case BS_IMG_E_TOOBIG:
		return "Immagine troppo grande per questo dispositivo";
	case BS_IMG_E_MEM:
		return "Memoria insufficiente";
	default:
		return "Immagine non valida o formato non supportato";
	}
}

void bs_img_free(bs_img *im)
{
	free(im->px);
	im->px = NULL;
}

/* ------------------------------------------------------------------ */
/* riduzione a media d'area, una riga alla volta                      */
/* ------------------------------------------------------------------ */

struct scaler {
	int sw, sh, dw, dh;
	bui_px *dst;
	uint32_t *hrow;		/* riga ridotta in orizzontale, canali 8.4 */
	uint32_t *vacc;		/* somma pesata delle righe che cadono in quella d'uscita */
	uint32_t mh;		/* 16 * 2^24 / sw */
	uint64_t mv;		/* 256 * 2^24 / sh */
	int vcap;		/* quanto manca a completare la riga d'uscita (su sh) */
	int oy;			/* prossima riga d'uscita */
};

static int scaler_init(struct scaler *s, int sw, int sh, int dw, int dh)
{
	memset(s, 0, sizeof(*s));
	s->sw = sw;
	s->sh = sh;
	s->dw = dw;
	s->dh = dh;
	s->dst = malloc(sizeof(bui_px) * (size_t)dw * dh);
	s->hrow = malloc(sizeof(uint32_t) * 3 * (size_t)dw);
	s->vacc = calloc(3 * (size_t)dw, sizeof(uint32_t));
	if (!s->dst || !s->hrow || !s->vacc)
		return -1;
	s->mh = (uint32_t)(((16u << 24) + (uint32_t)sw - 1) / (uint32_t)sw);
	s->mv = (((uint64_t)256 << 24) + (uint64_t)sh - 1) / (uint64_t)sh;
	s->vcap = sh;
	return 0;
}

static void scaler_free(struct scaler *s, int keep_dst)
{
	free(s->hrow);
	free(s->vacc);
	if (!keep_dst)
		free(s->dst);
	s->hrow = s->vacc = NULL;
}

static void scaler_emit(struct scaler *s)
{
	bui_px *d = s->dst + (size_t)s->oy * s->dw;
	const uint32_t *v = s->vacc;
	int x;

	for (x = 0; x < s->dw; x++, v += 3)
		d[x] = bui_pack_dither((int)(((uint64_t)v[0] * s->mv) >> 24),
				       (int)(((uint64_t)v[1] * s->mv) >> 24),
				       (int)(((uint64_t)v[2] * s->mv) >> 24), x, s->oy);
	s->oy++;
}

/* una riga di origine: sw pixel RGB888 */
static void scaler_row(struct scaler *s, const uint8_t *p)
{
	uint32_t *h = s->hrow;
	int n = s->dw * 3, i, a;

	if (s->oy >= s->dh)
		return;
	if (s->sw == s->dw) {
		for (i = 0; i < n; i++)
			h[i] = (uint32_t)p[i] << 4;
	} else {
		uint32_t r = 0, g = 0, b = 0;
		int cap = s->sw, x;

		for (x = 0; x < s->sw; x++, p += 3) {
			a = s->dw;
			while (a >= cap) {
				h[0] = (uint32_t)(((uint64_t)(r + p[0] * (uint32_t)cap) * s->mh) >> 24);
				h[1] = (uint32_t)(((uint64_t)(g + p[1] * (uint32_t)cap) * s->mh) >> 24);
				h[2] = (uint32_t)(((uint64_t)(b + p[2] * (uint32_t)cap) * s->mh) >> 24);
				h += 3;
				r = g = b = 0;
				a -= cap;
				cap = s->sw;
			}
			r += p[0] * (uint32_t)a;
			g += p[1] * (uint32_t)a;
			b += p[2] * (uint32_t)a;
			cap -= a;
		}
		h = s->hrow;
	}
	a = s->dh;
	while (a >= s->vcap && s->oy < s->dh) {
		uint32_t c = (uint32_t)s->vcap;

		for (i = 0; i < n; i++)
			s->vacc[i] += h[i] * c;
		scaler_emit(s);
		memset(s->vacc, 0, sizeof(uint32_t) * (size_t)n);
		a -= s->vcap;
		s->vcap = s->sh;
	}
	if (a > 0 && s->oy < s->dh) {
		for (i = 0; i < n; i++)
			s->vacc[i] += h[i] * (uint32_t)a;
		s->vcap -= a;
	}
}

/* file troncato: le righe mancanti restano grigie. Ritorna le righe valide */
static int scaler_finish(struct scaler *s)
{
	int got = s->oy, i;

	for (i = got * s->dw; i < s->dw * s->dh; i++)
		s->dst[i] = BUI_RGB(BG_R, BG_G, BG_B);
	s->oy = s->dh;
	return got;
}

/* ------------------------------------------------------------------ */
/* geometria: riquadro, ritaglio, orientamento EXIF                   */
/* ------------------------------------------------------------------ */

struct geom {
	int cx, cy, cw, ch;	/* ritaglio dell'origine */
	int dw, dh;		/* dimensione d'uscita (prima della rotazione) */
};

static void geom_calc(struct geom *g, int w, int h, int bw, int bh, int mode)
{
	g->cx = g->cy = 0;
	g->cw = w;
	g->ch = h;
	if (mode == BS_IMG_COVER) {
		if ((long long)w * bh > (long long)h * bw) {
			g->cw = (int)((long long)h * bw / bh);
			if (g->cw < 1)
				g->cw = 1;
			g->cx = (w - g->cw) / 2;
		} else {
			g->ch = (int)((long long)w * bh / bw);
			if (g->ch < 1)
				g->ch = 1;
			g->cy = (h - g->ch) / 2;
		}
		g->dw = g->cw < bw ? g->cw : bw;
		g->dh = g->ch < bh ? g->ch : bh;
		return;
	}
	if ((long long)w * bh > (long long)h * bw) {
		g->dw = w < bw ? w : bw;
		g->dh = (int)((long long)h * g->dw / w);
	} else {
		g->dh = h < bh ? h : bh;
		g->dw = (int)((long long)w * g->dh / h);
	}
	if (g->dw < 1)
		g->dw = 1;
	if (g->dh < 1)
		g->dh = 1;
}

/* applica l'orientamento EXIF (1..8) a un'immagine RGB565; ritorna 0 se riuscito */
static int orient_apply(bs_img *im, int o)
{
	int w = im->w, h = im->h, nw = o >= 5 ? h : w, nh = o >= 5 ? w : h, x, y;
	bui_px *d;

	if (o <= 1 || o > 8)
		return 0;
	d = malloc(sizeof(bui_px) * (size_t)nw * nh);
	if (!d)
		return -1;
	for (y = 0; y < nh; y++) {
		bui_px *row = d + (size_t)y * nw;

		for (x = 0; x < nw; x++) {
			int sx, sy;

			switch (o) {
			case 2: sx = w - 1 - x; sy = y; break;
			case 3: sx = w - 1 - x; sy = h - 1 - y; break;
			case 4: sx = x; sy = h - 1 - y; break;
			case 5: sx = y; sy = x; break;
			case 6: sx = y; sy = h - 1 - x; break;
			case 7: sx = w - 1 - y; sy = h - 1 - x; break;
			default: sx = w - 1 - y; sy = x; break;
			}
			row[x] = im->px[(size_t)sy * w + sx];
		}
	}
	free(im->px);
	im->px = d;
	im->w = nw;
	im->h = nh;
	return 0;
}

/* ------------------------------------------------------------------ */
/* intestazione JPEG: tipo di codifica, dimensioni, orientamento EXIF */
/* ------------------------------------------------------------------ */

struct jinfo {
	int w, h, sof;		/* sof: 0xC0.. (0 = non trovato) */
	int orient;
};

static unsigned rd16(const uint8_t *p, int le)
{
	return le ? (unsigned)(p[0] | (p[1] << 8)) : (unsigned)((p[0] << 8) | p[1]);
}

static unsigned long rd32(const uint8_t *p, int le)
{
	return le ? (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
			    ((unsigned long)p[3] << 24)
		  : (unsigned long)p[3] | ((unsigned long)p[2] << 8) | ((unsigned long)p[1] << 16) |
			    ((unsigned long)p[0] << 24);
}

static int exif_orientation(const uint8_t *b, size_t n)
{
	unsigned long off;
	unsigned cnt, i;
	int le;

	if (n < 14 || memcmp(b, "Exif\0\0", 6))
		return 1;
	b += 6;
	n -= 6;
	if (b[0] == 'I' && b[1] == 'I')
		le = 1;
	else if (b[0] == 'M' && b[1] == 'M')
		le = 0;
	else
		return 1;
	if (rd16(b + 2, le) != 42)
		return 1;
	off = rd32(b + 4, le);
	if (off > n || n - off < 2)
		return 1;
	cnt = rd16(b + off, le);
	for (i = 0; i < cnt; i++) {
		size_t e = off + 2 + (size_t)i * 12;

		if (e + 12 > n)
			break;
		if (rd16(b + e, le) == 0x0112) {
			unsigned v = rd16(b + e + 8, le);

			return v >= 1 && v <= 8 ? (int)v : 1;
		}
	}
	return 1;
}

static void jpeg_scan(FILE *f, struct jinfo *ji)
{
	uint8_t hd[8], *buf;
	int m, guard = 0;
	unsigned len;

	memset(ji, 0, sizeof(*ji));
	ji->orient = 1;
	if (fseek(f, 2, SEEK_SET))
		return;
	while (guard++ < 200) {
		if (fread(hd, 1, 2, f) != 2 || hd[0] != 0xFF)
			return;
		m = hd[1];
		while (m == 0xFF) {
			int c = fgetc(f);

			if (c == EOF)
				return;
			m = c;
		}
		if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01)
			continue;
		if (m == 0xD9 || m == 0xDA)
			return;
		if (fread(hd, 1, 2, f) != 2)
			return;
		len = (unsigned)((hd[0] << 8) | hd[1]);
		if (len < 2)
			return;
		len -= 2;
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			if (len < 6 || fread(hd, 1, 6, f) != 6)
				return;
			ji->sof = m;
			ji->h = (hd[1] << 8) | hd[2];
			ji->w = (hd[3] << 8) | hd[4];
			return;
		}
		if (m == 0xE1 && len >= 14) {
			buf = malloc(len);
			if (!buf)
				return;
			if (fread(buf, 1, len, f) != len) {
				free(buf);
				return;
			}
			if (!memcmp(buf, "Exif\0\0", 6))
				ji->orient = exif_orientation(buf, len);
			free(buf);
			continue;
		}
		if (fseek(f, (long)len, SEEK_CUR))
			return;
	}
}

/* ------------------------------------------------------------------ */
/* JPEG baseline con TJpgDec                                          */
/* ------------------------------------------------------------------ */

struct jctx {
	FILE *f;
	const volatile int *cancel;
	struct scaler *sc;
	uint8_t *strip;		/* una riga di MCU gia' ridotta: sw x mcu_h pixel RGB */
	int sw, mcu_h;
	int top, rows;		/* riga di partenza della striscia e righe valide */
	int cx, cy, cw, ch;	/* ritaglio, nello spazio ridotto */
	int done;		/* tutte le righe utili sono passate */
};

static size_t jd_in(JDEC *jd, uint8_t *buf, size_t len)
{
	struct jctx *j = jd->device;

	if (j->cancel && *j->cancel)
		return 0;
	if (!buf)
		return fseek(j->f, (long)len, SEEK_CUR) ? 0 : len;
	return fread(buf, 1, len, j->f);
}

static void strip_flush(struct jctx *j)
{
	int y;

	for (y = 0; y < j->rows; y++) {
		int iy = j->top + y;

		if (iy >= j->cy && iy < j->cy + j->ch)
			scaler_row(j->sc, j->strip + ((size_t)y * j->sw + j->cx) * 3);
	}
	if (j->rows && j->top + j->rows >= j->cy + j->ch)
		j->done = 1;
	j->rows = 0;
}

static int jd_out(JDEC *jd, void *bitmap, JRECT *r)
{
	struct jctx *j = jd->device;
	const uint8_t *src = bitmap;
	int w = r->right - r->left + 1, y;

	if (j->cancel && *j->cancel)
		return 0;
	if (r->top != j->top) {
		strip_flush(j);
		if (j->done)
			return 0;	/* sotto il ritaglio non serve altro */
		j->top = r->top;
	}
	if (r->right >= j->sw || r->bottom - r->top >= j->mcu_h || w <= 0)
		return 1;
	for (y = r->top; y <= r->bottom; y++, src += w * 3)
		memcpy(j->strip + ((size_t)(y - j->top) * j->sw + r->left) * 3, src, (size_t)w * 3);
	if (r->bottom - r->top + 1 > j->rows)
		j->rows = r->bottom - r->top + 1;
	return 1;
}

/* ritorna BS_IMG_*, oppure -1 se il file va passato a stb_image */
static int load_tjpgd(FILE *f, int bw, int bh, int mode, const volatile int *cancel, bs_img *out)
{
	enum { POOL = 32 * 1024 };
	struct scaler sc;
	struct jctx j;
	struct geom g;
	JDEC jd;
	JRESULT rc;
	void *pool;
	int s, err = BS_IMG_E_FORMAT, w, h;

	memset(&j, 0, sizeof(j));
	memset(&sc, 0, sizeof(sc));
	j.f = f;
	j.cancel = cancel;
	pool = malloc(POOL);
	if (!pool)
		return BS_IMG_E_MEM;
	rewind(f);
	rc = jd_prepare(&jd, jd_in, pool, POOL, &j);
	if (rc != JDR_OK) {
		free(pool);
		if (cancel && *cancel)
			return BS_IMG_E_CANCEL;
		return rc == JDR_FMT3 || rc == JDR_FMT2 || rc == JDR_MEM1 ? -1 : BS_IMG_E_FORMAT;
	}
	w = jd.width;
	h = jd.height;
	if (w < 1 || h < 1) {
		free(pool);
		return BS_IMG_E_FORMAT;
	}
	if ((long long)w * h > BS_IMG_MAX_PIXELS) {
		free(pool);
		return BS_IMG_E_TOOBIG;
	}
	geom_calc(&g, w, h, bw, bh, mode);
	for (s = 3; s > 0; s--)
		if ((g.cw >> s) >= g.dw && (g.ch >> s) >= g.dh)
			break;
	j.sw = w >> s;
	j.mcu_h = (jd.msy * 8) >> s;
	j.cx = g.cx >> s;
	j.cy = g.cy >> s;
	j.cw = g.cw >> s;
	j.ch = g.ch >> s;
	j.top = -1;
	j.sc = &sc;
	j.strip = malloc((size_t)j.sw * j.mcu_h * 3);
	if (!j.strip || scaler_init(&sc, j.cw, j.ch, g.dw, g.dh) < 0) {
		err = BS_IMG_E_MEM;
		goto fail;
	}
	rc = jd_decomp(&jd, jd_out, (uint8_t)s);
	if (cancel && *cancel) {
		err = BS_IMG_E_CANCEL;
		goto fail;
	}
	if (!j.done)
		strip_flush(&j);
	/* un file troncato mostra comunque la parte arrivata */
	if (scaler_finish(&sc) < 1 || (rc != JDR_OK && rc != JDR_INTR && rc != JDR_INP && rc != JDR_FMT1))
		goto fail;
	out->px = sc.dst;
	out->w = g.dw;
	out->h = g.dh;
	out->src_w = w;
	out->src_h = h;
	scaler_free(&sc, 1);
	free(j.strip);
	free(pool);
	return BS_IMG_OK;
fail:
	scaler_free(&sc, 0);
	free(j.strip);
	free(pool);
	return err;
}

/* ------------------------------------------------------------------ */
/* tutto il resto con stb_image                                       */
/* ------------------------------------------------------------------ */

struct sctx {
	FILE *f;
	const volatile int *cancel;
};

static int st_read(void *user, char *data, int size)
{
	struct sctx *s = user;

	if (s->cancel && *s->cancel)
		return 0;
	return (int)fread(data, 1, (size_t)size, s->f);
}

static void st_skip(void *user, int n)
{
	struct sctx *s = user;

	if (fseek(s->f, n, SEEK_CUR)) {
		/* fine del file: se ne accorge st_eof */
	}
}

static int st_eof(void *user)
{
	struct sctx *s = user;
	int c;

	if (s->cancel && *s->cancel)
		return 1;
	c = fgetc(s->f);
	if (c == EOF)
		return 1;
	ungetc(c, s->f);
	return 0;
}

static const stbi_io_callbacks st_cb = { st_read, st_skip, st_eof };

static int load_stb(FILE *f, int is_gif, int bw, int bh, int mode, const volatile int *cancel, bs_img *out)
{
	struct sctx s = { f, cancel };
	struct scaler sc;
	struct geom g;
	uint8_t *pix, *tmp = NULL;
	int w, h, comp, req, y, err = BS_IMG_OK;

	rewind(f);
	if (!stbi_info_from_callbacks(&st_cb, &s, &w, &h, &comp) || w < 1 || h < 1)
		return cancel && *cancel ? BS_IMG_E_CANCEL : BS_IMG_E_FORMAT;
	if ((long long)w * h > (is_gif ? BS_IMG_MAX_PIXELS_GIF : BS_IMG_MAX_PIXELS_FULL))
		return BS_IMG_E_TOOBIG;
	req = (comp == 2 || comp == 4) ? 4 : 3;
	rewind(f);
	pix = stbi_load_from_callbacks(&st_cb, &s, &w, &h, &comp, req);
	if (!pix)
		return cancel && *cancel ? BS_IMG_E_CANCEL : BS_IMG_E_FORMAT;
	geom_calc(&g, w, h, bw, bh, mode);
	memset(&sc, 0, sizeof(sc));
	if (scaler_init(&sc, g.cw, g.ch, g.dw, g.dh) < 0 || (req == 4 && !(tmp = malloc((size_t)g.cw * 3)))) {
		err = BS_IMG_E_MEM;
		goto out;
	}
	for (y = 0; y < g.ch; y++) {
		const uint8_t *row = pix + ((size_t)(g.cy + y) * w + g.cx) * (size_t)req;

		if (cancel && *cancel) {
			err = BS_IMG_E_CANCEL;
			goto out;
		}
		if (req == 4) {
			int x;

			/* le parti trasparenti lasciano vedere un fondo scuro */
			for (x = 0; x < g.cw; x++) {
				int a = row[x * 4 + 3], na = 255 - a;

				tmp[x * 3] = (uint8_t)((row[x * 4] * a + BG_R * na + 127) / 255);
				tmp[x * 3 + 1] = (uint8_t)((row[x * 4 + 1] * a + BG_G * na + 127) / 255);
				tmp[x * 3 + 2] = (uint8_t)((row[x * 4 + 2] * a + BG_B * na + 127) / 255);
			}
			row = tmp;
		}
		scaler_row(&sc, row);
	}
	scaler_finish(&sc);
	out->px = sc.dst;
	out->w = g.dw;
	out->h = g.dh;
	out->src_w = w;
	out->src_h = h;
out:
	scaler_free(&sc, err == BS_IMG_OK);
	free(tmp);
	stbi_image_free(pix);
	return err;
}

int bs_img_load(const char *path, int box_w, int box_h, int mode, const volatile int *cancel, bs_img *out)
{
	struct jinfo ji = { 0, 0, 0, 1 };
	uint8_t magic[4] = { 0 };
	FILE *f;
	int rc = -1, rot;

	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return BS_IMG_E_FORMAT;
	f = fopen(path, "rb");
	if (!f)
		return BS_IMG_E_OPEN;
	if (fread(magic, 1, 4, f) < 4) {
		fclose(f);
		return BS_IMG_E_FORMAT;
	}
	if (magic[0] == 0xFF && magic[1] == 0xD8) {
		jpeg_scan(f, &ji);
		if ((long long)ji.w * ji.h > BS_IMG_MAX_PIXELS) {
			fclose(f);
			return BS_IMG_E_TOOBIG;
		}
	}
	/* il riquadro e' dato per l'immagine gia' raddrizzata */
	rot = ji.orient >= 5;
	if (ji.sof == 0xC0 || ji.sof == 0xC1)
		rc = load_tjpgd(f, rot ? box_h : box_w, rot ? box_w : box_h, mode, cancel, out);
	if (rc < 0)
		rc = load_stb(f, magic[0] == 'G', rot ? box_h : box_w, rot ? box_w : box_h, mode, cancel,
			      out);
	fclose(f);
	if (rc == BS_IMG_OK && ji.orient > 1) {
		if (orient_apply(out, ji.orient) < 0) {
			bs_img_free(out);
			return BS_IMG_E_MEM;
		}
		if (rot) {
			int t = out->src_w;

			out->src_w = out->src_h;
			out->src_h = t;
		}
	}
	if (rc == BS_IMG_OK && cancel && *cancel) {
		bs_img_free(out);
		rc = BS_IMG_E_CANCEL;
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* miniature con cache su disco                                       */
/* ------------------------------------------------------------------ */

/*
 * ~/.cache/brainshell/thumbs/<16 cifre esadecimali>.t565
 * Il nome e' l'FNV-1a a 64 bit di "percorso\0mtime\0dimensione": un file
 * modificato ha un altro nome, e quello vecchio viene tolto dalla pulizia.
 * Contenuto: "BTH1", larghezza e altezza (16 bit LE), poi i pixel RGB565.
 */
unsigned long long bs_img_thumb_key(const char *path, long long mtime, long long size)
{
	unsigned long long h = 0xCBF29CE484222325ULL;
	char tail[48];
	const char *p;
	int n, i;

	for (p = path; *p; p++)
		h = (h ^ (unsigned char)*p) * 0x100000001B3ULL;
	n = snprintf(tail, sizeof(tail), "%c%lld%c%lld", 0, mtime, 0, size);
	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)tail[i]) * 0x100000001B3ULL;
	return h;
}

static int thumb_dir(char *buf, size_t len, int create)
{
	const char *home = getenv("HOME");
	int n;

	if (!home || !*home)
		return -1;
	n = snprintf(buf, len, "%s/.cache", home);
	if (n < 0 || (size_t)n >= len - 40)
		return -1;
	if (create)
		mkdir(buf, 0700);
	strcat(buf, "/brainshell");
	if (create)
		mkdir(buf, 0700);
	strcat(buf, "/thumbs");
	if (create)
		mkdir(buf, 0700);
	return 0;
}

static int thumb_read(const char *file, bs_img *out)
{
	uint8_t hd[8];
	FILE *f = fopen(file, "rb");
	int w, h;

	if (!f)
		return -1;
	if (fread(hd, 1, 8, f) != 8 || memcmp(hd, "BTH1", 4))
		goto bad;
	w = hd[4] | (hd[5] << 8);
	h = hd[6] | (hd[7] << 8);
	if (w < 1 || h < 1 || w > BS_THUMB || h > BS_THUMB)
		goto bad;
	out->px = malloc(sizeof(bui_px) * (size_t)w * h);
	if (!out->px)
		goto bad;
	if (fread(out->px, sizeof(bui_px), (size_t)w * h, f) != (size_t)w * h) {
		bs_img_free(out);
		goto bad;
	}
	out->w = w;
	out->h = h;
	fclose(f);
	return 0;
bad:
	fclose(f);
	return -1;
}

static void thumb_write(const char *file, const bs_img *im)
{
	uint8_t hd[8] = { 'B', 'T', 'H', '1', (uint8_t)im->w, (uint8_t)(im->w >> 8), (uint8_t)im->h,
			  (uint8_t)(im->h >> 8) };
	char tmp[600];
	FILE *f;
	int ok;

	snprintf(tmp, sizeof(tmp), "%s.%d.tmp", file, (int)getpid());
	f = fopen(tmp, "wb");
	if (!f)
		return;
	ok = fwrite(hd, 1, 8, f) == 8 &&
	     fwrite(im->px, sizeof(bui_px), (size_t)im->w * im->h, f) == (size_t)im->w * im->h;
	if (fclose(f) || !ok || rename(tmp, file))
		unlink(tmp);
}

int bs_img_thumb(const char *path, long long mtime, long long size, const volatile int *cancel, bs_img *out)
{
	char dir[512], file[560];
	int have_dir, rc;

	memset(out, 0, sizeof(*out));
	have_dir = thumb_dir(dir, sizeof(dir), 0) == 0;
	if (have_dir) {
		snprintf(file, sizeof(file), "%s/%016llx.t565", dir, bs_img_thumb_key(path, mtime, size));
		if (thumb_read(file, out) == 0)
			return BS_IMG_OK;
	}
	rc = bs_img_load(path, BS_THUMB, BS_THUMB, BS_IMG_COVER, cancel, out);
	if (rc == BS_IMG_OK && have_dir && thumb_dir(dir, sizeof(dir), 1) == 0)
		thumb_write(file, out);
	return rc;
}

static int key_cmp(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;

	return x < y ? -1 : x > y;
}

void bs_img_thumb_prune(const unsigned long long *keys, int n, const volatile int *cancel)
{
	char dir[512], file[800];
	struct dirent *de;
	DIR *d;

	if (thumb_dir(dir, sizeof(dir), 0) < 0)
		return;
	d = opendir(dir);
	if (!d)
		return;
	while ((de = readdir(d)) != NULL && !(cancel && *cancel)) {
		unsigned long long k;
		char *end;
		size_t len = strlen(de->d_name);

		if (de->d_name[0] == '.')
			continue;
		snprintf(file, sizeof(file), "%s/%s", dir, de->d_name);
		if (len == 21 && !strcmp(de->d_name + 16, ".t565")) {
			k = strtoull(de->d_name, &end, 16);
			if (end == de->d_name + 16 && bsearch(&k, keys, (size_t)n, sizeof(k), key_cmp))
				continue;
		} else if (len < 4 || strcmp(de->d_name + len - 4, ".tmp")) {
			continue;	/* non e' roba nostra */
		}
		unlink(file);
	}
	closedir(d);
}
