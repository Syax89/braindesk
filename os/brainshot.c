// SPDX-License-Identifier: GPL-2.0
/*
 * brainshot: scrive su stdout (RGB565 grezzo) l'immagine che il pannello
 * del Brain sta mostrando, leggendo il framebuffer attivo del CRTC via DRM.
 * Va eseguito da root.  Stampa "LARGHEZZAxALTEZZA" su stderr.
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/dev/dri/card0", O_RDWR);
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[4];
	struct drm_mode_crtc crtc = { 0 };
	struct drm_mode_fb_cmd fb = { 0 };
	struct drm_mode_map_dumb md = { 0 };
	uint8_t *p;
	uint32_t y;

	if (fd < 0 || ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return perror("drm"), 1;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.count_crtcs = res.count_crtcs > 4 ? 4 : res.count_crtcs;
	res.count_fbs = res.count_connectors = res.count_encoders = 0;
	ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
	crtc.crtc_id = crtcs[0];
	if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) || !crtc.fb_id)
		return fprintf(stderr, "nessun framebuffer attivo\n"), 1;
	fb.fb_id = crtc.fb_id;
	if (ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb) || !fb.handle)
		return perror("GETFB (serve root)"), 1;
	md.handle = fb.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		return perror("MAP_DUMB"), 1;
	p = mmap(NULL, (size_t)fb.pitch * fb.height, PROT_READ, MAP_SHARED, fd, md.offset);
	if (p == MAP_FAILED)
		return perror("mmap"), 1;
	for (y = 0; y < fb.height; y++)
		fwrite(p + (size_t)y * fb.pitch, 2, fb.width, stdout);
	fprintf(stderr, "%ux%u\n", fb.width, fb.height);
	return 0;
}
