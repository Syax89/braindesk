// SPDX-License-Identifier: GPL-2.0
/*
 * pxptest: prove del driver brain-pxp.
 * Ogni prova confronta il risultato del PXP con lo stesso calcolo fatto
 * dalla CPU e misura i tempi di entrambi.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "brain_pxp_uapi.h"

static int fd;

struct buf {
	uint32_t index;
	uint32_t size;
	void *p;
};

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int balloc(struct buf *b, uint32_t size)
{
	struct pxp_alloc a = { .size = size };

	if (ioctl(fd, PXP_IOC_ALLOC, &a)) {
		perror("PXP_IOC_ALLOC");
		return -1;
	}
	b->index = a.index;
	b->size = a.size;
	b->p = mmap(NULL, a.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    a.mmap_offset);
	if (b->p == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	return 0;
}

static void bfree(struct buf *b)
{
	munmap(b->p, b->size);
	ioctl(fd, PXP_IOC_FREE, &b->index);
}

static int run(struct pxp_job *j, const char *what)
{
	if (ioctl(fd, PXP_IOC_RUN, j)) {
		printf("  %-34s ERRORE %s (stat=0x%08x)\n", what,
		       strerror(errno), j->stat);
		return -1;
	}
	return 0;
}

static int verdict(const char *what, long bad, long total, double hw_us,
		   double cpu_us)
{
	printf("  %-34s %s  (%ld/%ld pixel diversi)  PXP %7.0f us  CPU %8.0f us\n",
	       what, bad ? "FALLITA" : "OK     ", bad, total, hw_us, cpu_us);
	return bad ? 1 : 0;
}

/* 1. Copia RGB565 -> RGB565 senza trasformazioni */
static int test_copy(void)
{
	const int W = 64, H = 64;
	struct buf s, o;
	uint16_t *sp, *op, *ref;
	struct pxp_job j = { 0 };
	long bad = 0;
	double t;
	int i;

	if (balloc(&s, W * H * 2) || balloc(&o, W * H * 2))
		return 1;
	sp = s.p;
	op = o.p;
	for (i = 0; i < W * H; i++)
		sp[i] = (uint16_t)(i * 2654435761u >> 7);
	memset(op, 0, W * H * 2);

	j.s0_index = s.index;
	j.s0_format = PXP_S0_RGB565;
	j.s0_width = W;
	j.s0_height = H;
	j.out_index = o.index;
	j.out_format = PXP_OUT_RGB565;
	j.out_width = W;
	j.out_height = H;
	if (run(&j, "copia 64x64 RGB565"))
		return 1;

	ref = malloc(W * H * 2);
	t = now_us();
	memcpy(ref, sp, W * H * 2);
	t = now_us() - t;
	for (i = 0; i < W * H; i++)
		bad += op[i] != ref[i];
	if (bad)
		printf("    primo pixel: atteso %04x, ottenuto %04x\n", ref[0], op[0]);
	free(ref);
	bfree(&s);
	bfree(&o);
	return verdict("copia 64x64 RGB565", bad, W * H, j.elapsed_us, t);
}

/* 2. Rotazione di 90 gradi a schermo intero (RGB565) */
static int test_rotate(int deg, int W, int H)
{
	struct buf s, o;
	uint16_t *sp, *op, *ref;
	struct pxp_job j = { 0 };
	int OW = (deg % 180) ? H : W, OH = (deg % 180) ? W : H;
	long bad = 0;
	char name[64];
	double t;
	int x, y;

	if (balloc(&s, W * H * 2) || balloc(&o, W * H * 2))
		return 1;
	sp = s.p;
	op = o.p;
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			sp[y * W + x] = (uint16_t)((x * 7) ^ (y * 131));
	memset(op, 0, W * H * 2);

	j.s0_index = s.index;
	j.s0_format = PXP_S0_RGB565;
	j.s0_width = W;
	j.s0_height = H;
	j.out_index = o.index;
	j.out_format = PXP_OUT_RGB565;
	j.out_width = OW;
	j.out_height = OH;
	j.rotate = deg;
	snprintf(name, sizeof(name), "rotazione %d %dx%d RGB565", deg, W, H);
	if (run(&j, name))
		return 1;

	/* stessa rotazione (oraria) con la CPU, leggendo dalla RAM normale */
	ref = malloc(W * H * 2);
	{
		uint16_t *src = malloc(W * H * 2);

		memcpy(src, sp, W * H * 2);
		t = now_us();
		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				int dx, dy;

				switch (deg) {
				case 90:  dx = H - 1 - y; dy = x; break;
				case 180: dx = W - 1 - x; dy = H - 1 - y; break;
				case 270: dx = y; dy = W - 1 - x; break;
				default:  dx = x; dy = y; break;
				}
				ref[dy * OW + dx] = src[y * W + x];
			}
		t = now_us() - t;
		free(src);
	}
	for (x = 0; x < W * H; x++)
		bad += op[x] != ref[x];
	if (bad) {
		/* forse il PXP ruota in senso antiorario: prova l'altro verso */
		long bad2 = 0;

		for (y = 0; y < H; y++)
			for (x = 0; x < W; x++) {
				int dx = (deg == 90) ? y : (deg == 270) ? H - 1 - y : x;
				int dy = (deg == 90) ? W - 1 - x : (deg == 270) ? x : y;

				if (deg % 180)
					bad2 += op[dy * OW + dx] != sp[y * W + x];
			}
		if (deg % 180)
			printf("    (in senso antiorario: %ld pixel diversi)\n", bad2);
	}
	free(ref);
	bfree(&s);
	bfree(&o);
	return verdict(name, bad, (long)W * H, j.elapsed_us, t);
}

/* 3. YUV420 -> RGB565 con riduzione 2x (lo scaler vuole sorgenti YUV) */
static int clamp8(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : v;
}

static int test_yuv_scale(void)
{
	const int W = 320, H = 240, OW = 160, OH = 120;
	struct buf s, o;
	uint8_t *Y, *U, *V;
	uint16_t *op;
	struct pxp_job j = { 0 };
	long bad = 0;
	double t;
	int x, y;

	if (balloc(&s, W * H * 3 / 2) || balloc(&o, OW * OH * 2))
		return 1;
	Y = s.p;
	U = Y + W * H;
	V = U + W * H / 4;
	/* sfumatura orizzontale di luminosita', colore costante */
	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
			Y[y * W + x] = 16 + x * 219 / W;
	memset(U, 128, W * H / 4);
	memset(V, 128, W * H / 4);
	memset(o.p, 0, OW * OH * 2);
	op = o.p;

	j.s0_index = s.index;
	j.s0_format = PXP_S0_YUV420;
	j.s0_width = W;
	j.s0_height = H;
	j.src_w = W;
	j.src_h = H;
	j.dst_w = OW;
	j.dst_h = OH;
	j.out_index = o.index;
	j.out_format = PXP_OUT_RGB565;
	j.out_width = OW;
	j.out_height = OH;
	if (run(&j, "YUV420 320x240 -> RGB565 160x120"))
		return 1;

	/* confronto tollerante: grigio atteso con Y su scala 16..235 */
	t = now_us();
	for (y = 0; y < OH; y++)
		for (x = 0; x < OW; x++) {
			int yy = Y[(y * 2) * W + x * 2];
			int g = clamp8((yy - 16) * 255 / 219);
			uint16_t p = op[y * OW + x];
			int r = (p >> 11) << 3, gg = ((p >> 5) & 63) << 2,
			    b = (p & 31) << 3;

			if (abs(r - g) > 24 || abs(gg - g) > 24 || abs(b - g) > 24)
				bad++;
		}
	t = now_us() - t;
	if (bad)
		printf("    esempio pixel (80,60): 0x%04x\n", op[60 * OW + 80]);
	bfree(&s);
	bfree(&o);
	return verdict("YUV420 320x240 -> RGB565 160x120", bad, OW * OH,
		       j.elapsed_us, t);
}

int main(void)
{
	struct pxp_info info;
	int fails = 0;

	fd = open("/dev/pxp", O_RDWR);
	if (fd < 0) {
		perror("/dev/pxp");
		return 1;
	}
	if (ioctl(fd, PXP_IOC_INFO, &info) == 0)
		printf("PXP versione 0x%08x  ctrl=0x%08x  stat=0x%08x  irq=%u\n",
		       info.version, info.ctrl, info.stat, info.irq_count);

	fails += test_copy();
	fails += test_rotate(90, 64, 32);
	fails += test_rotate(180, 64, 32);
	fails += test_rotate(270, 64, 32);
	fails += test_yuv_scale();
	fails += test_rotate(90, 848, 480);

	if (ioctl(fd, PXP_IOC_INFO, &info) == 0)
		printf("lavori completati %u, timeout %u, interrupt %u\n",
		       info.jobs_done, info.jobs_timeout, info.irq_count);
	printf(fails ? "\n%d prove fallite\n" : "\ntutte le prove superate\n",
	       fails);
	close(fd);
	return fails ? 1 : 0;
}
