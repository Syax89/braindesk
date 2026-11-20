// SPDX-License-Identifier: MIT
/*
 * bake: strumento lato host che trasforma font TrueType/OpenType in tabelle C
 * (maschere A8) per il toolkit bui. Gira solo durante "make bake": sul Brain
 * non servono freetype, fontconfig ne' file di font.
 *
 *   bake <dir DejaVu> <font icone> <file .codepoints> <dir uscita>
 *
 * Produce: bui_assets.h, bui_fonts.c, bui_icons.c
 *
 * Qui i float sono permessi (e' un programma per il PC di sviluppo).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ------------------------------------------------------------------ */
/* insiemi di caratteri                                               */
/* ------------------------------------------------------------------ */

/* testo: ASCII + lettere accentate Latin-1 + qualche simbolo */
static const int cs_text_extra[] = {
	0xA0, 0xA1, 0xA3, 0xA7, 0xA9, 0xAB, 0xAE, 0xB0, 0xB1, 0xB2, 0xB3, 0xB5,
	0xB7, 0xBB, 0xBF,
	/* 0xC0..0xFF aggiunte in blocco */
	0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
	0x20AC, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x2713, 0
};
/* tasti della calcolatrice e numeri medi */
static const char cs_keys[] = "0123456789.,:+-=%()/*ACEe ";
static const int cs_keys_extra[] = { 0xB1, 0xD7, 0xF7, 0x2212, 0x221A, 0 };
/* numeri grandi (risultati, cronometro) */
static const char cs_num[] = "0123456789.,:+-=%()Ee ";
static const int cs_num_extra[] = { 0xD7, 0xF7, 0x2212, 0x2026, 0 };
/* cifre enormi (orologio) */
static const char cs_digits[] = "0123456789:., -";

enum { CS_TEXT, CS_KEYS, CS_NUM, CS_DIGITS };

struct fontspec {
	const char *name;	/* bui_font_<name> */
	const char *file;
	int px;
	int charset;
	int kern;
	double gamma;		/* <1 rinforza il testo chiaro su fondo scuro */
};

static const struct fontspec fonts[] = {
	{ "sans15", "DejaVuSans.ttf",      15, CS_TEXT,   1, 0.80 },
	{ "sans18", "DejaVuSans.ttf",      18, CS_TEXT,   1, 0.82 },
	{ "bold15", "DejaVuSans-Bold.ttf", 15, CS_TEXT,   1, 0.85 },
	{ "bold18", "DejaVuSans-Bold.ttf", 18, CS_TEXT,   1, 0.88 },
	{ "bold22", "DejaVuSans-Bold.ttf", 22, CS_TEXT,   1, 0.90 },
	{ "key26",  "DejaVuSans.ttf",      26, CS_KEYS,   0, 0.90 },
	{ "num44",  "DejaVuSans.ttf",      44, CS_NUM,    0, 1.00 },
	{ "num88",  "DejaVuSans.ttf",      88, CS_DIGITS, 0, 1.00 },
};

struct iconspec {
	const char *name;	/* bui_ic_<name>_<px> */
	const char *glyph;	/* nome Material Icons */
	int sizes[4];
};

static const struct iconspec icons[] = {
	/* barra, intestazioni, pulsanti */
	{ "home",       "home",               { 24 } },
	{ "back",       "arrow_back",         { 24 } },
	{ "close",      "close",              { 18, 24 } },
	{ "vol_up",     "volume_up",          { 24 } },
	{ "vol_down",   "volume_down",        { 24 } },
	{ "vol_off",    "volume_off",         { 24 } },
	{ "bolt",       "bolt",               { 16, 24 } },
	{ "brightness", "brightness_6",       { 24 } },
	{ "rotate",     "screen_rotation",    { 24 } },
	{ "screenshot", "screenshot_monitor", { 24 } },
	{ "power",      "power_settings_new", { 24 } },
	{ "restart",    "restart_alt",        { 24 } },
	{ "play",       "play_arrow",         { 24 } },
	{ "pause",      "pause",              { 24 } },
	{ "replay",     "replay",             { 24 } },
	{ "flag",       "flag",               { 24 } },
	{ "add",        "add",                { 24 } },
	{ "remove",     "remove",             { 24 } },
	{ "check",      "check",              { 24 } },
	{ "info",       "info",               { 24 } },
	{ "usb",        "usb",                { 24 } },
	{ "memory",     "memory",             { 24 } },
	{ "storage",    "sd_storage",         { 24 } },
	{ "battery",    "battery_full",       { 24 } },
	{ "backspace",  "backspace",          { 24 } },
	{ "chevron",    "chevron_right",      { 24 } },
	{ "expand",     "expand_more",        { 24 } },
	{ "warning",    "warning",            { 24 } },
	{ "timer",      "timer",              { 24, 40 } },
	{ "hourglass",  "hourglass_empty",    { 24 } },
	{ "stopwatch",  "timer",              { 0 } },	/* alias, non generato */
	/* app (riquadri della home a 40 px, righe a 24 px) */
	{ "settings",   "settings",           { 24, 40 } },
	{ "clock",      "schedule",           { 24, 40 } },
	{ "calc",       "calculate",          { 24, 40 } },
	{ "terminal",   "terminal",           { 24, 40 } },
	{ "folder",     "folder",             { 24, 40 } },
	{ "files",      "folder_open",        { 24, 40 } },
	{ "gallery",    "photo_library",      { 24, 40 } },
	{ "music",      "music_note",         { 24, 40 } },
	{ "video",      "movie",              { 24, 40 } },
	{ "note",       "sticky_note_2",      { 24, 40 } },
	{ "apps",       "apps",               { 24, 40 } },
	/* Esplora e Galleria */
	{ "more",       "more_vert",          { 24 } },
	{ "trash",      "delete",             { 24 } },
	{ "edit",       "edit",               { 24 } },
	{ "up",         "arrow_upward",       { 24 } },
	{ "eye",        "visibility",         { 24 } },
	{ "eye_off",    "visibility_off",     { 24 } },
	{ "image",      "image",              { 24, 40 } },
	{ "broken",     "broken_image",       { 40 } },
	{ "doc",        "description",        { 24 } },
	{ "file",       "insert_drive_file",  { 24 } },
	{ "audio",      "audiotrack",         { 24 } },
	{ "sdcard",     "sd_card",            { 24 } },
	{ "prev",       "chevron_left",       { 24 } },
	{ "open",       "open_in_new",        { 24 } },
};

#define NFONTS ((int)(sizeof(fonts) / sizeof(fonts[0])))
#define NICONS ((int)(sizeof(icons) / sizeof(icons[0])))

/* ------------------------------------------------------------------ */

static unsigned char *load(const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;

	if (!f) {
		fprintf(stderr, "bake: impossibile aprire %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n);
	if (fread(buf, 1, n, f) != (size_t)n)
		exit(1);
	fclose(f);
	return buf;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int charset(int which, int *out)
{
	int n = 0, i;

	switch (which) {
	case CS_TEXT:
		for (i = 0x20; i <= 0x7E; i++)
			out[n++] = i;
		for (i = 0xC0; i <= 0xFF; i++)
			out[n++] = i;
		for (i = 0; cs_text_extra[i]; i++)
			out[n++] = cs_text_extra[i];
		break;
	case CS_KEYS:
		for (i = 0; cs_keys[i]; i++)
			out[n++] = (unsigned char)cs_keys[i];
		for (i = 0; cs_keys_extra[i]; i++)
			out[n++] = cs_keys_extra[i];
		break;
	case CS_NUM:
		for (i = 0; cs_num[i]; i++)
			out[n++] = (unsigned char)cs_num[i];
		for (i = 0; cs_num_extra[i]; i++)
			out[n++] = cs_num_extra[i];
		break;
	case CS_DIGITS:
		for (i = 0; cs_digits[i]; i++)
			out[n++] = (unsigned char)cs_digits[i];
		break;
	}
	qsort(out, n, sizeof(int), cmp_int);
	/* niente doppioni */
	{
		int m = 0;

		for (i = 0; i < n; i++)
			if (!m || out[m - 1] != out[i])
				out[m++] = out[i];
		n = m;
	}
	return n;
}

static void emit_bytes(FILE *o, const unsigned char *p, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (i % 20 == 0)
			fputs("\n\t", o);
		fprintf(o, "%d,", p[i]);
	}
}

static long total_bytes;

static void bake_font(FILE *o, FILE *h, const char *dir, const struct fontspec *fs)
{
	char path[1024];
	unsigned char *ttf, *data = NULL, lut[256];
	stbtt_fontinfo f;
	int cps[512], n, i, j, asc, desc, gap, nk = 0;
	int *off, *gw, *gh, *gx, *gy, *adv;
	long dlen = 0, dcap = 0;
	float sc;
	int x0, y0, x1, y1, cap_h, x_h;

	snprintf(path, sizeof(path), "%s/%s", dir, fs->file);
	ttf = load(path);
	if (!stbtt_InitFont(&f, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
		fprintf(stderr, "bake: font non valido %s\n", path);
		exit(1);
	}
	sc = stbtt_ScaleForMappingEmToPixels(&f, (float)fs->px);
	stbtt_GetFontVMetrics(&f, &asc, &desc, &gap);
	n = charset(fs->charset, cps);
	off = calloc(n, sizeof(int));
	gw = calloc(n, sizeof(int));
	gh = calloc(n, sizeof(int));
	gx = calloc(n, sizeof(int));
	gy = calloc(n, sizeof(int));
	adv = calloc(n, sizeof(int));

	for (i = 0; i < 256; i++)
		lut[i] = (unsigned char)(pow(i / 255.0, fs->gamma) * 255.0 + 0.5);

	for (i = 0; i < n; i++) {
		int g = stbtt_FindGlyphIndex(&f, cps[i]);
		int a, lsb, w, hh;

		if (!g && cps[i] != 0x20)
			fprintf(stderr, "bake: %s senza U+%04X\n", fs->name, cps[i]);
		stbtt_GetGlyphHMetrics(&f, g, &a, &lsb);
		stbtt_GetGlyphBitmapBox(&f, g, sc, sc, &x0, &y0, &x1, &y1);
		w = x1 - x0;
		hh = y1 - y0;
		adv[i] = (int)floor(a * sc * 64.0 + 0.5);
		off[i] = (int)dlen;
		gw[i] = w;
		gh[i] = hh;
		gx[i] = x0;
		gy[i] = -y0;		/* righe sopra la linea di base */
		if (w > 255 || hh > 255 || gy[i] > 127 || gy[i] < -128) {
			fprintf(stderr, "bake: glifo troppo grande\n");
			exit(1);
		}
		if (w <= 0 || hh <= 0) {
			gw[i] = gh[i] = 0;
			continue;
		}
		if (dlen + w * hh > dcap) {
			dcap = (dlen + w * hh) * 2 + 4096;
			data = realloc(data, dcap);
		}
		stbtt_MakeGlyphBitmap(&f, data + dlen, w, hh, w, sc, sc, g);
		for (j = 0; j < w * hh; j++)
			data[dlen + j] = lut[data[dlen + j]];
		dlen += w * hh;
	}

	stbtt_GetCodepointBitmapBox(&f, 'H', sc, sc, &x0, &y0, &x1, &y1);
	cap_h = -y0;
	stbtt_GetCodepointBitmapBox(&f, 'x', sc, sc, &x0, &y0, &x1, &y1);
	x_h = -y0;
	if (fs->charset == CS_DIGITS || fs->charset == CS_NUM || fs->charset == CS_KEYS) {
		stbtt_GetCodepointBitmapBox(&f, '0', sc, sc, &x0, &y0, &x1, &y1);
		cap_h = -y0;
	}

	fprintf(o, "\n/* %s: %s a %d px, %d glifi, %ld byte */\n", fs->name, fs->file, fs->px, n, dlen);
	fprintf(o, "static const uint8_t %s_data[] = {", fs->name);
	emit_bytes(o, data, (int)dlen);
	fprintf(o, "\n};\n");
	fprintf(o, "static const bui_glyph %s_glyphs[] = {\n", fs->name);
	for (i = 0; i < n; i++)
		fprintf(o, "\t{ 0x%04x, %d, %d, %d, %d, %d, %d },\n", cps[i], adv[i], off[i],
			gw[i], gh[i], gx[i], gy[i]);
	fprintf(o, "};\n");

	if (fs->kern) {
		/* solo coppie ASCII con spostamento di almeno ~0,4 px */
		fprintf(o, "static const uint16_t %s_kpair[] = {", fs->name);
		for (i = 0x21; i <= 0x7E; i++)
			for (j = 0x21; j <= 0x7E; j++) {
				int k = stbtt_GetCodepointKernAdvance(&f, i, j);
				int d = (int)floor(k * sc * 64.0 + 0.5);

				if (d > -26 && d < 26)
					continue;
				if (nk % 12 == 0)
					fputs("\n\t", o);
				fprintf(o, "0x%04x,", (i << 8) | j);
				nk++;
			}
		fprintf(o, "\n};\nstatic const int8_t %s_kdx[] = {", fs->name);
		nk = 0;
		for (i = 0x21; i <= 0x7E; i++)
			for (j = 0x21; j <= 0x7E; j++) {
				int k = stbtt_GetCodepointKernAdvance(&f, i, j);
				int d = (int)floor(k * sc * 64.0 + 0.5);

				if (d > -26 && d < 26)
					continue;
				if (d < -127)
					d = -127;
				if (d > 127)
					d = 127;
				if (nk % 20 == 0)
					fputs("\n\t", o);
				fprintf(o, "%d,", d);
				nk++;
			}
		fprintf(o, "\n};\n");
	}

	fprintf(o, "const bui_font bui_font_%s = {\n", fs->name);
	fprintf(o, "\t%s_glyphs, %s_data, ", fs->name, fs->name);
	if (fs->kern && nk)
		fprintf(o, "%s_kpair, %s_kdx,\n", fs->name, fs->name);
	else
		fprintf(o, "0, 0,\n");
	fprintf(o, "\t%d, %d, %d, %d, %d, %d, %d, %d,\n};\n", n, nk, fs->px,
		(int)ceil(asc * sc), (int)ceil(-desc * sc),
		(int)ceil((asc - desc + gap) * sc), cap_h, x_h);
	fprintf(h, "extern const bui_font bui_font_%s;\n", fs->name);
	fprintf(stderr, "  font %-7s %3d px  %3d glifi  %4d coppie  %6ld byte\n", fs->name, fs->px, n, nk,
		dlen + n * 12L + nk * 3L);
	total_bytes += dlen + n * 12L + nk * 3L;
	free(data);
	free(ttf);
}

static int lookup_cp(const char *cpfile, const char *name)
{
	FILE *f = fopen(cpfile, "r");
	char nm[128];
	unsigned cp;

	if (!f) {
		fprintf(stderr, "bake: impossibile aprire %s\n", cpfile);
		exit(1);
	}
	while (fscanf(f, "%127s %x", nm, &cp) == 2)
		if (!strcmp(nm, name)) {
			fclose(f);
			return (int)cp;
		}
	fclose(f);
	fprintf(stderr, "bake: icona sconosciuta %s\n", name);
	exit(1);
}

static void bake_icons(FILE *o, FILE *h, const char *fontfile, const char *cpfile)
{
	unsigned char *ttf = load(fontfile);
	stbtt_fontinfo f;
	int i, s;

	if (!stbtt_InitFont(&f, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
		fprintf(stderr, "bake: font icone non valido\n");
		exit(1);
	}
	for (i = 0; i < NICONS; i++) {
		int cp, g;

		if (!icons[i].sizes[0])
			continue;
		cp = lookup_cp(cpfile, icons[i].glyph);
		g = stbtt_FindGlyphIndex(&f, cp);
		if (!g) {
			fprintf(stderr, "bake: glifo mancante per %s\n", icons[i].glyph);
			exit(1);
		}
		for (s = 0; s < 4 && icons[i].sizes[s]; s++) {
			int px = icons[i].sizes[s];
			float sc = stbtt_ScaleForMappingEmToPixels(&f, (float)px);
			int x0, y0, x1, y1, w, hh, ox, oy;
			unsigned char *bm;

			/* le icone Material occupano il quadrato em, linea di base in basso */
			stbtt_GetGlyphBitmapBox(&f, g, sc, sc, &x0, &y0, &x1, &y1);
			w = x1 - x0;
			hh = y1 - y0;
			ox = x0;
			oy = px + y0;
			if (ox < 0 || oy < 0 || ox + w > px || oy + hh > px)
				fprintf(stderr, "bake: %s_%d esce dalla cella (%d,%d %dx%d)\n",
					icons[i].name, px, ox, oy, w, hh);
			if (ox < 0)
				ox = 0;
			if (oy < 0)
				oy = 0;
			bm = calloc(1, w * hh + 1);
			stbtt_MakeGlyphBitmap(&f, bm, w, hh, w, sc, sc, g);
			fprintf(o, "\nstatic const uint8_t ic_%s_%d_a8[] = {", icons[i].name, px);
			emit_bytes(o, bm, w * hh);
			fprintf(o, "\n};\nconst bui_icon bui_ic_%s_%d = { %d, %d, %d, %d, %d, ic_%s_%d_a8 };\n",
				icons[i].name, px, px, w, hh, ox, oy, icons[i].name, px);
			fprintf(h, "extern const bui_icon bui_ic_%s_%d;\n", icons[i].name, px);
			total_bytes += w * hh + 12;
			free(bm);
		}
	}
	free(ttf);
}

int main(int argc, char **argv)
{
	char path[1024];
	FILE *o, *h;
	int i;

	if (argc != 5) {
		fprintf(stderr, "uso: bake <dir DejaVu> <font icone> <codepoints> <dir uscita>\n");
		return 1;
	}
	snprintf(path, sizeof(path), "%s/bui_assets.h", argv[4]);
	h = fopen(path, "w");
	if (!h) {
		perror(path);
		return 1;
	}
	fprintf(h, "/* GENERATO da tools/bake.c (make bake): non modificare a mano. */\n"
		   "#ifndef BUI_ASSETS_H\n#define BUI_ASSETS_H\n\n"
		   "/* font: DejaVu Sans (licenza Bitstream Vera / DejaVu) */\n");

	snprintf(path, sizeof(path), "%s/bui_fonts.c", argv[4]);
	o = fopen(path, "w");
	if (!o) {
		perror(path);
		return 1;
	}
	fprintf(o, "/* GENERATO da tools/bake.c (make bake): non modificare a mano.\n"
		   " * Glifi DejaVu Sans come maschere A8. */\n#include \"../bui.h\"\n");
	for (i = 0; i < NFONTS; i++)
		bake_font(o, h, argv[1], &fonts[i]);
	fclose(o);

	snprintf(path, sizeof(path), "%s/bui_icons.c", argv[4]);
	o = fopen(path, "w");
	if (!o) {
		perror(path);
		return 1;
	}
	fprintf(o, "/* GENERATO da tools/bake.c (make bake): non modificare a mano.\n"
		   " * Icone Material Icons Round (Apache-2.0) come maschere A8. */\n#include \"../bui.h\"\n");
	fprintf(h, "\n/* icone: Material Icons Round (Apache-2.0) */\n");
	bake_icons(o, h, argv[2], argv[3]);
	fclose(o);

	fprintf(h, "\n#endif\n");
	fclose(h);
	fprintf(stderr, "  totale dati: %ld byte\n", total_bytes);
	return 0;
}
