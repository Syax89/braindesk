/* SPDX-License-Identifier: MIT */
/*
 * Integer YUV -> RGB565 with nearest-neighbour scaling.
 * Used by the software output (vo_x11) and by the PXP simulator; on the
 * Brain the PXP does this job in hardware.
 */
#ifndef BRAINPLAY_YUV2RGB_H
#define BRAINPLAY_YUV2RGB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct yuv_src {
	const uint8_t *y, *u, *v;	/* u == NULL: grey */
	ptrdiff_t ystride, cstride;
	int csx, csy;			/* log2 of the chroma subsampling */
	bool full_range;
};

/*
 * Converts the source rectangle (sx, sy, sw, sh) to a dw x dh RGB565 image.
 * dstride is in pixels.
 */
void yuv_to_rgb565(const struct yuv_src *s, int sx, int sy, int sw, int sh,
		   uint16_t *dst, int dstride, int dw, int dh);

#endif
