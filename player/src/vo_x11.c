// SPDX-License-Identifier: MIT
/*
 * Software video output: integer YUV -> RGB565 conversion with
 * nearest-neighbour scaling straight into an XShm image.
 * Used when there is no /dev/pxp, i.e. for tests off the Brain.
 */
#include "brainplay.h"
#include "vo.h"
#include "yuv2rgb.h"

#include <stdlib.h>

struct x11_priv {
	AVFrame *last;		/* reference to the frame on screen, for redraws */
	AVRational sar;
	struct ximg *img;
	bool bad_format;
};

static int x11_init(struct vo *vo, const struct options *opt)
{
	struct x11_priv *xp = calloc(1, sizeof(*xp));

	if (!xp || !(xp->last = av_frame_alloc())) {
		free(xp);
		return -1;
	}
	vo->priv = xp;
	vo->path = "x11";
	verb("video output: software conversion to X11");
	return 0;
}

static void x11_uninit(struct vo *vo)
{
	struct x11_priv *xp = vo->priv;

	ximg_free(vo->win, xp->img);
	av_frame_free(&xp->last);
	free(xp);
}

static int x11_draw(struct vo *vo, const AVFrame *f, AVRational sar)
{
	struct x11_priv *xp = vo->priv;
	struct win *w = vo->win;
	struct yuv_src src = { 0 };
	int csx = 0, csy = 0, dw, dh;
	bool grey = false, full = false;

	if (f) {
		av_frame_unref(xp->last);
		if (!vo_format_ok(f, &csx, &csy, &grey, &full)) {
			if (!xp->bad_format)
				err("cannot show this picture format (%s)",
				    av_get_pix_fmt_name(f->format));
			xp->bad_format = true;
			return -1;
		}
		xp->bad_format = false;
		if (av_frame_ref(xp->last, f) < 0)
			return -1;
		xp->sar = sar;
	}
	f = xp->last;
	if (!f->data[0] || !w->mapped || w->visibility == VisibilityFullyObscured ||
	    vo->area.w < 2 || vo->area.h < 2)
		return VO_OK;
	if (vo->busy)
		return VO_DROPPED;	/* X has not finished with the last one */
	if (!vo_format_ok(f, &csx, &csy, &grey, &full))
		return -1;

	vo_fit(vo_dar(f->width, f->height, xp->sar), vo->area.w, vo->area.h, 1, &dw, &dh);
	if (!xp->img || xp->img->w != dw || xp->img->h != dh) {
		ximg_free(w, xp->img);
		xp->img = ximg_new(w, dw, dh);
		if (!xp->img)
			return -1;
		vo->need_clear = true;
	}
	src.y = f->data[0];
	src.ystride = f->linesize[0];
	if (!grey) {
		src.u = f->data[1];
		src.v = f->data[2];
		src.cstride = f->linesize[1];
	}
	src.csx = csx;
	src.csy = csy;
	src.full_range = full;
	yuv_to_rgb565(&src, 0, 0, f->width, f->height, xp->img->px, xp->img->stride, dw, dh);

	if (vo->need_clear) {
		win_clear(w, vo->area.x, vo->area.y, vo->area.w, vo->area.h);
		vo->need_clear = false;
	}
	vo->busy = ximg_put(w, xp->img, 0, 0, vo->area.x + (vo->area.w - dw) / 2,
			    vo->area.y + (vo->area.h - dh) / 2, dw, dh, true);
	return VO_OK;
}

const struct vo_driver vo_x11 = {
	.name = "x11",
	.init = x11_init,
	.draw = x11_draw,
	.uninit = x11_uninit,
};
