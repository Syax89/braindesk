// SPDX-License-Identifier: MIT
/*
 * Software model of the brain-pxp kernel driver (test builds only, -DPXP_SIM).
 *
 * It accepts and rejects exactly what linux-brain/drivers/misc/brain_pxp.c
 * accepts and rejects (same checks, same error codes), so that the player's
 * PXP outputs can be exercised under Xvfb: multiples of 8, the 1/4 limit of
 * the scaler, buffer sizes, and the rules of the PXP_BUF_SCREEN overlay.
 * "legacy" models the driver before the overlay: only the short struct
 * pxp_job is known and PXP_IOC_SCREEN does not exist.
 *
 * Only what the player uses is converted: planar YUV 4:2:0 / 4:2:2 to
 * RGB565, no rotation.
 */
#include "pxpdev.h"
#include "yuv2rgb.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SZ_4M		(4u << 20)
#define MAX_BLOCKS	255
#define SCALE_ONE	0x1000
#define SCALE_MAX	0x4000

struct pxp_job_v1 {
	__u32 f[22];
};
#define PXP_IOC_RUN_V1	_IOWR(PXP_IOC_MAGIC, 2, struct pxp_job_v1)

struct simbuf {
	uint8_t *mem;
	size_t size;
};

struct pxpsim {
	bool legacy;
	struct simbuf bufs[PXP_MAX_BUFFERS];
};

/* the "panel": one for the process, like the real one */
static struct {
	int w, h;
	uint16_t *fb;
	pxpsim_present_fn present;
	void *ctx;
} screen;

void pxpsim_set_screen(int width, int height, pxpsim_present_fn fn, void *ctx)
{
	free(screen.fb);
	screen.fb = NULL;
	screen.w = width;
	screen.h = height;
	screen.present = fn;
	screen.ctx = ctx;
	if (width > 0 && height > 0)
		screen.fb = calloc((size_t)width * height, 2);
}

struct pxpsim *pxpsim_new(bool legacy)
{
	struct pxpsim *s = calloc(1, sizeof(*s));

	if (s)
		s->legacy = legacy;
	return s;
}

void pxpsim_free(struct pxpsim *s)
{
	int i;

	if (!s)
		return;
	for (i = 0; i < PXP_MAX_BUFFERS; i++)
		free(s->bufs[i].mem);
	free(s);
}

void *pxpsim_mmap(struct pxpsim *s, size_t size, uint64_t offset)
{
	unsigned idx = (unsigned)(offset >> 24);

	if (idx >= PXP_MAX_BUFFERS || (offset & 0xffffff) || !s->bufs[idx].mem ||
	    size > s->bufs[idx].size) {
		errno = EINVAL;
		return NULL;
	}
	return s->bufs[idx].mem;
}

static int sim_alloc(struct pxpsim *s, struct pxp_alloc *a)
{
	int i;

	if (!a->size || a->size > SZ_4M)
		return -EINVAL;
	for (i = 0; i < PXP_MAX_BUFFERS; i++) {
		struct simbuf *b = &s->bufs[i];

		if (b->mem)
			continue;
		b->size = (a->size + 4095) & ~(size_t)4095;
		b->mem = calloc(1, b->size);
		if (!b->mem)
			return -ENOMEM;
		a->index = i;
		a->size = (uint32_t)b->size;
		a->mmap_offset = (uint64_t)i << 24;
		return 0;
	}
	return -ENOSPC;
}

static int check_blk(uint32_t v)
{
	return (v & 7) || (v >> 3) > MAX_BLOCKS;
}

static uint64_t s0_bytes(uint32_t fmt, uint32_t w, uint32_t h)
{
	uint64_t px = (uint64_t)w * h;

	switch (fmt) {
	case PXP_S0_ARGB8888:
	case PXP_S0_RGB888:
		return px * 4;
	case PXP_S0_RGB565:
	case PXP_S0_RGB555:
	case PXP_S0_UYVY:
	case PXP_S0_VYUY:
	case PXP_S0_YUV422:
		return px * 2;
	case PXP_S0_YUV420:
		return px * 3 / 2;
	default:
		return 0;
	}
}

static uint32_t rgb888_to_565(uint32_t c)
{
	return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
}

/* same order of checks as pxp_ioc_run() in the kernel */
static int sim_run(struct pxpsim *s, struct pxp_job *jp)
{
	struct pxp_job j = *jp;
	bool to_screen = j.out_index == PXP_BUF_SCREEN;
	uint32_t xscale = SCALE_ONE, yscale = SCALE_ONE;
	struct simbuf *s0;
	struct yuv_src src = { 0 };
	struct timespec t0, t1;
	uint16_t *out, bg;
	uint32_t px, x, y;

	if (j.s0_index >= PXP_MAX_BUFFERS || (!to_screen && j.out_index >= PXP_MAX_BUFFERS))
		return -EINVAL;
	if (j.reserved[0] || j.reserved[1] || j.reserved[2])
		return -EINVAL;
	if (j.rotate % 90 || j.rotate > 270)
		return -EINVAL;
	if (to_screen && (j.rotate || j.out_format != PXP_OUT_RGB565))
		return -EINVAL;
	if (j.rotate)
		return -ENOSYS;		/* not modelled */

	if (!j.dst_w)
		j.dst_w = j.out_width;
	if (!j.dst_h)
		j.dst_h = j.out_height;
	if (!j.src_w)
		j.src_w = j.dst_w;
	if (!j.src_h)
		j.src_h = j.dst_h;

	if (check_blk(j.s0_width) || check_blk(j.s0_height) ||
	    check_blk(j.src_x) || check_blk(j.src_y) ||
	    check_blk(j.dst_x) || check_blk(j.dst_y) ||
	    check_blk(j.dst_w) || check_blk(j.dst_h) ||
	    !j.s0_width || !j.s0_height || !j.dst_w || !j.dst_h ||
	    !j.out_width || !j.out_height ||
	    j.out_width > 0xfff || j.out_height > 0xfff)
		return -EINVAL;
	if ((uint64_t)j.src_x + j.src_w > j.s0_width ||
	    (uint64_t)j.src_y + j.src_h > j.s0_height)
		return -EINVAL;
	if ((uint64_t)j.dst_x + j.dst_w > j.out_width ||
	    (uint64_t)j.dst_y + j.dst_h > j.out_height)
		return -EINVAL;
	if (j.src_w != j.dst_w || j.src_h != j.dst_h) {
		if (j.s0_format < PXP_S0_YUV422 || j.s0_format > PXP_S0_VYUY)
			return -EINVAL;
		xscale = j.src_w * SCALE_ONE / j.dst_w;
		yscale = j.src_h * SCALE_ONE / j.dst_h;
		if (xscale > SCALE_MAX || yscale > SCALE_MAX)
			return -ERANGE;
	}

	s0 = &s->bufs[j.s0_index];
	if (!s0->mem)
		return -EINVAL;
	if (!s0_bytes(j.s0_format, j.s0_width, j.s0_height) ||
	    s0_bytes(j.s0_format, j.s0_width, j.s0_height) > s0->size)
		return -EINVAL;
	if (j.out_format != PXP_OUT_RGB565)
		return -ENOSYS;		/* not modelled */

	if (to_screen) {
		if (!screen.fb)
			return -ENODEV;
		if (j.out_width != (uint32_t)screen.w ||
		    (uint64_t)j.out_y + j.out_height > (uint32_t)screen.h)
			return -EINVAL;
		out = screen.fb + (size_t)j.out_y * screen.w;
	} else {
		uint64_t rows = (uint64_t)j.out_y + j.out_height;
		struct simbuf *ob = &s->bufs[j.out_index];

		if (!ob->mem || rows > 0xffff || (uint64_t)j.out_width * rows * 2 > ob->size)
			return -EINVAL;
		out = (uint16_t *)ob->mem + (size_t)j.out_y * j.out_width;
	}
	if (j.s0_format != PXP_S0_YUV420 && j.s0_format != PXP_S0_YUV422)
		return -ENOSYS;		/* not modelled */

	clock_gettime(CLOCK_MONOTONIC, &t0);
	px = j.s0_width * j.s0_height;
	src.y = s0->mem;
	src.u = s0->mem + px;
	src.v = src.u + (j.s0_format == PXP_S0_YUV420 ? px / 4 : px / 2);
	src.ystride = j.s0_width;
	src.cstride = j.s0_width / 2;
	src.csx = 1;
	src.csy = j.s0_format == PXP_S0_YUV420 ? 1 : 0;
	src.full_range = !!(j.flags & PXP_FLAG_YUV_FULL);

	/* the PXP always writes whole rows: background outside the picture */
	bg = (uint16_t)rgb888_to_565(j.background);
	for (y = 0; y < j.out_height; y++) {
		uint16_t *row = out + (size_t)y * j.out_width;
		bool inside = y >= j.dst_y && y < j.dst_y + j.dst_h;

		for (x = 0; x < (inside ? j.dst_x : j.out_width); x++)
			row[x] = bg;
		for (x = j.dst_x + j.dst_w; inside && x < j.out_width; x++)
			row[x] = bg;
	}
	yuv_to_rgb565(&src, j.src_x, j.src_y, j.src_w, j.src_h,
		      out + (size_t)j.dst_y * j.out_width + j.dst_x, j.out_width,
		      j.dst_w, j.dst_h);

	if (to_screen && screen.present)
		screen.present(screen.ctx, out, j.out_y, j.out_height);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	jp->stat = 1;
	jp->elapsed_us = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
				    (t1.tv_nsec - t0.tv_nsec) / 1000);
	return 0;
}

int pxpsim_ioctl(struct pxpsim *s, unsigned long req, void *arg)
{
	int r = -ENOTTY;

	if (req == PXP_IOC_ALLOC) {
		r = sim_alloc(s, arg);
	} else if (req == PXP_IOC_FREE) {
		uint32_t idx = *(uint32_t *)arg;

		r = -EINVAL;
		if (idx < PXP_MAX_BUFFERS) {
			free(s->bufs[idx].mem);
			memset(&s->bufs[idx], 0, sizeof(s->bufs[idx]));
			r = 0;
		}
	} else if (req == PXP_IOC_RUN && !s->legacy) {
		r = sim_run(s, arg);
	} else if (req == PXP_IOC_RUN_V1 && s->legacy) {
		struct pxp_job j = { 0 };

		memcpy(&j, arg, sizeof(struct pxp_job_v1));
		/* that driver knew nothing about the screen */
		r = j.out_index >= PXP_MAX_BUFFERS ? -EINVAL : sim_run(s, &j);
		memcpy(arg, &j, sizeof(struct pxp_job_v1));
	} else if (req == PXP_IOC_SCREEN && !s->legacy) {
		struct pxp_screen *scr = arg;

		r = -ENODEV;
		if (screen.fb) {
			scr->width = screen.w;
			scr->height = screen.h;
			scr->pitch = screen.w * 2;
			scr->format = PXP_OUT_RGB565;
			r = 0;
		}
	}
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return 0;
}
