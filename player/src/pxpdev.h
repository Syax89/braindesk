/* SPDX-License-Identifier: MIT */
/*
 * /dev/pxp: the pixel pipeline of the i.MX28 (see brain_pxp_uapi.h).
 * Handles both generations of the kernel interface: the current one with
 * the screen overlay and the first one (smaller struct pxp_job, no overlay).
 */
#ifndef BRAINPLAY_PXPDEV_H
#define BRAINPLAY_PXPDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "brain_pxp_uapi.h"

struct pxpdev;

/* sim: NULL = the real /dev/pxp; "new" / "legacy" = simulator (test builds) */
struct pxpdev *pxpdev_open(const char *sim);
void pxpdev_close(struct pxpdev *d);
/* geometry of the frame buffer the panel shows; false = no overlay support */
bool pxpdev_screen(struct pxpdev *d, struct pxp_screen *scr);
/* contiguous DMA buffer, mapped; *size is rounded up by the kernel */
void *pxpdev_alloc(struct pxpdev *d, size_t *size, uint32_t *index);
void pxpdev_free(struct pxpdev *d, void *map, size_t size, uint32_t index);
/* runs a job and waits for it; 0 or -errno */
int pxpdev_run(struct pxpdev *d, struct pxp_job *job);

#ifdef PXP_SIM
/*
 * Software model of the kernel driver, for tests without the hardware.
 * Jobs for PXP_BUF_SCREEN end in a private frame buffer whose changed rows
 * are handed to the "present" callback (the test build paints them on the
 * X root window over all the windows, as the panel would show them).
 */
struct pxpsim;
typedef void (*pxpsim_present_fn)(void *ctx, const uint16_t *rows, int y, int h);

struct pxpsim *pxpsim_new(bool legacy);
void pxpsim_free(struct pxpsim *s);
void pxpsim_set_screen(int width, int height, pxpsim_present_fn fn, void *ctx);
int pxpsim_ioctl(struct pxpsim *s, unsigned long req, void *arg);	/* like ioctl() */
void *pxpsim_mmap(struct pxpsim *s, size_t size, uint64_t offset);
#endif

#endif
