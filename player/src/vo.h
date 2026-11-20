/* SPDX-License-Identifier: MIT */
/*
 * Video outputs.
 *
 *   pxp      hardware colour conversion + scaling by the PXP:
 *            - "screen": the PXP writes straight into the frame buffer the
 *              panel shows, in the rows of the video area (overlay; nothing
 *              is copied by the CPU). Needs a full-width, unobscured window.
 *            - "shm": the PXP writes into its own buffer, which is copied
 *              to an XShm image. Used with kernels that have no overlay and
 *              whenever the overlay conditions do not hold.
 *   x11      software conversion + nearest-neighbour scaling, XShm.
 *            For machines without /dev/pxp (tests under Xvfb).
 *
 * All calls come from the main thread.
 */
#ifndef BRAINPLAY_VO_H
#define BRAINPLAY_VO_H

#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>

#include "canvas.h"
#include "win.h"

struct options;
struct vo;

enum vo_event {
	VO_AREA,	/* the video area moved or changed size */
	VO_EXPOSE,	/* X repainted (part of) the video area */
	VO_SHM_DONE,	/* X finished copying the last image */
};

enum { VO_OK, VO_DROPPED };

struct vo_driver {
	const char *name;
	int (*init)(struct vo *vo, const struct options *opt);
	/* shows a frame now; f == NULL shows the last one again. <0 = error */
	int (*draw)(struct vo *vo, const AVFrame *f, AVRational sar);
	void (*uninit)(struct vo *vo);
};

struct vo {
	const struct vo_driver *drv;
	struct win *win;
	void *priv;
	struct rect area;	/* video area inside the window */
	bool busy;		/* X is still copying the last image */
	bool need_clear;	/* the area has to be painted black again */
	bool redraw;		/* a redraw is waiting for X to finish */
	const char *path;	/* how the last frame was shown (for the log) */
	/* time spent showing frames */
	int n;
	double ms_sum, ms_max, hw_ms_sum;
};

extern const struct vo_driver vo_pxp, vo_x11;

/* name: auto, pxp, pxp-shm, x11, null (returns NULL) */
struct vo *vo_open(const char *name, struct win *win, const struct options *opt);
void vo_close(struct vo *vo);
int vo_draw(struct vo *vo, const AVFrame *f, AVRational sar);
void vo_event(struct vo *vo, enum vo_event ev);
void vo_set_area(struct vo *vo, struct rect area);

/* largest w x h with the aspect dar that fits aw x ah; multiples of "align" */
void vo_fit(AVRational dar, int aw, int ah, int align, int *w, int *h);
AVRational vo_dar(int w, int h, AVRational sar);
/* is this an 8 bit planar YUV (or grey) frame we can show? */
bool vo_format_ok(const AVFrame *f, int *csx, int *csy, bool *grey, bool *full_range);

#endif
