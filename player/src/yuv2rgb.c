// SPDX-License-Identifier: MIT
#include "yuv2rgb.h"

#include <pthread.h>
#include <string.h>

/* BT.601; index 0 = studio range (16..235), 1 = full range (JPEG) */
static int16_t tab_y[2][256], tab_rv[2][256], tab_gu[2][256], tab_gv[2][256], tab_bu[2][256];
/* clamp to 0..255 and move to the RGB565 position; index = value + CLIP_OFF */
#define CLIP_OFF 384
static uint16_t clip_r[1024], clip_g[1024], clip_b[1024];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init_tables(void)
{
	int i;

	for (i = 0; i < 256; i++) {
		int c = i - 128;

		tab_y[0][i] = (int16_t)((298 * (i - 16) + 128) >> 8);
		tab_rv[0][i] = (int16_t)((409 * c) >> 8);
		tab_gu[0][i] = (int16_t)((-100 * c) >> 8);
		tab_gv[0][i] = (int16_t)((-208 * c) >> 8);
		tab_bu[0][i] = (int16_t)((516 * c) >> 8);

		tab_y[1][i] = (int16_t)i;
		tab_rv[1][i] = (int16_t)((359 * c) >> 8);
		tab_gu[1][i] = (int16_t)((-88 * c) >> 8);
		tab_gv[1][i] = (int16_t)((-183 * c) >> 8);
		tab_bu[1][i] = (int16_t)((454 * c) >> 8);
	}
	for (i = 0; i < 1024; i++) {
		int v = i - CLIP_OFF;

		v = v < 0 ? 0 : v > 255 ? 255 : v;
		clip_r[i] = (uint16_t)((v >> 3) << 11);
		clip_g[i] = (uint16_t)((v >> 2) << 5);
		clip_b[i] = (uint16_t)(v >> 3);
	}
}

void yuv_to_rgb565(const struct yuv_src *s, int sx, int sy, int sw, int sh,
		   uint16_t *dst, int dstride, int dw, int dh)
{
	int r = s->full_range ? 1 : 0;
	const int16_t *ty = tab_y[r], *trv = tab_rv[r], *tgu = tab_gu[r],
		      *tgv = tab_gv[r], *tbu = tab_bu[r];
	uint32_t xstep, ystep, fy;
	int last_row = -1;
	int i, j;

	pthread_once(&once, init_tables);
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
		return;
	xstep = ((uint32_t)sw << 16) / dw;
	ystep = ((uint32_t)sh << 16) / dh;
	fy = ystep / 2;

	for (j = 0; j < dh; j++, fy += ystep) {
		int row = sy + (int)(fy >> 16);
		uint16_t *d = dst + (size_t)j * dstride;
		const uint8_t *yp;
		uint32_t fx = xstep / 2;

		if (row == last_row) {
			/* upscaling: same source row as the line above */
			memcpy(d, d - dstride, (size_t)dw * 2);
			continue;
		}
		last_row = row;
		yp = s->y + row * s->ystride + sx;
		if (!s->u) {
			for (i = 0; i < dw; i++, fx += xstep) {
				int y = ty[yp[fx >> 16]] + CLIP_OFF;

				d[i] = clip_r[y] | clip_g[y] | clip_b[y];
			}
		} else {
			const uint8_t *up = s->u + (row >> s->csy) * s->cstride;
			const uint8_t *vp = s->v + (row >> s->csy) * s->cstride;
			int csx = s->csx;

			for (i = 0; i < dw; i++, fx += xstep) {
				int x = (int)(fx >> 16);
				int c = (sx + x) >> csx;
				int y = ty[yp[x]] + CLIP_OFF;
				int u = up[c], v = vp[c];

				d[i] = clip_r[y + trv[v]] | clip_g[y + tgu[u] + tgv[v]] |
				       clip_b[y + tbu[u]];
			}
		}
	}
}
