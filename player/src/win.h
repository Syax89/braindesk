/* SPDX-License-Identifier: MIT */
/* The X11 window of the player and XShm images (RGB565 only). */
#ifndef BRAINPLAY_WIN_H
#define BRAINPLAY_WIN_H

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <stdbool.h>
#include <stdint.h>

struct options;

struct ximg {
	XImage *img;
	XShmSegmentInfo shm;
	bool is_shm;
	int w, h;
	uint16_t *px;
	int stride;		/* pixels per row */
};

struct win {
	Display *dpy;
	int screen;
	Window root, win;
	GC gc;
	int scr_w, scr_h;
	int w, h;		/* size of the window */
	int abs_x, abs_y;	/* where it is on the screen */
	bool mapped;
	int visibility;		/* VisibilityUnobscured / PartiallyObscured / FullyObscured */
	bool shm_ok;
	int shm_event;		/* event type of XShmCompletionEvent */
	bool fullscreen;
	Atom wm_protocols, wm_delete, net_wm_name, utf8_string;
};

int win_open(struct win *w, const struct options *opt, const char *title);
void win_set_title(struct win *w, const char *title);
void win_close(struct win *w);
/* after a ConfigureNotify: new size and position on the screen */
void win_configure(struct win *w, int width, int height);
void win_clear(struct win *w, int x, int y, int cw, int ch);	/* black */

struct ximg *ximg_new(struct win *w, int width, int height);
/* true if X will send a completion event for this put (shared memory) */
bool ximg_put(struct win *w, struct ximg *im, int sx, int sy, int dx, int dy,
	      int cw, int ch, bool want_event);
void ximg_free(struct win *w, struct ximg *im);

#endif
