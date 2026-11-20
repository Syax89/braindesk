// SPDX-License-Identifier: MIT
#include "pxpdev.h"
#include "brainplay.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* struct pxp_job of the first kernel interface: no out_y, no reserved[] */
struct pxp_job_v1 {
	__u32 f[22];
};
#define PXP_IOC_RUN_V1	_IOWR(PXP_IOC_MAGIC, 2, struct pxp_job_v1)

struct pxpdev {
	int fd;
	bool legacy;
#ifdef PXP_SIM
	struct pxpsim *sim;
#endif
};

static int dev_ioctl(struct pxpdev *d, unsigned long req, void *arg)
{
#ifdef PXP_SIM
	if (d->sim)
		return pxpsim_ioctl(d->sim, req, arg);
#endif
	return ioctl(d->fd, req, arg);
}

struct pxpdev *pxpdev_open(const char *sim)
{
	struct pxpdev *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->fd = -1;
	if (sim) {
#ifdef PXP_SIM
		d->sim = pxpsim_new(!strcmp(sim, "legacy"));
		if (d->sim)
			return d;
#endif
		free(d);
		return NULL;
	}
	d->fd = open("/dev/pxp", O_RDWR | O_CLOEXEC);
	if (d->fd < 0) {
		if (errno != ENOENT)
			verb("/dev/pxp: %s", strerror(errno));
		free(d);
		return NULL;
	}
	return d;
}

void pxpdev_close(struct pxpdev *d)
{
	if (!d)
		return;
#ifdef PXP_SIM
	if (d->sim)
		pxpsim_free(d->sim);
#endif
	if (d->fd >= 0)
		close(d->fd);
	free(d);
}

bool pxpdev_screen(struct pxpdev *d, struct pxp_screen *scr)
{
	memset(scr, 0, sizeof(*scr));
	if (dev_ioctl(d, PXP_IOC_SCREEN, scr) < 0) {
		verb("PXP: no screen overlay in this kernel (%s)", strerror(errno));
		return false;
	}
	return scr->width && scr->height && scr->format == PXP_OUT_RGB565;
}

void *pxpdev_alloc(struct pxpdev *d, size_t *size, uint32_t *index)
{
	struct pxp_alloc a = { .size = (uint32_t)*size };
	void *map;

	if (dev_ioctl(d, PXP_IOC_ALLOC, &a) < 0) {
		err("PXP: cannot allocate a %zu byte buffer: %s", *size, strerror(errno));
		return NULL;
	}
#ifdef PXP_SIM
	if (d->sim)
		map = pxpsim_mmap(d->sim, a.size, a.mmap_offset);
	else
#endif
		map = mmap(NULL, a.size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd,
			   (off_t)a.mmap_offset);
	if (!map || map == MAP_FAILED) {
		err("PXP: cannot map a buffer: %s", strerror(errno));
		dev_ioctl(d, PXP_IOC_FREE, &a.index);
		return NULL;
	}
	*size = a.size;
	*index = a.index;
	return map;
}

void pxpdev_free(struct pxpdev *d, void *map, size_t size, uint32_t index)
{
	if (!map)
		return;
#ifdef PXP_SIM
	if (!d->sim)
#endif
		munmap(map, size);
	dev_ioctl(d, PXP_IOC_FREE, &index);
}

int pxpdev_run(struct pxpdev *d, struct pxp_job *job)
{
	struct pxp_job_v1 v1;
	int r;

	if (!d->legacy) {
		if (dev_ioctl(d, PXP_IOC_RUN, job) == 0)
			return 0;
		if (errno != ENOTTY)
			return -errno;
		/* kernel with the first interface: same job, shorter structure */
		d->legacy = true;
		verb("PXP: kernel with the first job interface");
	}
	if (job->out_index == PXP_BUF_SCREEN || job->out_y)
		return -ENOTTY;
	memcpy(&v1, job, sizeof(v1));
	r = dev_ioctl(d, PXP_IOC_RUN_V1, &v1);
	job->stat = v1.f[20];
	job->elapsed_us = v1.f[21];
	return r < 0 ? -errno : 0;
}
