// SPDX-License-Identifier: GPL-2.0
/*
 * pxpwin: lettore video in una finestra del desktop del Brain.
 *
 * Ogni fotogramma YUV420 viene convertito in RGB565 e scalato alla
 * dimensione della finestra dal PXP (hardware); il risultato passa a X
 * con la memoria condivisa (XShm), senza conversioni fatte dalla CPU.
 *
 *   pxpwin [-t titolo] file.yuv|- larghezza altezza fps
 *
 * fps = 0: nessun ritmo imposto (lo da' chi scrive, es. ffmpeg con l'audio).
 * Tasti: q / Esc chiude, f alterna finestra e schermo intero.
 */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

#include "brain_pxp_uapi.h"

struct out {
	int w, h;			/* multipli di 8 */
	struct pxp_alloc buf;
	void *map;
	XImage *img;
	XShmSegmentInfo shm;
};

static int pxp;
static Display *dpy;
static Window win;
static GC gc;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void out_free(struct out *o)
{
	if (o->img) {
		XShmDetach(dpy, &o->shm);
		XDestroyImage(o->img);
		shmdt(o->shm.shmaddr);
		o->img = NULL;
	}
	if (o->map) {
		munmap(o->map, o->buf.size);
		ioctl(pxp, PXP_IOC_FREE, &o->buf.index);
		o->map = NULL;
	}
}

/* buffer di uscita del PXP + immagine condivisa con X della stessa misura */
static int out_alloc(struct out *o, int w, int h)
{
	int scr = DefaultScreen(dpy);

	o->w = w & ~7;
	o->h = h & ~7;
	if (o->w < 8 || o->h < 8)
		return -1;
	o->buf.size = o->w * o->h * 2;
	if (ioctl(pxp, PXP_IOC_ALLOC, &o->buf))
		return perror("PXP_IOC_ALLOC"), -1;
	o->map = mmap(NULL, o->buf.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		      pxp, o->buf.mmap_offset);
	if (o->map == MAP_FAILED) {
		o->map = NULL;
		return perror("mmap"), -1;
	}
	o->img = XShmCreateImage(dpy, DefaultVisual(dpy, scr), 16, ZPixmap,
				 NULL, &o->shm, o->w, o->h);
	if (!o->img)
		return fprintf(stderr, "XShm non disponibile\n"), -1;
	o->shm.shmid = shmget(IPC_PRIVATE, o->img->bytes_per_line * o->h,
			      IPC_CREAT | 0600);
	o->shm.shmaddr = o->img->data = shmat(o->shm.shmid, NULL, 0);
	o->shm.readOnly = False;
	XShmAttach(dpy, &o->shm);
	XSync(dpy, False);
	shmctl(o->shm.shmid, IPC_RMID, NULL);
	return 0;
}

static void set_fullscreen(int on)
{
	Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
	Atom fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	XEvent e = { 0 };

	e.xclient.type = ClientMessage;
	e.xclient.window = win;
	e.xclient.message_type = state;
	e.xclient.format = 32;
	e.xclient.data.l[0] = on ? 1 : 0;
	e.xclient.data.l[1] = fs;
	XSendEvent(dpy, DefaultRootWindow(dpy), False,
		   SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

int main(int argc, char **argv)
{
	const char *title = "Video (PXP)";
	struct pxp_alloc src = { 0 };
	struct out o = { 0 };
	uint8_t *srcmap;
	int a = 1, vid, sw, sh, fps, ww, wh, fullscreen = 0, frames = 0;
	size_t fsize;
	double t_next, t_stat, pxp_ms = 0, put_ms = 0;
	Atom wm_delete;
	XSizeHints hints = { 0 };

	if (a + 1 < argc && !strcmp(argv[a], "-t")) {
		title = argv[a + 1];
		a += 2;
	}
	if (argc - a != 4) {
		fprintf(stderr, "uso: %s [-t titolo] file.yuv|- larghezza altezza fps\n", argv[0]);
		return 1;
	}
	sw = atoi(argv[a + 1]);
	sh = atoi(argv[a + 2]);
	fps = atoi(argv[a + 3]);
	if (sw <= 0 || sh <= 0 || sw % 8 || sh % 8 || fps < 0)
		return fprintf(stderr, "dimensioni a multipli di 8\n"), 1;
	fsize = (size_t)sw * sh * 3 / 2;

	vid = strcmp(argv[a], "-") ? open(argv[a], O_RDONLY) : 0;
	if (vid < 0)
		return perror(argv[a]), 1;
	pxp = open("/dev/pxp", O_RDWR | O_CLOEXEC);
	if (pxp < 0)
		return perror("/dev/pxp"), 1;
	dpy = XOpenDisplay(NULL);
	if (!dpy || !XShmQueryExtension(dpy))
		return fprintf(stderr, "serve X con XShm\n"), 1;

	src.size = fsize;
	if (ioctl(pxp, PXP_IOC_ALLOC, &src))
		return perror("PXP_IOC_ALLOC"), 1;
	srcmap = mmap(NULL, src.size, PROT_READ | PROT_WRITE, MAP_SHARED, pxp,
		      src.mmap_offset);
	if (srcmap == MAP_FAILED)
		return perror("mmap"), 1;

	/* finestra: il doppio del video se ci sta, altrimenti la misura del video */
	ww = sw * 2 <= DisplayWidth(dpy, 0) - 16 ? sw * 2 : sw;
	wh = sh * 2 <= DisplayHeight(dpy, 0) - 80 ? sh * 2 : sh;
	if (ww != sw * 2 || wh != sh * 2) {
		ww = sw;
		wh = sh;
	}
	win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, ww, wh, 0, 0, 0);
	XStoreName(dpy, win, title);
	hints.flags = PMinSize;
	hints.min_width = 64;
	hints.min_height = 48;
	XSetWMNormalHints(dpy, win, &hints);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);
	XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
	gc = XCreateGC(dpy, win, 0, NULL);
	XMapWindow(dpy, win);
	if (out_alloc(&o, ww, wh))
		return 1;

	t_next = t_stat = now();
	for (;;) {
		ssize_t got = 0;
		double t0, t1;

		/* eventi della finestra */
		while (XPending(dpy)) {
			XEvent e;

			XNextEvent(dpy, &e);
			if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete)
				goto done;
			if (e.type == KeyPress) {
				KeySym k = XLookupKeysym(&e.xkey, 0);

				if (k == XK_q || k == XK_Escape)
					goto done;
				if (k == XK_f)
					set_fullscreen(fullscreen ^= 1);
			}
			if (e.type == ConfigureNotify &&
			    ((e.xconfigure.width & ~7) != o.w ||
			     (e.xconfigure.height & ~7) != o.h)) {
				out_free(&o);
				if (out_alloc(&o, e.xconfigure.width, e.xconfigure.height))
					goto done;
			}
		}

		while ((size_t)got < fsize) {
			ssize_t r = read(vid, srcmap + got, fsize - got);

			if (r <= 0)
				goto done;
			got += r;
		}

		t0 = now();
		{
			struct pxp_job j = {
				.s0_index = src.index, .s0_format = PXP_S0_YUV420,
				.s0_width = sw, .s0_height = sh,
				.src_w = sw, .src_h = sh,
				.out_index = o.buf.index, .out_format = PXP_OUT_RGB565,
				.out_width = o.w, .out_height = o.h,
				.dst_w = o.w, .dst_h = o.h,
			};

			/* il PXP riduce al massimo a 1/4: oltre, mostra a 1/4 */
			if (sw > o.w * 4 || sh > o.h * 4) {
				j.dst_w = (sw / 4 + 7) & ~7;
				j.dst_h = (sh / 4 + 7) & ~7;
			}
			if (ioctl(pxp, PXP_IOC_RUN, &j))
				break;
		}
		t1 = now();
		/* dal buffer DMA all'immagine condivisa, poi a X */
		memcpy(o.img->data, o.map, o.buf.size < (uint32_t)(o.img->bytes_per_line * o.h) ?
		       o.buf.size : (uint32_t)(o.img->bytes_per_line * o.h));
		XShmPutImage(dpy, win, gc, o.img, 0, 0, 0, 0, o.w, o.h, False);
		XSync(dpy, False);
		pxp_ms += (t1 - t0) * 1000;
		put_ms += (now() - t1) * 1000;
		frames++;

		if (now() - t_stat >= 5) {
			fprintf(stderr, "%.1f fps | PXP %.1f ms | copia+X %.1f ms | finestra %dx%d\n",
				frames / (now() - t_stat), pxp_ms / frames, put_ms / frames, o.w, o.h);
			frames = 0;
			pxp_ms = put_ms = 0;
			t_stat = now();
		}

		if (fps) {
			struct timespec ts;
			double wait;

			t_next += 1.0 / fps;
			wait = t_next - now();
			if (wait > 0) {
				ts.tv_sec = (time_t)wait;
				ts.tv_nsec = (long)((wait - ts.tv_sec) * 1e9);
				nanosleep(&ts, NULL);
			} else if (wait < -1) {
				t_next = now();
			}
		}
	}
done:
	out_free(&o);
	XCloseDisplay(dpy);
	return 0;
}
