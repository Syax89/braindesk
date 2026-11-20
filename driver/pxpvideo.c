// SPDX-License-Identifier: GPL-2.0
/*
 * pxpvideo: riproduce un video YUV420 grezzo a schermo intero sul Brain.
 *
 * I fotogrammi piccoli (es. 424x240) vengono convertiti in RGB565 e
 * ingranditi dal PXP direttamente nel buffer dello schermo (DRM dumb
 * buffer condiviso con /dev/pxp come dma-buf). Con --cpu fa lo stesso
 * lavoro con il processore, per confronto.
 *
 *   pxpvideo [--cpu] [--loop] file.yuv|- larghezza altezza fps
 *
 * Con "-" legge i fotogrammi da stdin (es. da ffmpeg); con fps = 0 non
 * impone un ritmo (lo da' chi scrive, es. ffmpeg con l'audio su ALSA).
 * ESC, q o un tocco sullo schermo interrompono la riproduzione.
 */
#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "brain_pxp_uapi.h"

#define NFB 2

struct fb {
	uint32_t handle, fb_id, pitch, size;
	int prime_fd;
	uint32_t pxp_index;
	uint16_t *map;		/* solo in modalita' --cpu */
};

static int drm, pxp = -1;
static struct drm_mode_crtc saved_crtc;
static uint32_t conn_id;
static volatile sig_atomic_t stop;

/* tasti/tocco per uscire: ascolta tutti i dispositivi di input */
static int input_fds[8], n_input;

static void open_inputs(void)
{
	char path[64];
	int i;

	for (i = 0; i < 8 && n_input < 8; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0)
			input_fds[n_input++] = fd;
	}
}

static int quit_requested(void)
{
	struct input_event ev;
	int i;

	for (i = 0; i < n_input; i++)
		while (read(input_fds[i], &ev, sizeof(ev)) == sizeof(ev))
			if (ev.type == EV_KEY && ev.value == 1 &&
			    (ev.code == KEY_ESC || ev.code == KEY_Q ||
			     ev.code == BTN_TOUCH))
				return 1;
	return 0;
}

static int open_drm(void)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}
	return -1;
}

static void on_signal(int s)
{
	(void)s;
	stop = 1;
}

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	if (r)
		fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return r;
}

/* Trova connettore, modalita' e CRTC del pannello */
static int drm_setup(struct drm_mode_modeinfo *mode, uint32_t *crtc_id)
{
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector conn = { 0 };
	uint32_t conns[8], crtcs[8], encs[8];
	struct drm_mode_modeinfo modes[16];
	struct drm_mode_get_encoder enc = { 0 };

	if (xioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES"))
		return -1;
	if (!res.count_connectors || res.count_connectors > 8 ||
	    !res.count_crtcs || res.count_crtcs > 8)
		return -1;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.count_fbs = res.count_encoders = 0;
	if (xioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES"))
		return -1;

	conn.connector_id = conns[0];
	if (xioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR"))
		return -1;
	if (!conn.count_modes || conn.count_modes > 16 || conn.count_encoders > 8)
		return -1;
	conn.modes_ptr = (uintptr_t)modes;
	conn.encoders_ptr = (uintptr_t)encs;
	conn.count_props = 0;
	if (xioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR"))
		return -1;

	*mode = modes[0];
	conn_id = conn.connector_id;
	*crtc_id = crtcs[0];
	if (conn.encoder_id) {
		enc.encoder_id = conn.encoder_id;
		if (!ioctl(drm, DRM_IOCTL_MODE_GETENCODER, &enc) && enc.crtc_id)
			*crtc_id = enc.crtc_id;
	}

	saved_crtc.crtc_id = *crtc_id;
	ioctl(drm, DRM_IOCTL_MODE_GETCRTC, &saved_crtc);
	return 0;
}

static int fb_create(struct fb *f, int w, int h, int cpu)
{
	struct drm_mode_create_dumb cd = { .width = w, .height = h, .bpp = 16 };
	struct drm_mode_fb_cmd fc = { 0 };
	struct drm_prime_handle ph = { 0 };
	struct pxp_import imp = { 0 };

	if (ioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd) && errno == ENOMEM) {
		/* niente CMA sul Brain: compatta la RAM per avere 1 MB contiguo */
		int c = open("/proc/sys/vm/compact_memory", O_WRONLY);

		fprintf(stderr, "memoria frammentata, la compatto...\n");
		if (c >= 0) {
			if (write(c, "1", 1) < 0)
				perror("compact_memory");
			close(c);
		}
		if (xioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB"))
			return -1;
	} else if (!cd.handle) {
		fprintf(stderr, "CREATE_DUMB: %s\n", strerror(errno));
		return -1;
	}
	f->handle = cd.handle;
	f->pitch = cd.pitch;
	f->size = cd.size;

	fc.width = w;
	fc.height = h;
	fc.pitch = cd.pitch;
	fc.bpp = 16;
	fc.depth = 16;
	fc.handle = cd.handle;
	if (xioctl(drm, DRM_IOCTL_MODE_ADDFB, &fc, "ADDFB"))
		return -1;
	f->fb_id = fc.fb_id;

	if (cpu) {
		struct drm_mode_map_dumb md = { .handle = cd.handle };

		if (xioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md, "MAP_DUMB"))
			return -1;
		f->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED,
			      drm, md.offset);
		if (f->map == MAP_FAILED) {
			perror("mmap dumb");
			return -1;
		}
		memset(f->map, 0, cd.size);
		return 0;
	}

	ph.handle = cd.handle;
	ph.flags = DRM_CLOEXEC | DRM_RDWR;
	if (xioctl(drm, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph, "PRIME_HANDLE_TO_FD"))
		return -1;
	f->prime_fd = ph.fd;

	imp.fd = ph.fd;
	if (xioctl(pxp, PXP_IOC_IMPORT, &imp, "PXP_IOC_IMPORT"))
		return -1;
	f->pxp_index = imp.index;
	return 0;
}

static int show(uint32_t crtc_id, struct fb *f, struct drm_mode_modeinfo *mode,
		int first)
{
	if (first) {
		struct drm_mode_crtc c = {
			.crtc_id = crtc_id,
			.fb_id = f->fb_id,
			.set_connectors_ptr = (uintptr_t)&conn_id,
			.count_connectors = 1,
			.mode = *mode,
			.mode_valid = 1,
		};

		return xioctl(drm, DRM_IOCTL_MODE_SETCRTC, &c, "SETCRTC");
	} else {
		struct drm_mode_crtc_page_flip pf = {
			.crtc_id = crtc_id,
			.fb_id = f->fb_id,
		};

		return xioctl(drm, DRM_IOCTL_MODE_PAGE_FLIP, &pf, "PAGE_FLIP");
	}
}

/* Versione CPU: YUV420 -> RGB565 con ingrandimento nearest-neighbour */
static inline int clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void cpu_convert(const uint8_t *yuv, int sw, int sh, struct fb *f,
			int dw, int dh)
{
	const uint8_t *Y = yuv, *U = Y + sw * sh, *V = U + sw * sh / 4;
	int x, y;

	for (y = 0; y < dh; y++) {
		int sy = y * sh / dh;
		uint16_t *row = (uint16_t *)((uint8_t *)f->map + y * f->pitch);

		for (x = 0; x < dw; x++) {
			int sx = x * sw / dw;
			int c = Y[sy * sw + sx] - 16;
			int d = U[(sy / 2) * (sw / 2) + sx / 2] - 128;
			int e = V[(sy / 2) * (sw / 2) + sx / 2] - 128;
			int r = clamp8((298 * c + 409 * e + 128) >> 8);
			int g = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
			int b = clamp8((298 * c + 516 * d + 128) >> 8);

			row[x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		}
	}
}

int main(int argc, char **argv)
{
	int cpu = 0, loop = 0, a = 1;
	struct drm_mode_modeinfo mode;
	struct fb fbs[NFB] = { 0 };
	uint32_t crtc_id;
	struct pxp_alloc src = { 0 };
	uint8_t *srcmap = NULL, *cpubuf = NULL;
	int vid, sw, sh, fps, W, H, dw, dh, cur = 0, frames = 0, n = 0;
	size_t fsize;
	double t_read = 0, t_conv = 0, t_flip = 0, t_start, t_report;
	struct timespec next;

	while (a < argc && argv[a][0] == '-' && argv[a][1]) {
		if (!strcmp(argv[a], "--cpu"))
			cpu = 1;
		else if (!strcmp(argv[a], "--loop"))
			loop = 1;
		a++;
	}
	if (argc - a != 4) {
		fprintf(stderr, "uso: %s [--cpu] [--loop] file.yuv larghezza altezza fps\n",
			argv[0]);
		return 1;
	}
	sw = atoi(argv[a + 1]);
	sh = atoi(argv[a + 2]);
	fps = atoi(argv[a + 3]);
	fsize = (size_t)sw * sh * 3 / 2;
	if (sw <= 0 || sh <= 0 || fps < 0 || (!cpu && (sw % 8 || sh % 8))) {
		fprintf(stderr, "dimensioni non valide (multipli di 8 per il PXP)\n");
		return 1;
	}

	vid = strcmp(argv[a], "-") ? open(argv[a], O_RDONLY) : 0;
	if (vid < 0) {
		perror(argv[a]);
		return 1;
	}
	if (!vid)
		loop = 0;
	drm = open_drm();
	if (drm < 0) {
		perror("/dev/dri/card*");
		return 1;
	}
	open_inputs();
	if (!cpu) {
		pxp = open("/dev/pxp", O_RDWR | O_CLOEXEC);
		if (pxp < 0) {
			perror("/dev/pxp (modulo brain_pxp caricato?)");
			return 1;
		}
	}
	if (drm_setup(&mode, &crtc_id))
		return 1;
	W = mode.hdisplay;
	H = mode.vdisplay;
	/* area ingrandita: la piu' grande con le proporzioni del video,
	 * a multipli di 8 per il PXP */
	dw = W & ~7;
	dh = (int)((long)dw * sh / sw) & ~7;
	if (dh > H) {
		dh = H & ~7;
		dw = (int)((long)dh * sw / sh) & ~7;
	}
	fprintf(stderr, "schermo %dx%d, video %dx%d -> %dx%d, %d fps, %s\n",
		W, H, sw, sh, dw, dh, fps, cpu ? "CPU" : "PXP");

	for (int i = 0; i < NFB; i++)
		if (fb_create(&fbs[i], W, H, cpu))
			return 1;

	if (cpu) {
		cpubuf = malloc(fsize);
	} else {
		src.size = fsize;
		if (xioctl(pxp, PXP_IOC_ALLOC, &src, "PXP_IOC_ALLOC"))
			return 1;
		srcmap = mmap(NULL, src.size, PROT_READ | PROT_WRITE,
			      MAP_SHARED, pxp, src.mmap_offset);
		if (srcmap == MAP_FAILED) {
			perror("mmap pxp");
			return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	clock_gettime(CLOCK_MONOTONIC, &next);
	t_start = t_report = now_us();

	while (!stop && !quit_requested()) {
		double t0 = now_us(), t1, t2, t3;
		uint8_t *dst = cpu ? cpubuf : srcmap;
		ssize_t got = 0;

		while ((size_t)got < fsize) {
			ssize_t r = read(vid, dst + got, fsize - got);

			if (r <= 0)
				break;
			got += r;
		}
		if ((size_t)got < fsize) {
			if (!loop)
				break;
			lseek(vid, 0, SEEK_SET);
			continue;
		}
		t1 = now_us();

		cur ^= 1;
		if (cpu) {
			cpu_convert(cpubuf, sw, sh, &fbs[cur], dw, dh);
		} else {
			struct pxp_job j = {
				.s0_index = src.index,
				.s0_format = PXP_S0_YUV420,
				.s0_width = sw,
				.s0_height = sh,
				.src_w = sw,
				.src_h = sh,
				.out_index = fbs[cur].pxp_index,
				.out_format = PXP_OUT_RGB565,
				.out_width = W,
				.out_height = H,
				.dst_x = ((W - dw) / 2) & ~7,
				.dst_y = ((H - dh) / 2) & ~7,
				.dst_w = dw,
				.dst_h = dh,
				.background = 0,
			};

			if (xioctl(pxp, PXP_IOC_RUN, &j, "PXP_IOC_RUN"))
				break;
		}
		t2 = now_us();

		if (show(crtc_id, &fbs[cur], &mode, frames == 0))
			break;
		t3 = now_us();

		t_read += t1 - t0;
		t_conv += t2 - t1;
		t_flip += t3 - t2;
		frames++;
		n++;

		if (t3 - t_report >= 2e6) {
			fprintf(stderr,
				"%5.1f fps | lettura %5.1f ms | %s %5.1f ms | schermo %5.1f ms\n",
				n * 1e6 / (t3 - t_report), t_read / n / 1e3,
				cpu ? "CPU" : "PXP", t_conv / n / 1e3, t_flip / n / 1e3);
			t_read = t_conv = t_flip = 0;
			n = 0;
			t_report = t3;
		}

		if (!fps)
			continue;
		/* ritmo costante: aspetta l'istante del prossimo fotogramma */
		next.tv_nsec += 1000000000L / fps;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		{
			struct timespec t;

			/* se siamo in ritardo non accumulare il ritardo */
			clock_gettime(CLOCK_MONOTONIC, &t);
			if (t.tv_sec > next.tv_sec + 1)
				next = t;
		}
	}

	fprintf(stderr, "%d fotogrammi in %.1f s (%.1f fps medi)\n", frames,
		(now_us() - t_start) / 1e6,
		frames * 1e6 / (now_us() - t_start));

	/* rimette lo schermo com'era (console) */
	if (saved_crtc.mode_valid) {
		saved_crtc.set_connectors_ptr = (uintptr_t)&conn_id;
		saved_crtc.count_connectors = 1;
		ioctl(drm, DRM_IOCTL_MODE_SETCRTC, &saved_crtc);
	}
	return 0;
}
