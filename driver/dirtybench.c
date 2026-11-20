// SPDX-License-Identifier: GPL-2.0
/*
 * dirtybench: misura quanti aggiornamenti al secondo arrivano al pannello
 * del Brain, a schermo intero e per una striscia di righe (DIRTYFB).
 * Ogni aggiornamento aspetta che il precedente sia finito, quindi il ritmo
 * misura davvero il trasferimento verso il pannello.
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDWR);
	struct drm_mode_card_res res = { 0 };
	uint32_t conns[4], crtcs[4];
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo modes[4];
	struct drm_mode_create_dumb cd = { 0 };
	struct drm_mode_fb_cmd fc = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	struct drm_mode_crtc crtc = { 0 }, saved = { 0 };
	int heights[] = { 0, 24, 8 }, i, n;

	if (fd < 0 || ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return perror("drm"), 1;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.count_fbs = res.count_encoders = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	conn.connector_id = conns[0];
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
	conn.modes_ptr = (uintptr_t)modes;
	conn.count_modes = conn.count_modes > 4 ? 4 : conn.count_modes;
	conn.count_props = conn.count_encoders = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);

	cd.width = modes[0].hdisplay;
	cd.height = modes[0].vdisplay;
	cd.bpp = 16;
	ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
	fc.width = cd.width; fc.height = cd.height; fc.pitch = cd.pitch;
	fc.bpp = 16; fc.depth = 16; fc.handle = cd.handle;
	ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fc);
	md.handle = cd.handle;
	ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
	uint16_t *p = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	for (i = 0; i < (int)(cd.size / 2); i++)
		p[i] = (i % cd.width) * 77 ^ (i / cd.width) * 31;

	saved.crtc_id = crtcs[0];
	ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &saved);
	crtc.crtc_id = crtcs[0];
	crtc.fb_id = fc.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&conns[0];
	crtc.count_connectors = 1;
	crtc.mode = modes[0];
	crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc))
		return perror("SETCRTC (serve il display libero)"), 1;

	printf("schermo %ux%u\n", cd.width, cd.height);
	for (i = 0; i < 3; i++) {
		struct drm_clip_rect clip = { 0, 200, cd.width, 200 + heights[i] };
		struct drm_mode_fb_dirty_cmd dc = { .fb_id = fc.fb_id, .num_clips = 1,
						    .clips_ptr = (uintptr_t)&clip };
		double t0, dt;

		if (!heights[i]) {
			clip.y1 = 0;
			clip.y2 = cd.height;
		}
		t0 = now();
		for (n = 0; now() - t0 < 3.0; n++)
			if (ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dc))
				return perror("DIRTYFB"), 1;
		/* ancora uno: aspetta la fine dell'ultimo trasferimento */
		ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dc);
		dt = now() - t0;
		printf("%-22s %7.1f aggiornamenti/s  (%.2f ms ciascuno)\n",
		       heights[i] ? (heights[i] == 24 ? "24 righe (testo)" : "8 righe") :
				    "schermo intero",
		       n / dt, dt * 1000 / n);
	}
	if (saved.mode_valid) {
		saved.set_connectors_ptr = (uintptr_t)&conns[0];
		saved.count_connectors = 1;
		ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &saved);
	}
	return 0;
}
