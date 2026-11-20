// SPDX-License-Identifier: MIT
/*
 * Video output through the PXP of the i.MX28 (hardware YUV -> RGB565
 * conversion and scaling, about 6 ms for a full-screen frame).
 *
 * The decoded frame is copied once, plane by plane, into the PXP source
 * buffer (contiguous DMA memory, write-combined: written sequentially, never
 * read back). From there:
 *
 *  - screen overlay (the normal way on the Brain): the PXP writes straight
 *    into the frame buffer the panel is showing, in the rows of the video
 *    area, and the kernel pushes those rows to the panel. X knows nothing
 *    about those pixels, so this is only done while our window covers those
 *    rows from edge to edge and nothing is on top of it. The letterbox bars
 *    come from the PXP too (background colour).
 *
 *  - XShm: the PXP writes into a second DMA buffer that is then copied to a
 *    shared image and given to X, which clips it like any other drawing.
 *    For kernels without the overlay, and while the window is partly
 *    covered or not full width.
 *
 * PXP rules: coordinates and sizes in multiples of 8 (not out_width /
 * out_height / out_y), source at most 2040 pixels per side, scaling down to
 * 1/4 at most (bigger sources are cropped around their centre).
 */
#include "brainplay.h"
#include "pxpdev.h"
#include "vo.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN8(x) (((x) + 7) & ~7)

struct pxp_priv {
	struct pxpdev *dev;
	bool have_screen;
	struct pxp_screen scr;

	/* source: planar YUV 4:2:0, padded to multiples of 8 */
	uint8_t *s_map;
	size_t s_size;
	uint32_t s_index;
	int s_w, s_h;
	bool have_frame, full_range;
	AVRational dar;

	/* XShm path */
	uint8_t *o_map;
	size_t o_size;
	uint32_t o_index;
	struct ximg *img;
	bool was_overlay;
	bool bad_format;
	int failures;		/* consecutive failed jobs */
};

static int pxp_init(struct vo *vo, const struct options *opt)
{
	struct pxp_priv *pp = calloc(1, sizeof(*pp));

	if (!pp)
		return -1;
	pp->dev = pxpdev_open(opt->pxp_sim);
	if (!pp->dev) {
		free(pp);
		return -1;
	}
	if (strcmp(opt->vo, "pxp-shm"))
		pp->have_screen = pxpdev_screen(pp->dev, &pp->scr);
	if (pp->have_screen &&
	    ((int)pp->scr.width != vo->win->scr_w || (int)pp->scr.height != vo->win->scr_h ||
	     pp->scr.pitch != pp->scr.width * 2)) {
		info("PXP: the panel is %ux%u (pitch %u) but the X screen is %dx%d: no overlay",
		     pp->scr.width, pp->scr.height, pp->scr.pitch, vo->win->scr_w, vo->win->scr_h);
		pp->have_screen = false;
	}
	vo->priv = pp;
	vo->path = pp->have_screen ? "pxp_screen" : "pxp_shm";
	verb("video output: PXP, %s", pp->have_screen ? "screen overlay" : "through XShm");
	return 0;
}

static void free_out(struct vo *vo)
{
	struct pxp_priv *pp = vo->priv;

	ximg_free(vo->win, pp->img);
	pp->img = NULL;
	pxpdev_free(pp->dev, pp->o_map, pp->o_size, pp->o_index);
	pp->o_map = NULL;
}

static void pxp_uninit(struct vo *vo)
{
	struct pxp_priv *pp = vo->priv;

	free_out(vo);
	pxpdev_free(pp->dev, pp->s_map, pp->s_size, pp->s_index);
	pxpdev_close(pp->dev);
	free(pp);
}

/* one plane into the source buffer, replicating the last column and row */
static void copy_plane(uint8_t *dst, int dw, int dh, const uint8_t *src, ptrdiff_t stride,
		       int sw, int sh, int xshift, int yshift)
{
	int x, y;

	for (y = 0; y < dh; y++, dst += dw) {
		int sy = y << 1 >> yshift;
		const uint8_t *s = src + (sy < sh ? sy : sh - 1) * stride;

		if (xshift == 1) {
			/* same horizontal sampling: plain row copy */
			int n = sw < dw ? sw : dw;

			memcpy(dst, s, n);
			if (dw > n)
				memset(dst + n, s[n - 1], dw - n);
		} else {
			for (x = 0; x < dw; x++) {
				int sx = x << 1 >> xshift;

				dst[x] = s[sx < sw ? sx : sw - 1];
			}
		}
	}
}

static int upload(struct vo *vo, const AVFrame *f, AVRational sar)
{
	struct pxp_priv *pp = vo->priv;
	int w = f->width, h = f->height, sw = ALIGN8(w), sh = ALIGN8(h);
	int csx = 0, csy = 0;
	bool grey = false, full = false;
	uint8_t *y, *u, *v;

	if (!vo_format_ok(f, &csx, &csy, &grey, &full) || sw > 2040 || sh > 2040 || w < 8 || h < 8) {
		if (!pp->bad_format)
			err("the PXP cannot show this picture (%s %dx%d)",
			    av_get_pix_fmt_name(f->format), w, h);
		pp->bad_format = true;
		pp->have_frame = false;
		return -1;
	}
	pp->bad_format = false;
	if (!pp->s_map || sw != pp->s_w || sh != pp->s_h) {
		pxpdev_free(pp->dev, pp->s_map, pp->s_size, pp->s_index);
		pp->s_size = (size_t)sw * sh * 3 / 2;
		pp->s_map = pxpdev_alloc(pp->dev, &pp->s_size, &pp->s_index);
		pp->have_frame = false;
		if (!pp->s_map)
			return -1;
		pp->s_w = sw;
		pp->s_h = sh;
		vo->need_clear = true;
	}
	y = pp->s_map;
	u = y + (size_t)sw * sh;
	v = u + (size_t)sw * sh / 4;
	/* luma: shift 1 with a doubled destination index = one to one */
	{
		int row;
		uint8_t *d = y;

		for (row = 0; row < sh; row++, d += sw) {
			const uint8_t *s = f->data[0] + (row < h ? row : h - 1) * (ptrdiff_t)f->linesize[0];

			memcpy(d, s, w);
			if (sw > w)
				memset(d + w, s[w - 1], sw - w);
		}
	}
	if (grey) {
		memset(u, 128, (size_t)sw * sh / 2);
	} else {
		int cw = AV_CEIL_RSHIFT(w, csx), ch = AV_CEIL_RSHIFT(h, csy);

		copy_plane(u, sw / 2, sh / 2, f->data[1], f->linesize[1], cw, ch, csx, csy);
		copy_plane(v, sw / 2, sh / 2, f->data[2], f->linesize[2], cw, ch, csx, csy);
	}
	pp->full_range = full;
	pp->dar = vo_dar(w, h, sar);
	pp->have_frame = true;
	return 0;
}

/* source crop for a dw x dh destination: the scaler stops at 1/4 */
static void fill_job(const struct pxp_priv *pp, struct pxp_job *j, int dw, int dh)
{
	int cw = pp->s_w, ch = pp->s_h;

	if (cw > dw * 4)
		cw = dw * 4;
	if (ch > dh * 4)
		ch = dh * 4;
	memset(j, 0, sizeof(*j));
	j->s0_index = pp->s_index;
	j->s0_format = PXP_S0_YUV420;
	j->s0_width = pp->s_w;
	j->s0_height = pp->s_h;
	j->src_w = cw;
	j->src_h = ch;
	j->src_x = ((pp->s_w - cw) / 2) & ~7;
	j->src_y = ((pp->s_h - ch) / 2) & ~7;
	j->out_format = PXP_OUT_RGB565;
	j->dst_w = dw;
	j->dst_h = dh;
	j->flags = pp->full_range ? PXP_FLAG_YUV_FULL : 0;
	j->background = 0x000000;
	j->timeout_ms = 250;	/* a job takes a few milliseconds */
}

static bool overlay_possible(const struct vo *vo)
{
	const struct pxp_priv *pp = vo->priv;
	const struct win *w = vo->win;
	int y0 = w->abs_y + vo->area.y;

	return pp->have_screen && w->visibility == VisibilityUnobscured &&
	       w->abs_x == 0 && w->w == (int)pp->scr.width &&
	       vo->area.x == 0 && vo->area.w == w->w &&
	       y0 >= 0 && y0 + vo->area.h <= (int)pp->scr.height &&
	       vo->area.h >= 8;
}

static int draw_overlay(struct vo *vo)
{
	struct pxp_priv *pp = vo->priv;
	struct pxp_job j;
	int dw, dh, e;

	vo_fit(pp->dar, vo->area.w, vo->area.h, 8, &dw, &dh);
	if (dw > 2040)
		dw = 2040;
	if (dh > 2040)
		dh = 2040;
	fill_job(pp, &j, dw, dh);
	j.out_index = PXP_BUF_SCREEN;
	j.out_width = pp->scr.width;
	j.out_height = vo->area.h;
	j.out_y = vo->win->abs_y + vo->area.y;
	j.dst_x = ((vo->area.w - dw) / 2) & ~7;
	j.dst_y = ((vo->area.h - dh) / 2) & ~7;
	e = pxpdev_run(pp->dev, &j);
	if (e < 0) {
		err("PXP: the screen overlay failed (%s): going through X", strerror(-e));
		pp->have_screen = false;
		return e;
	}
	vo->hw_ms_sum += j.elapsed_us / 1e3;
	vo->need_clear = false;		/* the PXP painted the whole area */
	if (!pp->was_overlay)
		verb("PXP: screen overlay, rows %u-%u", j.out_y, j.out_y + j.out_height - 1);
	pp->was_overlay = true;
	vo->path = "pxp_screen";
	return 0;
}

static int draw_shm(struct vo *vo)
{
	struct pxp_priv *pp = vo->priv;
	struct win *w = vo->win;
	struct pxp_job j;
	int dw, dh, e, y;

	if (vo->busy)
		return VO_DROPPED;	/* X has not finished with the last one */
	vo_fit(pp->dar, vo->area.w, vo->area.h, 8, &dw, &dh);
	if (dw > 2040)
		dw = 2040;
	if (dh > 2040)
		dh = 2040;
	if (!pp->img || pp->img->w != dw || pp->img->h != dh) {
		free_out(vo);
		pp->o_size = (size_t)dw * dh * 2;
		pp->o_map = pxpdev_alloc(pp->dev, &pp->o_size, &pp->o_index);
		pp->img = pp->o_map ? ximg_new(w, dw, dh) : NULL;
		if (!pp->img) {
			free_out(vo);
			return -1;
		}
		vo->need_clear = true;
	}
	fill_job(pp, &j, dw, dh);
	j.out_index = pp->o_index;
	j.out_width = dw;
	j.out_height = dh;
	e = pxpdev_run(pp->dev, &j);
	if (e < 0) {
		/* do not stall on every frame if the hardware is stuck */
		if (++pp->failures >= 3)
			err("PXP: job failed: %s; the picture is off", strerror(-e));
		return e;
	}
	pp->failures = 0;
	vo->hw_ms_sum += j.elapsed_us / 1e3;
	for (y = 0; y < dh; y++)
		memcpy(pp->img->px + (size_t)y * pp->img->stride,
		       pp->o_map + (size_t)y * dw * 2, (size_t)dw * 2);
	if (vo->need_clear || pp->was_overlay) {
		win_clear(w, vo->area.x, vo->area.y, vo->area.w, vo->area.h);
		vo->need_clear = false;
	}
	if (pp->was_overlay)
		verb("PXP: through XShm (no overlay: the window is covered or not full width)");
	pp->was_overlay = false;
	vo->busy = ximg_put(w, pp->img, 0, 0, vo->area.x + (vo->area.w - dw) / 2,
			    vo->area.y + (vo->area.h - dh) / 2, dw, dh, true);
	vo->path = "pxp_shm";
	return 0;
}

static int pxp_draw(struct vo *vo, const AVFrame *f, AVRational sar)
{
	struct pxp_priv *pp = vo->priv;
	struct win *w = vo->win;

	if (pp->failures >= 3)
		return -1;
	if (f && upload(vo, f, sar) < 0)
		return -1;
	if (!pp->have_frame || !w->mapped || w->visibility == VisibilityFullyObscured ||
	    vo->area.w < 8 || vo->area.h < 8)
		return VO_OK;
	if (overlay_possible(vo) && draw_overlay(vo) == 0)
		return VO_OK;
	return draw_shm(vo);
}

const struct vo_driver vo_pxp = {
	.name = "pxp",
	.init = pxp_init,
	.draw = pxp_draw,
	.uninit = pxp_uninit,
};
