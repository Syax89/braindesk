// SPDX-License-Identifier: MIT
/* Video outputs: selection and what they have in common. */
#include "vo.h"
#include "brainplay.h"

#include <stdlib.h>
#include <string.h>

#include <libavutil/pixdesc.h>

struct vo *vo_open(const char *name, struct win *win, const struct options *opt)
{
	static const struct vo_driver *const drivers[] = { &vo_pxp, &vo_x11 };
	bool any = !strcmp(name, "auto");
	unsigned i;

	if (!strcmp(name, "null"))
		return NULL;
	for (i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
		struct vo *vo;

		/* "pxp-shm" is the pxp driver told not to use the overlay */
		if (!any && strncmp(name, drivers[i]->name, strlen(drivers[i]->name)))
			continue;
		vo = calloc(1, sizeof(*vo));
		if (!vo)
			return NULL;
		vo->drv = drivers[i];
		vo->win = win;
		vo->area = (struct rect){ 0, 0, win->w, win->h };
		vo->path = drivers[i]->name;
		if (vo->drv->init(vo, opt) == 0)
			return vo;
		free(vo);
		if (!any)
			err("video output %s is not available", name);
	}
	return NULL;
}

void vo_close(struct vo *vo)
{
	if (!vo)
		return;
	if (vo->n && g_loglevel >= LOG_VERB)
		info("video output %s: %d frames, %.1f ms each (max %.1f ms)%s", vo->path, vo->n,
		     vo->ms_sum / vo->n, vo->ms_max,
		     vo->hw_ms_sum > 0 ? ", PXP included" : "");
	vo->drv->uninit(vo);
	free(vo);
}

int vo_draw(struct vo *vo, const AVFrame *f, AVRational sar)
{
	double t0 = now_sec(), ms;
	int r = vo->drv->draw(vo, f, sar);

	if (r == VO_OK && f) {
		ms = (now_sec() - t0) * 1e3;
		vo->n++;
		vo->ms_sum += ms;
		if (ms > vo->ms_max)
			vo->ms_max = ms;
	}
	return r;
}

void vo_event(struct vo *vo, enum vo_event ev)
{
	AVRational none = { 0, 1 };

	switch (ev) {
	case VO_SHM_DONE:
		vo->busy = false;
		if (!vo->redraw)
			break;
		/* fall through */
	case VO_AREA:
	case VO_EXPOSE:
		if (ev == VO_AREA)
			vo->need_clear = true;
		/* X may still be busy with the last image: then do it afterwards */
		vo->redraw = vo->drv->draw(vo, NULL, none) == VO_DROPPED;
		break;
	}
}

void vo_set_area(struct vo *vo, struct rect area)
{
	if (!memcmp(&vo->area, &area, sizeof(area)))
		return;
	vo->area = area;
	vo_event(vo, VO_AREA);
}

AVRational vo_dar(int w, int h, AVRational sar)
{
	AVRational dar;

	if (sar.num <= 0 || sar.den <= 0)
		sar = (AVRational){ 1, 1 };
	av_reduce(&dar.num, &dar.den, (int64_t)w * sar.num, (int64_t)h * sar.den, 1 << 20);
	if (dar.num <= 0 || dar.den <= 0)
		dar = (AVRational){ 16, 9 };
	return dar;
}

void vo_fit(AVRational dar, int aw, int ah, int align, int *w, int *h)
{
	int64_t fw = aw, fh = (int64_t)aw * dar.den / dar.num;

	if (fh > ah) {
		fh = ah;
		fw = (int64_t)ah * dar.num / dar.den;
	}
	fw -= fw % align;
	fh -= fh % align;
	*w = fw < align ? align : (int)fw;
	*h = fh < align ? align : (int)fh;
}

bool vo_format_ok(const AVFrame *f, int *csx, int *csy, bool *grey, bool *full_range)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);

	if (!d || d->comp[0].depth != 8 ||
	    (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL |
			 AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_ALPHA)))
		return false;
	*grey = d->nb_components == 1;
	if (!*grey && (d->nb_components != 3 || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) ||
		       d->comp[1].plane != 1 || d->comp[2].plane != 2 || d->comp[1].step != 1))
		return false;
	*csx = d->log2_chroma_w;
	*csy = d->log2_chroma_h;
	*full_range = f->color_range == AVCOL_RANGE_JPEG ||
		      f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P ||
		      f->format == AV_PIX_FMT_YUVJ444P || f->format == AV_PIX_FMT_YUVJ440P ||
		      f->format == AV_PIX_FMT_YUVJ411P;
	return true;
}
