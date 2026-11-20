// SPDX-License-Identifier: MIT
/*
 * test/imgtest.c - prova di bs_img.c fuori dalla shell.
 *
 *   imgtest FILE LARGHEZZA ALTEZZA fit|cover [USCITA.ppm]
 *
 * Stampa esito, dimensioni e tempo; con USCITA scrive il risultato (RGB565
 * riportato a 8 bit) per confrontarlo con un riferimento. Usato da
 * test/imgtest.sh, anche con ASan/UBSan su file rovinati.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../bs_img.h"

/* bs_img.c usa solo questo di bui */
const uint8_t bui_bayer8[64] = {
	0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
	12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
	3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
	15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21,
};

int main(int argc, char **argv)
{
	struct timespec t0, t1;
	bs_img im;
	int rc, i;

	if (argc < 5) {
		fprintf(stderr, "uso: imgtest FILE W H fit|cover [USCITA.ppm]\n");
		return 2;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	rc = bs_img_load(argv[1], atoi(argv[2]), atoi(argv[3]),
			 !strcmp(argv[4], "cover") ? BS_IMG_COVER : BS_IMG_FIT, NULL, &im);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("%s: rc=%d %dx%d (origine %dx%d) %ld ms\n", argv[1], rc, im.w, im.h, im.src_w, im.src_h,
	       (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
	if (rc == BS_IMG_OK && argc > 5) {
		FILE *f = fopen(argv[5], "wb");

		if (!f)
			return 1;
		fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
		for (i = 0; i < im.w * im.h; i++) {
			unsigned p = im.px[i], r = p >> 11, g = (p >> 5) & 63, b = p & 31;

			fputc((int)((r << 3) | (r >> 2)), f);
			fputc((int)((g << 2) | (g >> 4)), f);
			fputc((int)((b << 3) | (b >> 2)), f);
		}
		fclose(f);
	}
	bs_img_free(&im);
	return rc == BS_IMG_OK ? 0 : 10 + rc;
}
