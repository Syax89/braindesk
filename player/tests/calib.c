/* calibration: the CPU conversion loop of driver/pxpvideo.c (measured ~120 ms on the Brain) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void cpu_convert(const uint8_t *yuv, int sw, int sh, uint16_t *map, int pitch, int dw, int dh)
{
	const uint8_t *Y = yuv, *U = Y + sw * sh, *V = U + sw * sh / 4;
	int x, y;
	for (y = 0; y < dh; y++) {
		int sy = y * sh / dh;
		uint16_t *row = (uint16_t *)((uint8_t *)map + y * pitch);
		for (x = 0; x < dw; x++) {
			int sx = x * sw / dw;
			int c = Y[sy * sw + sx] - 16;
			int d = U[(sy / 2) * (sw / 2) + sx / 2] - 128;
			int e = V[(sy / 2) * (sw / 2) + sx / 2] - 128;
			int r = clamp8((298 * c + 409 * e + 128) >> 8);
			int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
			int b = clamp8((298 * c + 516 * d + 128) >> 8);
			row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		}
	}
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void)
{
	int sw = 424, sh = 240, dw = 848, dh = 480, i, n = 50;
	uint8_t *yuv = malloc(sw * sh * 3 / 2);
	uint16_t *fb = malloc(854 * 480 * 2);
	double t0;
	for (i = 0; i < sw * sh * 3 / 2; i++) yuv[i] = (uint8_t)(i * 7 + (i >> 8));
	cpu_convert(yuv, sw, sh, fb, 854 * 2, dw, dh);
	t0 = now();
	for (i = 0; i < n; i++) cpu_convert(yuv, sw, sh, fb, 854 * 2, dw, dh);
	printf("cpu_convert 424x240 -> 848x480: %.2f ms per frame under this CPU\n", (now() - t0) / n * 1e3);
	return fb[1000] == 12345;
}
