// SPDX-License-Identifier: MIT
/*
 * The player's X11 window: full screen (EWMH), black, no cursor.
 * Only Xlib and XShm: no Xft/fontconfig, no toolkit.
 */
#include "win.h"
#include "brainplay.h"
#include "pxpdev.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

static int x_error_code;

static int on_x_error(Display *dpy, XErrorEvent *e)
{
	char text[96] = "";

	x_error_code = e->error_code;
	XGetErrorText(dpy, e->error_code, text, sizeof(text));
	dbg("X error: %s (request %d.%d)", text, e->request_code, e->minor_code);
	return 0;
}

static int on_x_io_error(Display *dpy)
{
	/* the X server is gone: nothing to clean up on the screen */
	err("lost the X server");
	_exit(1);
}

#ifdef PXP_SIM
static GC sim_gc;

/* the simulated panel: paint the rows over everything, as an overlay would */
static void sim_present(void *ctx, const uint16_t *rows, int y, int h)
{
	struct win *w = ctx;
	XImage *img = XCreateImage(w->dpy, DefaultVisual(w->dpy, w->screen), 16, ZPixmap, 0,
				   (char *)rows, w->scr_w, h, 16, w->scr_w * 2);

	if (!img)
		return;
	XPutImage(w->dpy, w->root, sim_gc, img, 0, 0, 0, y, w->scr_w, h);
	img->data = NULL;
	XDestroyImage(img);
}
#endif

int win_open(struct win *w, const struct options *opt, const char *title)
{
	static char *atom_names[] = {
		"WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_STATE",
		"_NET_WM_STATE_FULLSCREEN", "_MOTIF_WM_HINTS", "_NET_WM_NAME", "UTF8_STRING",
	};
	static const char no_bits[1] = { 0 };
	Atom atoms[7];
	XSetWindowAttributes at;
	XSizeHints *sh;
	XClassHint ch = { (char *)"brainplay", (char *)"Brainplay" };
	XColor black = { 0 };
	Pixmap blank;
	Visual *vis;
	int width, height;

	memset(w, 0, sizeof(*w));
	w->dpy = XOpenDisplay(NULL);
	if (!w->dpy)
		return -1;
	XSetErrorHandler(on_x_error);
	XSetIOErrorHandler(on_x_io_error);
	w->screen = DefaultScreen(w->dpy);
	w->root = RootWindow(w->dpy, w->screen);
	w->scr_w = DisplayWidth(w->dpy, w->screen);
	w->scr_h = DisplayHeight(w->dpy, w->screen);
	vis = DefaultVisual(w->dpy, w->screen);
	if (DefaultDepth(w->dpy, w->screen) != 16 || vis->red_mask != 0xF800 ||
	    vis->green_mask != 0x07E0 || vis->blue_mask != 0x001F) {
		err("the X screen is not 16 bit RGB565 (depth %d): no window",
		    DefaultDepth(w->dpy, w->screen));
		XCloseDisplay(w->dpy);
		w->dpy = NULL;
		return -1;
	}

	w->fullscreen = opt->win_w <= 0 || opt->win_h <= 0;
	width = w->fullscreen ? w->scr_w : opt->win_w;
	height = w->fullscreen ? w->scr_h : opt->win_h;

	at.background_pixel = BlackPixel(w->dpy, w->screen);
	at.border_pixel = 0;
	at.bit_gravity = NorthWestGravity;
	at.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
			ButtonMotionMask | StructureNotifyMask | VisibilityChangeMask;
	w->win = XCreateWindow(w->dpy, w->root, 0, 0, width, height, 0, CopyFromParent,
			       InputOutput, CopyFromParent,
			       CWBackPixel | CWBorderPixel | CWBitGravity | CWEventMask, &at);

	XInternAtoms(w->dpy, atom_names, 7, False, atoms);
	w->wm_protocols = atoms[0];
	w->wm_delete = atoms[1];
	w->net_wm_name = atoms[5];
	w->utf8_string = atoms[6];
	XSetWMProtocols(w->dpy, w->win, &w->wm_delete, 1);
	XSetClassHint(w->dpy, w->win, &ch);
	sh = XAllocSizeHints();
	if (sh) {
		sh->flags = PMinSize | (w->fullscreen ? PPosition | PSize : PSize);
		sh->width = width;
		sh->height = height;
		sh->min_width = 320;
		sh->min_height = 200;
		XSetWMNormalHints(w->dpy, w->win, sh);
		XFree(sh);
	}
	if (w->fullscreen) {
		/* MWM hints: flags = decorations, decorations = none */
		unsigned long mwm[5] = { 2, 0, 0, 0, 0 };

		XChangeProperty(w->dpy, w->win, atoms[2], XA_ATOM, 32, PropModeReplace,
				(unsigned char *)&atoms[3], 1);
		XChangeProperty(w->dpy, w->win, atoms[4], atoms[4], 32, PropModeReplace,
				(unsigned char *)mwm, 5);
	}
	win_set_title(w, title);

	/* no pointer over the picture (the screen is a touch screen) */
	blank = XCreateBitmapFromData(w->dpy, w->win, no_bits, 1, 1);
	XDefineCursor(w->dpy, w->win,
		      XCreatePixmapCursor(w->dpy, blank, blank, &black, &black, 0, 0));
	XFreePixmap(w->dpy, blank);

	w->gc = XCreateGC(w->dpy, w->win, 0, NULL);
	w->shm_ok = XShmQueryExtension(w->dpy);
	w->shm_event = XShmGetEventBase(w->dpy) + ShmCompletion;
	w->w = width;
	w->h = height;
	w->visibility = VisibilityFullyObscured;	/* until the server says otherwise */

#ifdef PXP_SIM
	if (opt->pxp_sim) {
		XGCValues gv = { .subwindow_mode = IncludeInferiors };

		sim_gc = XCreateGC(w->dpy, w->root, GCSubwindowMode, &gv);
		pxpsim_set_screen(w->scr_w, w->scr_h, sim_present, w);
	}
#endif
	XMapWindow(w->dpy, w->win);
	XFlush(w->dpy);
	return 0;
}

void win_set_title(struct win *w, const char *title)
{
	XStoreName(w->dpy, w->win, title);
	XChangeProperty(w->dpy, w->win, w->net_wm_name, w->utf8_string, 8, PropModeReplace,
			(const unsigned char *)title, (int)strlen(title));
}

void win_close(struct win *w)
{
	if (!w->dpy)
		return;
#ifdef PXP_SIM
	pxpsim_set_screen(0, 0, NULL, NULL);
#endif
	XDestroyWindow(w->dpy, w->win);
	XCloseDisplay(w->dpy);
	w->dpy = NULL;
}

void win_configure(struct win *w, int width, int height)
{
	Window child;
	int x = 0, y = 0;

	w->w = width;
	w->h = height;
	if (XTranslateCoordinates(w->dpy, w->win, w->root, 0, 0, &x, &y, &child)) {
		w->abs_x = x;
		w->abs_y = y;
	}
}

void win_clear(struct win *w, int x, int y, int cw, int ch)
{
	if (cw > 0 && ch > 0)
		XClearArea(w->dpy, w->win, x, y, cw, ch, False);
}

struct ximg *ximg_new(struct win *w, int width, int height)
{
	Visual *vis = DefaultVisual(w->dpy, w->screen);
	struct ximg *im = calloc(1, sizeof(*im));
	char *data;

	if (!im || width <= 0 || height <= 0) {
		free(im);
		return NULL;
	}
	im->w = width;
	im->h = height;
	if (w->shm_ok) {
		im->img = XShmCreateImage(w->dpy, vis, 16, ZPixmap, NULL, &im->shm, width, height);
		if (im->img) {
			void *addr = (void *)-1;

			im->shm.shmid = shmget(IPC_PRIVATE,
					       (size_t)im->img->bytes_per_line * height,
					       IPC_CREAT | 0600);
			if (im->shm.shmid >= 0)
				addr = shmat(im->shm.shmid, NULL, 0);
			if (addr != (void *)-1) {
				im->shm.shmaddr = im->img->data = addr;
				im->shm.readOnly = False;
				x_error_code = 0;
				XShmAttach(w->dpy, &im->shm);
				XSync(w->dpy, False);
				if (!x_error_code)
					im->is_shm = true;
				else
					shmdt(addr);
			}
			/* the segment goes away with its last user */
			if (im->shm.shmid >= 0)
				shmctl(im->shm.shmid, IPC_RMID, NULL);
			if (!im->is_shm) {
				im->img->data = NULL;
				XDestroyImage(im->img);
				im->img = NULL;
			}
		}
		if (!im->is_shm) {
			info("no X shared memory: using XPutImage");
			w->shm_ok = false;
		}
	}
	if (!im->img) {
		data = calloc((size_t)width * height, 2);
		if (data)
			im->img = XCreateImage(w->dpy, vis, 16, ZPixmap, 0, data, width, height,
					       16, width * 2);
		if (!im->img) {
			free(data);
			free(im);
			return NULL;
		}
	}
	im->px = (uint16_t *)im->img->data;
	im->stride = im->img->bytes_per_line / 2;
	return im;
}

bool ximg_put(struct win *w, struct ximg *im, int sx, int sy, int dx, int dy,
	      int cw, int ch, bool want_event)
{
	if (sx < 0 || sy < 0 || cw <= 0 || ch <= 0 || sx + cw > im->w || sy + ch > im->h)
		return false;
	if (im->is_shm) {
		XShmPutImage(w->dpy, w->win, w->gc, im->img, sx, sy, dx, dy, cw, ch, want_event);
		return want_event;
	}
	XPutImage(w->dpy, w->win, w->gc, im->img, sx, sy, dx, dy, cw, ch);
	return false;
}

void ximg_free(struct win *w, struct ximg *im)
{
	if (!im)
		return;
	if (im->is_shm) {
		XShmDetach(w->dpy, &im->shm);
		im->img->data = NULL;
		XDestroyImage(im->img);
		shmdt(im->shm.shmaddr);
	} else {
		XDestroyImage(im->img);		/* frees the pixels too */
	}
	free(im);
}
