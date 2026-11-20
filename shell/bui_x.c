// SPDX-License-Identifier: MIT
/*
 * bui_x.c - backend X11: finestre, immagini condivise (MIT-SHM), popup,
 * traduzione degli eventi. Timer, animazioni e ciclo sono in bui_core.c.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/shape.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#include "bui.h"
#include "bui_be.h"
#include "bui_x.h"

#define MAX_WINS 8

enum {
	A_WM_PROTOCOLS, A_WM_DELETE_WINDOW, A_NET_WM_NAME, A_UTF8_STRING,
	A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_DOCK, A_NET_WM_WINDOW_TYPE_DESKTOP,
	A_NET_WM_STATE, A_NET_WM_STATE_STICKY, A_NET_WM_STATE_SKIP_TASKBAR,
	A_NET_WM_STATE_SKIP_PAGER, A_NET_WM_STATE_ABOVE, A_NET_WM_STATE_BELOW,
	A_NET_WM_DESKTOP, A_NET_WM_STRUT, A_NET_WM_STRUT_PARTIAL, A_NET_WM_PID,
	A_MOTIF_WM_HINTS, A_COUNT
};
static const char *const atom_names[A_COUNT] = {
	"WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME", "UTF8_STRING",
	"_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP",
	"_NET_WM_STATE", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR",
	"_NET_WM_STATE_SKIP_PAGER", "_NET_WM_STATE_ABOVE", "_NET_WM_STATE_BELOW",
	"_NET_WM_DESKTOP", "_NET_WM_STRUT", "_NET_WM_STRUT_PARTIAL", "_NET_WM_PID",
	"_MOTIF_WM_HINTS",
};

static struct {
	Display *dpy;
	int scr;
	Window root;
	Visual *vis;
	int depth, sw, sh;
	int native;		/* il visual e' RGB565: la tela e' l'immagine stessa */
	int shm, shm_ev;
	int xerr;
	GC gc;
	Atom atom[A_COUNT];
	Time time;
	bui_win *wins[MAX_WINS];
	int nwins;
	int trace;		/* BUI_TRACE: elenca su stderr ogni rettangolo inviato */
	void (*hook)(XEvent *);
	int (*ext_hook)(XEvent *);
} X;

Display *bui_x_display(void) { return X.dpy; }
Window bui_x_root(void) { return X.root; }
Window bui_x_window(const bui_win *w) { return w ? w->xid : None; }
Time bui_x_time(void) { return X.time; }
void bui_x_set_hook(void (*hook)(XEvent *)) { X.hook = hook; }
void bui_x_set_ext_hook(int (*hook)(XEvent *)) { X.ext_hook = hook; }
static int x_screen_w(void) { return X.sw; }
static int x_screen_h(void) { return X.sh; }

int bui_x_error_pending(void)
{
	int e;

	XSync(X.dpy, False);
	e = X.xerr;
	X.xerr = 0;
	return e;
}

static int on_xerror(Display *d, XErrorEvent *e)
{
	(void)d;
	/* finestre altrui che spariscono, SHM non disponibile: mai fatale */
	X.xerr = e->error_code;
	if (getenv("BUI_DEBUG"))
		fprintf(stderr, "bui: errore X %d (richiesta %d.%d)\n", e->error_code,
			e->request_code, e->minor_code);
	return 0;
}

static int x_init(void)
{
	XGCValues gv;

	X.dpy = XOpenDisplay(NULL);
	if (!X.dpy)
		return -1;
	XSetErrorHandler(on_xerror);
	X.scr = DefaultScreen(X.dpy);
	X.root = RootWindow(X.dpy, X.scr);
	X.vis = DefaultVisual(X.dpy, X.scr);
	X.depth = DefaultDepth(X.dpy, X.scr);
	X.sw = DisplayWidth(X.dpy, X.scr);
	X.sh = DisplayHeight(X.dpy, X.scr);
	X.native = X.depth == 16 && X.vis->red_mask == 0xF800 && X.vis->green_mask == 0x07E0 &&
		   X.vis->blue_mask == 0x001F;
	X.shm = XShmQueryExtension(X.dpy) && !getenv("BUI_NO_SHM");
	X.trace = getenv("BUI_TRACE") != NULL;
	X.shm_ev = XShmGetEventBase(X.dpy) + ShmCompletion;
	gv.graphics_exposures = False;
	X.gc = XCreateGC(X.dpy, X.root, GCGraphicsExposures, &gv);
	XInternAtoms(X.dpy, (char **)atom_names, A_COUNT, False, X.atom);
	return 0;
}

/* ------------------------------------------------------------------ */
/* immagini                                                           */
/* ------------------------------------------------------------------ */

static XImage *img_create(int w, int h, XShmSegmentInfo **pshm)
{
	XImage *im;
	char *data;

	*pshm = NULL;
	if (X.shm) {
		XShmSegmentInfo *si = calloc(1, sizeof(*si));

		im = si ? XShmCreateImage(X.dpy, X.vis, X.depth, ZPixmap, NULL, si, w, h) : NULL;
		if (im) {
			si->shmid = shmget(IPC_PRIVATE, (size_t)im->bytes_per_line * im->height,
					   IPC_CREAT | 0600);
			if (si->shmid >= 0) {
				si->shmaddr = shmat(si->shmid, NULL, 0);
				if (si->shmaddr != (char *)-1) {
					im->data = si->shmaddr;
					si->readOnly = False;
					XSync(X.dpy, False);
					X.xerr = 0;
					XShmAttach(X.dpy, si);
					XSync(X.dpy, False);
					/* il segmento sparisce da solo alla chiusura */
					shmctl(si->shmid, IPC_RMID, NULL);
					if (!X.xerr) {
						*pshm = si;
						return im;
					}
					shmdt(si->shmaddr);
				} else {
					shmctl(si->shmid, IPC_RMID, NULL);
				}
			}
			im->data = NULL;
			XDestroyImage(im);
		}
		free(si);
		X.shm = 0;	/* display remoto: si ripiega su XPutImage */
		X.xerr = 0;
	}
	im = XCreateImage(X.dpy, X.vis, X.depth, ZPixmap, 0, NULL, w, h, 32, 0);
	if (!im)
		return NULL;
	data = calloc((size_t)im->bytes_per_line, h);
	if (!data) {
		XDestroyImage(im);
		return NULL;
	}
	im->data = data;
	return im;
}

static void img_destroy(XImage *im, XShmSegmentInfo *si)
{
	if (!im)
		return;
	if (si) {
		XShmDetach(X.dpy, si);
		im->data = NULL;
		XDestroyImage(im);
		shmdt(si->shmaddr);
		free(si);
	} else {
		XDestroyImage(im);	/* libera anche i dati */
	}
}

static int win_alloc(bui_win *w)
{
	XImage *im = img_create(w->w, w->h, (XShmSegmentInfo **)&w->shm);

	if (!im)
		return -1;
	if (X.native) {
		w->img = im;
		w->cv.px = (bui_px *)im->data;
		w->cv.stride = im->bytes_per_line / 2;
		w->own_px = 0;
	} else {
		/* visual diverso (es. 24 bit sul PC): tela a parte, convertita all'invio */
		w->img2 = im;
		w->shm2 = w->shm;
		w->shm = NULL;
		w->cv.px = calloc((size_t)w->w * w->h, sizeof(bui_px));
		w->cv.stride = w->w;
		w->own_px = 1;
		if (!w->cv.px)
			return -1;
	}
	w->cv.w = w->w;
	w->cv.h = w->h;
	w->cv.clip = bui_r(0, 0, w->w, w->h);
	return 0;
}

static void win_free(bui_win *w)
{
	if (w->put_pending)
		XSync(X.dpy, False);	/* il server ha finito di leggere il segmento */
	img_destroy(w->img, w->shm);
	img_destroy(w->img2, w->shm2);
	if (w->own_px)
		free(w->cv.px);
	w->img = w->img2 = w->shm = w->shm2 = NULL;
	w->cv.px = NULL;
	w->put_pending = 0;
}

static void win_put(bui_win *w, bui_rect r, int notify)
{
	XImage *im = w->img;
	XShmSegmentInfo *si = w->shm;

	if (!X.native) {
		int x, y;

		im = w->img2;
		si = w->shm2;
		if (im->bits_per_pixel != 32)
			return;
		for (y = r.y; y < r.y + r.h; y++) {
			const bui_px *s = w->cv.px + y * w->cv.stride;
			uint32_t *d = (uint32_t *)(void *)(im->data + y * im->bytes_per_line);

			for (x = r.x; x < r.x + r.w; x++) {
				unsigned p = s[x];
				unsigned rr = (p >> 11) & 31, gg = (p >> 5) & 63, bb = p & 31;

				d[x] = 0xFF000000u | ((rr << 3 | rr >> 2) << 16) |
				       ((gg << 2 | gg >> 4) << 8) | (bb << 3 | bb >> 2);
			}
		}
	}
	if (si) {
		XShmPutImage(X.dpy, w->xid, X.gc, im, r.x, r.y, r.x, r.y, r.w, r.h, notify);
		if (notify) {
			w->put_pending = 1;
			w->put_time = bui_now();
		}
	} else {
		XPutImage(X.dpy, w->xid, X.gc, im, r.x, r.y, r.x, r.y, r.w, r.h);
	}
}

/* ------------------------------------------------------------------ */
/* finestre                                                           */
/* ------------------------------------------------------------------ */

static bui_win *win_find(Window xid)
{
	int i;

	for (i = 0; i < X.nwins; i++)
		if (X.wins[i]->xid == xid)
			return X.wins[i];
	return NULL;
}

static void set_atoms(Window w, int prop, const int *ids, int n)
{
	Atom a[8];
	int i;

	for (i = 0; i < n; i++)
		a[i] = X.atom[ids[i]];
	XChangeProperty(X.dpy, w, X.atom[prop], XA_ATOM, 32, PropModeReplace, (unsigned char *)a, n);
}

static bui_win *x_win_create(int kind, bui_rect r, const char *name)
{
	XSetWindowAttributes a;
	unsigned long mask = CWBackPixmap | CWEventMask | CWBitGravity;
	bui_win *w;
	XClassHint ch = { (char *)"brainshell", (char *)"Brainshell" };
	XWMHints wh;
	XSizeHints sz;
	long pid = getpid();
	long all = 0xFFFFFFFFl;
	long motif[5] = { 2, 0, 0, 0, 0 };	/* nessuna decorazione */

	if (X.nwins >= MAX_WINS || r.w <= 0 || r.h <= 0)
		return NULL;
	w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;
	w->x = r.x;
	w->y = r.y;
	w->w = r.w;
	w->h = r.h;
	w->kind = kind;

	a.background_pixmap = None;	/* niente lampi: si vede cio' che c'era finche' non disegniamo */
	a.bit_gravity = NorthWestGravity;
	a.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask |
		       KeyPressMask | StructureNotifyMask;
	if (kind == BUI_WIN_POPUP) {
		a.override_redirect = True;
		a.save_under = True;
		mask |= CWOverrideRedirect | CWSaveUnder;
	}
	w->xid = XCreateWindow(X.dpy, X.root, r.x, r.y, r.w, r.h, 0, CopyFromParent, InputOutput,
			       CopyFromParent, mask, &a);
	if (win_alloc(w) < 0) {
		XDestroyWindow(X.dpy, w->xid);
		win_free(w);
		free(w);
		return NULL;
	}

	XStoreName(X.dpy, w->xid, name);
	XChangeProperty(X.dpy, w->xid, X.atom[A_NET_WM_NAME], X.atom[A_UTF8_STRING], 8, PropModeReplace,
			(const unsigned char *)name, (int)strlen(name));
	XSetClassHint(X.dpy, w->xid, &ch);
	XChangeProperty(X.dpy, w->xid, X.atom[A_NET_WM_PID], XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&pid, 1);
	XSetWMProtocols(X.dpy, w->xid, &X.atom[A_WM_DELETE_WINDOW], 1);

	if (kind == BUI_WIN_DOCK || kind == BUI_WIN_DESKTOP) {
		static const int dock_state[] = { A_NET_WM_STATE_STICKY, A_NET_WM_STATE_SKIP_TASKBAR,
						  A_NET_WM_STATE_SKIP_PAGER, A_NET_WM_STATE_ABOVE };
		static const int desk_state[] = { A_NET_WM_STATE_STICKY, A_NET_WM_STATE_SKIP_TASKBAR,
						  A_NET_WM_STATE_SKIP_PAGER, A_NET_WM_STATE_BELOW };
		int type = kind == BUI_WIN_DOCK ? A_NET_WM_WINDOW_TYPE_DOCK : A_NET_WM_WINDOW_TYPE_DESKTOP;

		set_atoms(w->xid, A_NET_WM_WINDOW_TYPE, &type, 1);
		set_atoms(w->xid, A_NET_WM_STATE, kind == BUI_WIN_DOCK ? dock_state : desk_state, 4);
		XChangeProperty(X.dpy, w->xid, X.atom[A_NET_WM_DESKTOP], XA_CARDINAL, 32,
				PropModeReplace, (unsigned char *)&all, 1);
		XChangeProperty(X.dpy, w->xid, X.atom[A_MOTIF_WM_HINTS], X.atom[A_MOTIF_WM_HINTS], 32,
				PropModeReplace, (unsigned char *)motif, 5);
		wh.flags = InputHint;
		wh.input = kind == BUI_WIN_DESKTOP;	/* la barra non prende mai il fuoco */
		XSetWMHints(X.dpy, w->xid, &wh);
		sz.flags = USPosition | USSize | PMinSize | PMaxSize;
		sz.x = r.x;
		sz.y = r.y;
		sz.width = sz.min_width = sz.max_width = r.w;
		sz.height = sz.min_height = sz.max_height = r.h;
		XSetWMNormalHints(X.dpy, w->xid, &sz);
	}

	X.wins[X.nwins++] = w;
	bui_win_damage_all(w);
	return w;
}

static void x_win_set_strut_top(bui_win *w, int h)
{
	long strut[12] = { 0 };

	strut[2] = h;		/* top */
	strut[8] = 0;		/* top_start_x */
	strut[9] = w->w - 1;	/* top_end_x */
	XChangeProperty(X.dpy, w->xid, X.atom[A_NET_WM_STRUT], XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)strut, 4);
	XChangeProperty(X.dpy, w->xid, X.atom[A_NET_WM_STRUT_PARTIAL], XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)strut, 12);
}

static void handle_event(XEvent *e);

static void grab(bui_win *w)
{
	XGrabPointer(X.dpy, w->xid, False, ButtonPressMask | ButtonReleaseMask | Button1MotionMask,
		     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
	XGrabKeyboard(X.dpy, w->xid, False, GrabModeAsync, GrabModeAsync, CurrentTime);
	w->grabbed = 1;
}

/*
 * Un popup appena tolto lascia sullo schermo i suoi pixel finche' chi sta
 * sotto non ridisegna. Le nostre finestre hanno gia' i pixel pronti: li
 * rimandiamo subito, cosi' un popup aperto subito dopo cattura un fondo pulito.
 */
static void repair_exposed(void)
{
	XEvent e;
	int i;

	XSync(X.dpy, False);
	while (XCheckTypedEvent(X.dpy, Expose, &e))
		handle_event(&e);
	for (i = 0; i < X.nwins; i++) {
		bui_win *w = X.wins[i];

		if (w->mapped && w->has_expose) {
			win_put(w, w->expose, 0);
			w->has_expose = 0;
		}
	}
}

static void x_win_destroy(bui_win *w)
{
	int i, popup, regrab;

	if (!w)
		return;
	for (i = 0; i < X.nwins; i++)
		if (X.wins[i] == w) {
			memmove(&X.wins[i], &X.wins[i + 1], sizeof(X.wins[0]) * (size_t)(X.nwins - i - 1));
			X.nwins--;
			break;
		}
	popup = w->kind == BUI_WIN_POPUP;
	regrab = w->grabbed;
	bui_timer_del(w->remap);
	if (w->grabbed) {
		XUngrabPointer(X.dpy, CurrentTime);
		XUngrabKeyboard(X.dpy, CurrentTime);
	}
	XDestroyWindow(X.dpy, w->xid);
	win_free(w);
	free(w->backdrop);
	free(w);
	if (!popup)
		return;
	bui__popup_closed_now();
	repair_exposed();
	/* se sotto c'era un altro popup modale, il tocco torna a lui */
	for (i = X.nwins - 1; regrab && i >= 0; i--)
		if (X.wins[i]->grabbed) {
			grab(X.wins[i]);
			break;
		}
}

static void x_win_show(bui_win *w)
{
	if (w->kind == BUI_WIN_POPUP) {
		XMapRaised(X.dpy, w->xid);
		w->mapped = 1;
		w->skip_expose = 1;
	} else {
		XMapWindow(X.dpy, w->xid);
		if (w->kind == BUI_WIN_DESKTOP)
			XLowerWindow(X.dpy, w->xid);
	}
}

static void x_win_hide(bui_win *w)
{
	XUnmapWindow(X.dpy, w->xid);
	w->mapped = 0;
}

static void win_resized(bui_win *w, int nw, int nh)
{
	if (nw == w->w && nh == w->h)
		return;
	win_free(w);
	w->w = nw;
	w->h = nh;
	w->ndirty = 0;
	w->has_expose = 0;
	if (win_alloc(w) < 0) {
		fprintf(stderr, "bui: memoria esaurita\n");
		exit(1);
	}
	if (w->resized)
		w->resized(w);
	bui_win_damage_all(w);
}

static void remap_timer(void *user)
{
	bui_win *w = user;
	XSizeHints sz;

	w->remap = 0;
	sz.flags = USPosition | USSize | PMinSize | PMaxSize;
	sz.x = w->want.x;
	sz.y = w->want.y;
	sz.width = sz.min_width = sz.max_width = w->want.w;
	sz.height = sz.min_height = sz.max_height = w->want.h;
	XSetWMNormalHints(X.dpy, w->xid, &sz);
	/* la tela si adegua quando arriva ConfigureNotify */
	XMoveResizeWindow(X.dpy, w->xid, w->want.x, w->want.y, w->want.w, w->want.h);
	x_win_show(w);
}

/*
 * Cambia posizione e dimensione di una finestra gestita. I gestori di
 * finestre ridimensionano malvolentieri dock e desktop gia' mappati: la
 * finestra viene ritirata, modificata mentre nessuno la gestisce e
 * rimappata. Raro (cambio di risoluzione), quindi il costo non conta.
 */
static void x_win_move_resize(bui_win *w, bui_rect r)
{
	if (w->kind == BUI_WIN_POPUP) {
		XMoveResizeWindow(X.dpy, w->xid, r.x, r.y, r.w, r.h);
		w->x = r.x;
		w->y = r.y;
		win_resized(w, r.w, r.h);
		return;
	}
	if (!w->remap && r.x == w->x && r.y == w->y && r.w == w->w && r.h == w->h)
		return;
	w->want = r;
	w->x = r.x;
	w->y = r.y;
	XWithdrawWindow(X.dpy, w->xid, X.scr);
	w->mapped = 0;
	if (!w->remap)
		w->remap = bui_timer_add(150, 0, remap_timer, w);
}

static void win_flush(bui_win *w)
{
	int i, covered = 0;
	int n = w->ndirty;
	bui_rect rects[BUI_MAX_DIRTY];

	memcpy(rects, w->dirty, sizeof(rects));
	w->ndirty = 0;
	for (i = 0; i < n; i++) {
		w->cv.clip = rects[i];
		if (w->paint)
			w->paint(w, &w->cv, rects[i]);
	}
	w->cv.clip = bui_r(0, 0, w->w, w->h);
	if (w->has_expose) {
		/* un'esposizione chiede solo di rimandare pixel gia' pronti */
		for (i = 0; i < n; i++) {
			bui_rect k;

			if (bui_rect_isect(rects[i], w->expose, &k) && k.w == w->expose.w &&
			    k.h == w->expose.h)
				covered = 1;
		}
		if (!covered && n < BUI_MAX_DIRTY)
			rects[n++] = w->expose;
		else if (!covered)
			rects[0] = bui_rect_union(rects[0], w->expose);
		w->has_expose = 0;
	}
	for (i = 0; i < n; i++) {
		win_put(w, rects[i], i == n - 1);
		if (X.trace)
			fprintf(stderr, "bui: put %lu %s %dx%d+%d+%d\n", (unsigned long)(bui_now() % 100000),
				w->kind == BUI_WIN_DOCK ? "bar" : w->kind == BUI_WIN_DESKTOP ? "home" : "popup",
				rects[i].w, rects[i].h, rects[i].x, rects[i].y);
	}
}

/* ritorna i ms dopo i quali riprovare (-1: niente in sospeso) */
static int flush_all(uint32_t now)
{
	int i, wait = -1;

	for (i = 0; i < X.nwins; i++) {
		bui_win *w = X.wins[i];
		int el, d;

		if (!w->mapped || (!w->ndirty && !w->has_expose))
			continue;
		if (w->put_pending) {
			/* il server sta ancora copiando il fotogramma precedente */
			el = (int)(now - w->put_time);
			if (el < 250) {
				d = 250 - el;
				if (wait < 0 || d < wait)
					wait = d;
				continue;
			}
			w->put_pending = 0;
		}
		el = (int)(now - w->last_flush);
		if (el >= 0 && el < BUI_FRAME_MS - 3) {
			d = BUI_FRAME_MS - 3 - el;
			if (wait < 0 || d < wait)
				wait = d;
			continue;
		}
		win_flush(w);
		w->last_flush = now ? now : 1;
	}
	return wait;
}

/* ------------------------------------------------------------------ */
/* popup                                                              */
/* ------------------------------------------------------------------ */

static void popup_capture(bui_win *w, unsigned flags)
{
	int y;
	XImage *g = NULL;

	if (!X.native || w->x < 0 || w->y < 0 || w->x + w->w > X.sw || w->y + w->h > X.sh)
		return;
	w->backdrop = malloc((size_t)w->w * w->h * sizeof(bui_px));
	if (!w->backdrop)
		return;
	XSync(X.dpy, False);
	X.xerr = 0;
	if (w->shm) {
		XShmGetImage(X.dpy, X.root, w->img, w->x, w->y, AllPlanes);
		XSync(X.dpy, False);
		if (!X.xerr)
			for (y = 0; y < w->h; y++)
				memcpy(w->backdrop + y * w->w, w->cv.px + y * w->cv.stride,
				       (size_t)w->w * sizeof(bui_px));
	} else {
		g = XGetImage(X.dpy, X.root, w->x, w->y, w->w, w->h, AllPlanes, ZPixmap);
		if (g && !X.xerr && g->bits_per_pixel == 16)
			for (y = 0; y < w->h; y++)
				memcpy(w->backdrop + y * w->w, g->data + y * g->bytes_per_line,
				       (size_t)w->w * sizeof(bui_px));
		else
			X.xerr = 1;
		if (g)
			XDestroyImage(g);
	}
	if (X.xerr) {
		X.xerr = 0;
		free(w->backdrop);
		w->backdrop = NULL;
		return;
	}
	if (flags & BUI_POPUP_DIM)
		bui__dim(w->backdrop, w->w, w->h, w->w);
}

static bui_win *x_popup_open(bui_rect r, unsigned flags)
{
	bui_win *w;

	/* resta dentro lo schermo */
	if (r.w > X.sw)
		r.w = X.sw;
	if (r.h > X.sh)
		r.h = X.sh;
	if (r.x + r.w > X.sw)
		r.x = X.sw - r.w;
	if (r.y + r.h > X.sh)
		r.y = X.sh - r.h;
	if (r.x < 0)
		r.x = 0;
	if (r.y < 0)
		r.y = 0;
	w = x_win_create(BUI_WIN_POPUP, r, "brainshell-popup");
	if (!w)
		return NULL;
	popup_capture(w, flags);
	x_win_show(w);
	if (flags & BUI_POPUP_MODAL)
		grab(w);
	return w;
}

static bui_win *x_popup_open_round(bui_rect r, int radius)
{
	XRectangle rc[2 * 64 + 1];
	bui_win *w = x_win_create(BUI_WIN_POPUP, r, "brainshell-popup");
	int n = 0, j, ev, err;

	if (!w)
		return NULL;
	if (radius > r.h / 2)
		radius = r.h / 2;
	if (radius > r.w / 2)
		radius = r.w / 2;
	if (radius > 64)
		radius = 64;
	if (radius >= 2 && XShapeQueryExtension(X.dpy, &ev, &err)) {
		for (j = 0; j < radius; j++) {
			int in = bui_corner_inset(radius, j);

			rc[n].x = (short)in;
			rc[n].y = (short)j;
			rc[n].width = (unsigned short)(r.w - 2 * in);
			rc[n++].height = 1;
		}
		if (r.h > 2 * radius) {
			rc[n].x = 0;
			rc[n].y = (short)radius;
			rc[n].width = (unsigned short)r.w;
			rc[n++].height = (unsigned short)(r.h - 2 * radius);
		}
		for (j = radius - 1; j >= 0; j--) {
			int in = bui_corner_inset(radius, j);

			rc[n].x = (short)in;
			rc[n].y = (short)(r.h - 1 - j);
			rc[n].width = (unsigned short)(r.w - 2 * in);
			rc[n++].height = 1;
		}
		XShapeCombineRectangles(X.dpy, w->xid, ShapeBounding, 0, 0, rc, n, ShapeSet, Unsorted);
	}
	x_win_show(w);
	return w;
}

/* ------------------------------------------------------------------ */
/* eventi                                                             */
/* ------------------------------------------------------------------ */

static int map_key(XKeyEvent *e, bui_key *k)
{
	char buf[16];
	KeySym ks = NoSymbol;
	int n = XLookupString(e, buf, sizeof(buf), &ks, NULL);

	k->mods = 0;
	if (e->state & ShiftMask)
		k->mods |= BUI_MOD_SHIFT;
	if (e->state & ControlMask)
		k->mods |= BUI_MOD_CTRL;
	if (e->state & Mod1Mask)
		k->mods |= BUI_MOD_ALT;
	switch (ks) {
	case XK_Up: case XK_KP_Up: k->key = BUI_K_UP; return 1;
	case XK_Down: case XK_KP_Down: k->key = BUI_K_DOWN; return 1;
	case XK_Left: case XK_KP_Left: k->key = BUI_K_LEFT; return 1;
	case XK_Right: case XK_KP_Right: k->key = BUI_K_RIGHT; return 1;
	case XK_Return: case XK_KP_Enter: k->key = BUI_K_ENTER; return 1;
	case XK_Escape: k->key = BUI_K_ESC; return 1;
	case XK_BackSpace: k->key = BUI_K_BACKSPACE; return 1;
	case XK_Tab: case XK_ISO_Left_Tab: k->key = BUI_K_TAB; return 1;
	case XK_Delete: case XK_KP_Delete: k->key = BUI_K_DELETE; return 1;
	case XK_Prior: case XK_KP_Prior: k->key = BUI_K_PGUP; return 1;
	case XK_Next: case XK_KP_Next: k->key = BUI_K_PGDN; return 1;
	case XK_Home: case XK_KP_Home: k->key = BUI_K_HOME; return 1;
	case XK_End: case XK_KP_End: k->key = BUI_K_END; return 1;
	case XF86XK_HomePage: k->key = BUI_K_HOMEPAGE; return 1;
	default:
		break;
	}
	if (n == 1 && (unsigned char)buf[0] >= 0x20 && (unsigned char)buf[0] != 0x7F) {
		k->key = (unsigned char)buf[0];
		return 1;
	}
	/* fuori dal Latin-1 XLookupString non rende nulla: il simbolo dell'euro e
	 * i keysym Unicode (0x01000000 + codice) arrivano col loro codice */
	if (n == 0 && ks == 0x20AC) {
		k->key = 0x20AC;
		return 1;
	}
	if (n == 0 && (ks & 0xFFFF0000UL) == 0x01000000UL && (ks & 0xFFFF) >= 0xA0) {
		k->key = (int)(ks & 0xFFFF);
		return 1;
	}
	return 0;
}

static void handle_event(XEvent *e)
{
	bui_win *w;
	bui_pointer p;
	bui_key k;

	if (e->type == X.shm_ev) {
		w = win_find(((XShmCompletionEvent *)e)->drawable);
		if (w)
			w->put_pending = 0;
		return;
	}
	if (e->type == ConfigureNotify && e->xconfigure.window == X.root) {
		X.sw = e->xconfigure.width;
		X.sh = e->xconfigure.height;
	}
	if (e->type == PropertyNotify)
		X.time = e->xproperty.time;
	w = win_find(e->xany.window);
	if (!w) {
		if (X.ext_hook && X.ext_hook(e))
			return;
		if (X.hook)
			X.hook(e);
		return;
	}
	/* il filtro puo' scartare tocchi e tasti (schermo spento: il primo sveglia soltanto) */
	if ((e->type == ButtonPress || e->type == KeyPress || e->type == MotionNotify ||
	     e->type == ButtonRelease) &&
	    bui__input_filter(e->type == ButtonPress ? BUI_PRESS : e->type == MotionNotify ? BUI_MOVE :
			      e->type == ButtonRelease ? BUI_RELEASE : BUI_IN_KEY,
			      e->type == KeyPress && map_key(&e->xkey, &k) ? &k : NULL, 0)) {
		if (e->type == ButtonPress && w->down) {
			memset(&p, 0, sizeof(p));
			p.type = BUI_CANCEL;
			w->down = 0;
			if (w->pointer)
				w->pointer(w, &p);
		}
		return;
	}
	memset(&p, 0, sizeof(p));
	switch (e->type) {
	case Expose:
		/* un popup appena mappato e gia' disegnato non ha perso nulla */
		if (w->skip_expose) {
			if (e->xexpose.count == 0)
				w->skip_expose = 0;
			if (w->last_flush)
				break;
		}
		w->expose = w->has_expose ? bui_rect_union(w->expose,
			bui_r(e->xexpose.x, e->xexpose.y, e->xexpose.width, e->xexpose.height))
			: bui_r(e->xexpose.x, e->xexpose.y, e->xexpose.width, e->xexpose.height);
		bui_rect_isect(w->expose, bui_r(0, 0, w->w, w->h), &w->expose);
		w->has_expose = w->expose.w > 0;
		break;
	case MapNotify:
		w->mapped = 1;
		break;
	case UnmapNotify:
		w->mapped = 0;
		w->put_pending = 0;
		break;
	case ConfigureNotify:
		if (w->kind != BUI_WIN_POPUP) {
			Window child;
			int rx = w->x, ry = w->y;

			/* dove l'ha messa davvero il gestore di finestre? */
			XTranslateCoordinates(X.dpy, w->xid, X.root, 0, 0, &rx, &ry, &child);
			if (e->xconfigure.width != w->w || e->xconfigure.height != w->h) {
				w->x = rx;
				w->y = ry;
				win_resized(w, e->xconfigure.width, e->xconfigure.height);
			} else if (rx != w->x || ry != w->y) {
				w->x = rx;
				w->y = ry;
				if (w->resized)
					w->resized(w);
				bui_win_damage_all(w);
			}
		}
		break;
	case ButtonPress:
		X.time = e->xbutton.time;
		p.x = e->xbutton.x;
		p.y = e->xbutton.y;
		p.time = (uint32_t)e->xbutton.time;
		if (e->xbutton.button == Button1) {
			if (w->down) {	/* rilascio perso: chiude il tocco precedente */
				p.type = BUI_CANCEL;
				if (w->pointer)
					w->pointer(w, &p);
			}
			w->down = 1;
			p.type = BUI_PRESS;
		} else if (e->xbutton.button == Button4 || e->xbutton.button == Button5) {
			p.type = BUI_WHEEL;
			p.dy = e->xbutton.button == Button4 ? -1 : 1;
		} else {
			break;
		}
		if (w->pointer)
			w->pointer(w, &p);
		break;
	case MotionNotify:
		/* tiene solo l'ultimo di una raffica di movimenti consecutivi */
		while (XEventsQueued(X.dpy, QueuedAlready) > 0) {
			XEvent n;

			XPeekEvent(X.dpy, &n);
			if (n.type != MotionNotify || n.xmotion.window != e->xmotion.window)
				break;
			XNextEvent(X.dpy, e);
		}
		X.time = e->xmotion.time;
		if (!w->down)
			break;
		p.type = BUI_MOVE;
		p.x = e->xmotion.x;
		p.y = e->xmotion.y;
		p.time = (uint32_t)e->xmotion.time;
		if (w->pointer)
			w->pointer(w, &p);
		break;
	case ButtonRelease:
		X.time = e->xbutton.time;
		if (e->xbutton.button != Button1 || !w->down)
			break;
		w->down = 0;
		p.type = BUI_RELEASE;
		p.x = e->xbutton.x;
		p.y = e->xbutton.y;
		p.time = (uint32_t)e->xbutton.time;
		if (w->pointer)
			w->pointer(w, &p);
		break;
	case KeyPress:
		X.time = e->xkey.time;
		if (map_key(&e->xkey, &k) && w->key)
			w->key(w, &k);
		break;
	default:
		break;
	}
}

static void x_sync(void)
{
	XSync(X.dpy, False);
}

static void x_shutdown(void)
{
	if (!X.dpy)
		return;
	while (X.nwins)
		x_win_destroy(X.wins[X.nwins - 1]);
	XFreeGC(X.dpy, X.gc);
	XCloseDisplay(X.dpy);
	X.dpy = NULL;
}

static void x_pump(void)
{
	while (XPending(X.dpy)) {
		XEvent e;

		XNextEvent(X.dpy, &e);
		handle_event(&e);
	}
}

static int x_pending(void)
{
	return XEventsQueued(X.dpy, QueuedAlready) > 0;
}

static int x_flush(uint32_t now)
{
	int wait = flush_all(now);

	XFlush(X.dpy);
	return wait;
}

static int x_conn_fd(void)
{
	return ConnectionNumber(X.dpy);
}

static void x_redraw_all(void)
{
	int i;

	for (i = 0; i < X.nwins; i++)
		bui_win_damage_all(X.wins[i]);
}

const bui_backend bui_be_x11 = {
	.name = "x11",
	.init = x_init,
	.shutdown = x_shutdown,
	.screen_w = x_screen_w,
	.screen_h = x_screen_h,
	.win_create = x_win_create,
	.win_destroy = x_win_destroy,
	.win_show = x_win_show,
	.win_hide = x_win_hide,
	.win_move_resize = x_win_move_resize,
	.win_set_strut_top = x_win_set_strut_top,
	.popup_open = x_popup_open,
	.popup_open_round = x_popup_open_round,
	.pump = x_pump,
	.pending = x_pending,
	.flush = x_flush,
	.conn_fd = x_conn_fd,
	.sync = x_sync,
	.suspend = NULL,
	.redraw_all = x_redraw_all,
};
