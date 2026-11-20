// SPDX-License-Identifier: GPL-2.0
/* pxpscreen: prova dell'overlay (PXP -> frame buffer del pannello).
 * Genera fotogrammi YUV420 424x240 in movimento e li manda scalati sulle
 * righe [y, y+h) dello schermo; stampa i tempi. */
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

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100, sw = 424, sh = 240;
	int fd = open("/dev/pxp", O_RDWR);
	struct pxp_screen scr;
	struct pxp_alloc src = { .size = sw * sh * 3 / 2 };
	uint8_t *p;
	double t0, hw = 0;

	if (fd < 0 || ioctl(fd, PXP_IOC_SCREEN, &scr))
		return perror("PXP_IOC_SCREEN"), 1;
	printf("schermo %ux%u pitch %u\n", scr.width, scr.height, scr.pitch);
	if (ioctl(fd, PXP_IOC_ALLOC, &src))
		return perror("alloc"), 1;
	p = mmap(0, src.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, src.mmap_offset);
	int dw = (scr.width / 8 * 8), dh = dw * sh / sw / 8 * 8;
	if (dh > (int)scr.height - 96) { dh = (scr.height - 96) / 8 * 8; dw = dh * sw / sh / 8 * 8; }
	t0 = now();
	for (int f = 0; f < n; f++) {
		for (int y = 0; y < sh; y++)
			memset(p + y * sw, (y + f * 4) & 0xff, sw);
		memset(p + sw * sh, 128 + ((f * 2) & 63), sw * sh / 4);
		memset(p + sw * sh * 5 / 4, 100, sw * sh / 4);
		struct pxp_job j = {
			.s0_index = src.index, .s0_format = PXP_S0_YUV420,
			.s0_width = sw, .s0_height = sh, .src_w = sw, .src_h = sh,
			.out_index = PXP_BUF_SCREEN, .out_format = PXP_OUT_RGB565,
			.out_width = scr.width, .out_height = dh, .out_y = 48,
			.dst_x = (scr.width - dw) / 2 / 8 * 8, .dst_w = dw, .dst_h = dh,
		};
		if (ioctl(fd, PXP_IOC_RUN, &j))
			return perror("PXP_IOC_RUN"), 1;
		hw += j.elapsed_us;
	}
	t0 = now() - t0;
	printf("%d fotogrammi %dx%d: %.1f fps, PXP %.2f ms, totale %.2f ms a fotogramma\n",
	       n, dw, dh, n / t0, hw / n / 1000, t0 * 1000 / n);
	return 0;
}
