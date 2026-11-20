// SPDX-License-Identifier: GPL-2.0
/*
 * brain-pxp: driver minimale per il PXP (Pixel Pipeline) v2 dell'i.MX28,
 * pensato per i dizionari SHARP Brain con Brainux.
 *
 * Il nodo pxp@8002a000 esiste nel device tree ma e' "disabled" e senza
 * compatible, quindi il modulo lo cerca per nome e ne usa solo indirizzo
 * e interrupt. Espone /dev/pxp con:
 *   - buffer DMA contigui (write-combine) mappabili con mmap()
 *   - un ioctl che programma il PXP, lo avvia e attende l'interrupt
 *
 * Registri presi dal BSP Freescale 2.6.35 per i.MX28 (regs-pxp.h).
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/stmp_device.h>
#include <linux/uaccess.h>

#include "brain_pxp_uapi.h"

#define DRV_NAME		"brain-pxp"

/* Registri (offset dalla base 0x8002a000) */
#define HW_PXP_CTRL		0x000
#define HW_PXP_CTRL_SET		0x004
#define HW_PXP_CTRL_CLR		0x008
#define HW_PXP_STAT		0x010
#define HW_PXP_STAT_CLR		0x018
#define HW_PXP_OUTBUF		0x020
#define HW_PXP_OUTBUF2		0x030
#define HW_PXP_OUTSIZE		0x040
#define HW_PXP_S0BUF		0x050
#define HW_PXP_S0UBUF		0x060
#define HW_PXP_S0VBUF		0x070
#define HW_PXP_S0PARAM		0x080
#define HW_PXP_S0BACKGROUND	0x090
#define HW_PXP_S0CROP		0x0a0
#define HW_PXP_S0SCALE		0x0b0
#define HW_PXP_S0OFFSET		0x0c0
#define HW_PXP_CSCCOEFF0	0x0d0
#define HW_PXP_CSCCOEFF1	0x0e0
#define HW_PXP_CSCCOEFF2	0x0f0
#define HW_PXP_S0COLORKEYLOW	0x180
#define HW_PXP_S0COLORKEYHIGH	0x190
#define HW_PXP_OLCOLORKEYLOW	0x1a0
#define HW_PXP_OLCOLORKEYHIGH	0x1b0
#define HW_PXP_VERSION		0x1f0
#define HW_PXP_OLnPARAM(n)	(0x220 + (n) * 0x40)
#define PXP_NUM_OVERLAYS	8

/* Campi di HW_PXP_CTRL */
#define CTRL_SFTRST		BIT(31)
#define CTRL_CLKGATE		BIT(30)
#define CTRL_CROP		BIT(19)
#define CTRL_SCALE		BIT(18)
#define CTRL_S0_FORMAT(v)	(((v) & 0xf) << 12)
#define CTRL_VFLIP		BIT(11)
#define CTRL_HFLIP		BIT(10)
#define CTRL_ROTATE(v)		(((v) & 0x3) << 8)
#define CTRL_OUTBUF_FORMAT(v)	(((v) & 0xf) << 4)
#define CTRL_LCD_HANDSHAKE	BIT(3)
#define CTRL_NEXT_IRQ_ENABLE	BIT(2)
#define CTRL_IRQ_ENABLE		BIT(1)
#define CTRL_ENABLE		BIT(0)

/* Campi di HW_PXP_STAT */
#define STAT_NEXT_IRQ		BIT(3)
#define STAT_AXI_READ_ERROR	BIT(2)
#define STAT_AXI_WRITE_ERROR	BIT(1)
#define STAT_IRQ		BIT(0)
#define STAT_ALL_IRQS		(STAT_NEXT_IRQ | STAT_AXI_READ_ERROR | \
				 STAT_AXI_WRITE_ERROR | STAT_IRQ)

/* Campi a blocchi 8x8: XBASE[31:24] YBASE[23:16] WIDTH[15:8] HEIGHT[7:0] */
#define BLK(xb, yb, w, h)	((((xb) & 0xff) << 24) | (((yb) & 0xff) << 16) | \
				 (((w) & 0xff) << 8) | ((h) & 0xff))
#define OUTSIZE(a, w, h)	((((a) & 0xff) << 24) | (((w) & 0xfff) << 12) | \
				 ((h) & 0xfff))

#define SCALE_ONE		0x1000	/* 1.0 in virgola fissa 4.12 */
#define SCALE_MAX		0x4000	/* riduzione massima 1/4 */
#define MAX_BLOCKS		255	/* i campi a blocchi sono da 8 bit */

struct pxp_buf {
	void *vaddr;			/* solo per i buffer allocati qui */
	dma_addr_t dma;
	size_t size;
	/* buffer importati da un dma-buf */
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
};

static bool buf_in_use(const struct pxp_buf *b)
{
	return b->vaddr || b->dmabuf;
}

struct pxp_file {
	struct pxp_buf bufs[PXP_MAX_BUFFERS];
	struct mutex lock;	/* protegge bufs */
};

struct brain_pxp {
	void __iomem *base;
	int irq;
	struct platform_device *pdev;	/* solo per le allocazioni DMA */
	struct miscdevice misc;
	struct mutex hw_lock;		/* un lavoro alla volta */
	struct completion done;
	u32 last_stat;
	u32 irq_count;
	u32 jobs_done;
	u32 jobs_timeout;
};

static struct brain_pxp *pxp_dev;

static inline u32 pxp_read(struct brain_pxp *pxp, u32 reg)
{
	return readl(pxp->base + reg);
}

static inline void pxp_write(struct brain_pxp *pxp, u32 val, u32 reg)
{
	writel(val, pxp->base + reg);
}

static irqreturn_t pxp_irq(int irq, void *data)
{
	struct brain_pxp *pxp = data;
	u32 stat = pxp_read(pxp, HW_PXP_STAT);

	if (!(stat & STAT_ALL_IRQS))
		return IRQ_NONE;

	pxp_write(pxp, stat & STAT_ALL_IRQS, HW_PXP_STAT_CLR);
	pxp->last_stat = stat;
	pxp->irq_count++;
	complete(&pxp->done);
	return IRQ_HANDLED;
}

static int pxp_reset(struct brain_pxp *pxp)
{
	int ret = stmp_reset_block(pxp->base + HW_PXP_CTRL);

	if (ret)
		dev_err(pxp->misc.this_device, "reset del PXP fallito: %d\n", ret);
	return ret;
}

/* ---- formati ---------------------------------------------------------- */

static bool s0_is_yuv(u32 fmt)
{
	return fmt >= PXP_S0_YUV422 && fmt <= PXP_S0_VYUY;
}

static bool s0_is_planar(u32 fmt)
{
	return fmt == PXP_S0_YUV422 || fmt == PXP_S0_YUV420;
}

/* Byte necessari per un'immagine sorgente w x h; 0 = formato non valido */
static u64 s0_bytes(u32 fmt, u32 w, u32 h)
{
	u64 px = (u64)w * h;

	switch (fmt) {
	case PXP_S0_ARGB8888:
	case PXP_S0_RGB888:
		return px * 4;
	case PXP_S0_RGB565:
	case PXP_S0_RGB555:
	case PXP_S0_UYVY:
	case PXP_S0_VYUY:
	case PXP_S0_YUV422:
		return px * 2;
	case PXP_S0_YUV420:
		return px * 3 / 2;
	default:
		return 0;
	}
}

static u64 out_bytes(u32 fmt, u32 w, u32 h)
{
	u64 px = (u64)w * h;

	switch (fmt) {
	case PXP_OUT_ARGB8888:
	case PXP_OUT_RGB888:
	case PXP_OUT_YUV444:
		return px * 4;
	case PXP_OUT_RGB888P:
		return px * 3;
	case PXP_OUT_ARGB1555:
	case PXP_OUT_RGB565:
	case PXP_OUT_RGB555:
	case PXP_OUT_UYVY:
	case PXP_OUT_VYUY:
		return px * 2;
	default:
		return 0;
	}
}

/* ---- buffer ----------------------------------------------------------- */

static int pxp_ioc_alloc(struct pxp_file *pf, struct pxp_alloc __user *uarg)
{
	struct pxp_alloc a;
	struct device *dev = &pxp_dev->pdev->dev;
	int i, ret = -ENOSPC;

	if (copy_from_user(&a, uarg, sizeof(a)))
		return -EFAULT;
	if (!a.size || a.size > SZ_4M)
		return -EINVAL;

	mutex_lock(&pf->lock);
	for (i = 0; i < PXP_MAX_BUFFERS; i++) {
		struct pxp_buf *b = &pf->bufs[i];

		if (buf_in_use(b))
			continue;
		b->size = PAGE_ALIGN(a.size);
		b->vaddr = dma_alloc_wc(dev, b->size, &b->dma, GFP_KERNEL);
		if (!b->vaddr) {
			ret = -ENOMEM;
			break;
		}
		a.index = i;
		a.size = b->size;
		/* ogni buffer ha la sua finestra da 16 MB nello spazio di mmap */
		a.mmap_offset = (u64)i << 24;
		ret = 0;
		break;
	}
	mutex_unlock(&pf->lock);

	if (!ret && copy_to_user(uarg, &a, sizeof(a)))
		return -EFAULT;
	return ret;
}

static void pxp_free_buf(struct pxp_buf *b)
{
	if (b->dmabuf) {
		dma_buf_unmap_attachment(b->attach, b->sgt, DMA_BIDIRECTIONAL);
		dma_buf_detach(b->dmabuf, b->attach);
		dma_buf_put(b->dmabuf);
	} else if (b->vaddr) {
		dma_free_wc(&pxp_dev->pdev->dev, b->size, b->vaddr, b->dma);
	}
	memset(b, 0, sizeof(*b));
}

static int pxp_ioc_import(struct pxp_file *pf, struct pxp_import __user *uarg)
{
	struct device *dev = &pxp_dev->pdev->dev;
	struct pxp_import imp;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	int i, ret;

	if (copy_from_user(&imp, uarg, sizeof(imp)))
		return -EFAULT;

	dmabuf = dma_buf_get(imp.fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	attach = dma_buf_attach(dmabuf, dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto err_put;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_detach;
	}

	/* il PXP non ha MMU: serve memoria fisicamente contigua */
	if (sgt->nents != 1) {
		dev_warn(pxp_dev->misc.this_device,
			 "dma-buf non contiguo (%u segmenti)\n", sgt->nents);
		ret = -EINVAL;
		goto err_unmap;
	}

	mutex_lock(&pf->lock);
	ret = -ENOSPC;
	for (i = 0; i < PXP_MAX_BUFFERS; i++) {
		struct pxp_buf *b = &pf->bufs[i];

		if (buf_in_use(b))
			continue;
		b->dmabuf = dmabuf;
		b->attach = attach;
		b->sgt = sgt;
		b->dma = sg_dma_address(sgt->sgl);
		b->size = dmabuf->size;
		imp.index = i;
		imp.size = b->size;
		ret = 0;
		break;
	}
	mutex_unlock(&pf->lock);
	if (ret)
		goto err_unmap;

	return copy_to_user(uarg, &imp, sizeof(imp)) ? -EFAULT : 0;

err_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
err_detach:
	dma_buf_detach(dmabuf, attach);
err_put:
	dma_buf_put(dmabuf);
	return ret;
}

static int pxp_ioc_free(struct pxp_file *pf, u32 __user *uarg)
{
	u32 idx;

	if (get_user(idx, uarg))
		return -EFAULT;
	if (idx >= PXP_MAX_BUFFERS)
		return -EINVAL;
	mutex_lock(&pf->lock);
	pxp_free_buf(&pf->bufs[idx]);
	mutex_unlock(&pf->lock);
	return 0;
}

static int pxp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct pxp_file *pf = file->private_data;
	unsigned long idx = vma->vm_pgoff >> (24 - PAGE_SHIFT);
	unsigned long len = vma->vm_end - vma->vm_start;
	struct pxp_buf *b;
	int ret;

	if (idx >= PXP_MAX_BUFFERS || (vma->vm_pgoff & ((SZ_16M >> PAGE_SHIFT) - 1)))
		return -EINVAL;

	mutex_lock(&pf->lock);
	b = &pf->bufs[idx];
	if (!b->vaddr || len > b->size) {
		mutex_unlock(&pf->lock);
		return -EINVAL;
	}
	vma->vm_pgoff = 0;
	ret = dma_mmap_wc(&pxp_dev->pdev->dev, vma, b->vaddr, b->dma, len);
	mutex_unlock(&pf->lock);
	return ret;
}

/* ---- esecuzione di un lavoro ------------------------------------------ */

static int check_blk(u32 v)
{
	return (v & 7) || (v >> 3) > MAX_BLOCKS;
}

static int pxp_ioc_run(struct pxp_file *pf, struct pxp_job __user *uarg)
{
	struct brain_pxp *pxp = pxp_dev;
	struct pxp_job j;
	struct pxp_buf *s0, *out;
	u32 ctrl, xscale = SCALE_ONE, yscale = SCALE_ONE;
	u32 osize_w, osize_h, rot;
	dma_addr_t y, u = 0, v = 0;
	unsigned long tmo;
	ktime_t t0;
	long left;
	int i, ret;

	if (copy_from_user(&j, uarg, sizeof(j)))
		return -EFAULT;

	if (j.s0_index >= PXP_MAX_BUFFERS || j.out_index >= PXP_MAX_BUFFERS)
		return -EINVAL;
	if (j.rotate % 90 || j.rotate > 270)
		return -EINVAL;
	rot = j.rotate / 90;

	if (!j.dst_w)
		j.dst_w = (rot & 1) ? j.out_height : j.out_width;
	if (!j.dst_h)
		j.dst_h = (rot & 1) ? j.out_width : j.out_height;
	if (!j.src_w)
		j.src_w = j.dst_w;
	if (!j.src_h)
		j.src_h = j.dst_h;

	/* tutto a multipli di 8 e dentro i campi a 8 bit del PXP */
	if (check_blk(j.s0_width) || check_blk(j.s0_height) ||
	    check_blk(j.src_x) || check_blk(j.src_y) ||
	    check_blk(j.dst_x) || check_blk(j.dst_y) ||
	    check_blk(j.dst_w) || check_blk(j.dst_h) ||
	    !j.s0_width || !j.s0_height || !j.dst_w || !j.dst_h ||
	    !j.out_width || !j.out_height ||
	    j.out_width > 0xfff || j.out_height > 0xfff)
		return -EINVAL;

	/* il ritaglio deve stare dentro la sorgente */
	if ((u64)j.src_x + j.src_w > j.s0_width ||
	    (u64)j.src_y + j.src_h > j.s0_height)
		return -EINVAL;

	/* coordinate di uscita prima della rotazione */
	osize_w = (rot & 1) ? j.out_height : j.out_width;
	osize_h = (rot & 1) ? j.out_width : j.out_height;
	if ((u64)j.dst_x + j.dst_w > osize_w || (u64)j.dst_y + j.dst_h > osize_h)
		return -EINVAL;

	if (j.src_w != j.dst_w || j.src_h != j.dst_h) {
		/* sull'i.MX28 lo scaler funziona solo con sorgenti YUV */
		if (!s0_is_yuv(j.s0_format))
			return -EINVAL;
		xscale = j.src_w * SCALE_ONE / j.dst_w;
		yscale = j.src_h * SCALE_ONE / j.dst_h;
		if (xscale > SCALE_MAX || yscale > SCALE_MAX)
			return -ERANGE;
	}

	mutex_lock(&pf->lock);
	s0 = &pf->bufs[j.s0_index];
	out = &pf->bufs[j.out_index];
	ret = -EINVAL;
	if (!buf_in_use(s0) || !buf_in_use(out))
		goto out_unlock_file;
	if (!s0_bytes(j.s0_format, j.s0_width, j.s0_height) ||
	    s0_bytes(j.s0_format, j.s0_width, j.s0_height) > s0->size)
		goto out_unlock_file;
	if (!out_bytes(j.out_format, j.out_width, j.out_height) ||
	    out_bytes(j.out_format, j.out_width, j.out_height) > out->size)
		goto out_unlock_file;

	y = s0->dma;
	if (s0_is_planar(j.s0_format)) {
		u32 px = j.s0_width * j.s0_height;

		u = y + px;
		v = u + (j.s0_format == PXP_S0_YUV420 ? px / 4 : px / 2);
	}

	mutex_lock(&pxp->hw_lock);

	ret = pxp_reset(pxp);
	if (ret)
		goto out_unlock_hw;

	pxp_write(pxp, out->dma, HW_PXP_OUTBUF);
	pxp_write(pxp, 0, HW_PXP_OUTBUF2);
	pxp_write(pxp, OUTSIZE(0xff, osize_w, osize_h), HW_PXP_OUTSIZE);

	pxp_write(pxp, y, HW_PXP_S0BUF);
	pxp_write(pxp, u, HW_PXP_S0UBUF);
	pxp_write(pxp, v, HW_PXP_S0VBUF);
	pxp_write(pxp, BLK(j.dst_x >> 3, j.dst_y >> 3,
			   j.s0_width >> 3, j.s0_height >> 3), HW_PXP_S0PARAM);
	pxp_write(pxp, BLK(j.src_x >> 3, j.src_y >> 3,
			   j.dst_w >> 3, j.dst_h >> 3), HW_PXP_S0CROP);
	pxp_write(pxp, (yscale << 16) | xscale, HW_PXP_S0SCALE);
	pxp_write(pxp, 0, HW_PXP_S0OFFSET);
	pxp_write(pxp, j.background, HW_PXP_S0BACKGROUND);

	/* color key disattivati (low > high) */
	pxp_write(pxp, 0xffffff, HW_PXP_S0COLORKEYLOW);
	pxp_write(pxp, 0, HW_PXP_S0COLORKEYHIGH);
	pxp_write(pxp, 0xffffff, HW_PXP_OLCOLORKEYLOW);
	pxp_write(pxp, 0, HW_PXP_OLCOLORKEYHIGH);

	/* nessun overlay */
	for (i = 0; i < PXP_NUM_OVERLAYS; i++)
		pxp_write(pxp, 0, HW_PXP_OLnPARAM(i));

	/* conversione colore, valori del BSP Freescale */
	if (j.flags & PXP_FLAG_YUV_FULL) {
		pxp_write(pxp, 0x04030000, HW_PXP_CSCCOEFF0);
		pxp_write(pxp, 0x01230208, HW_PXP_CSCCOEFF1);
		pxp_write(pxp, 0x076b079b, HW_PXP_CSCCOEFF2);
	} else {
		pxp_write(pxp, 0x84ab01f0, HW_PXP_CSCCOEFF0);
		pxp_write(pxp, 0x01230204, HW_PXP_CSCCOEFF1);
		pxp_write(pxp, 0x0730079c, HW_PXP_CSCCOEFF2);
	}

	ctrl = CTRL_S0_FORMAT(j.s0_format) | CTRL_OUTBUF_FORMAT(j.out_format) |
	       CTRL_ROTATE(rot) | CTRL_CROP | CTRL_IRQ_ENABLE;
	if (xscale != SCALE_ONE || yscale != SCALE_ONE)
		ctrl |= CTRL_SCALE;
	if (j.flags & PXP_FLAG_HFLIP)
		ctrl |= CTRL_HFLIP;
	if (j.flags & PXP_FLAG_VFLIP)
		ctrl |= CTRL_VFLIP;

	pxp_write(pxp, STAT_ALL_IRQS, HW_PXP_STAT_CLR);
	reinit_completion(&pxp->done);
	pxp->last_stat = 0;

	/* le scritture nei buffer write-combine devono arrivare in RAM */
	wmb();

	t0 = ktime_get();
	pxp_write(pxp, ctrl, HW_PXP_CTRL);
	pxp_write(pxp, ctrl | CTRL_ENABLE, HW_PXP_CTRL);

	tmo = msecs_to_jiffies(j.timeout_ms ? j.timeout_ms : 1000);
	left = wait_for_completion_interruptible_timeout(&pxp->done, tmo);
	j.elapsed_us = ktime_us_delta(ktime_get(), t0);

	if (left <= 0) {
		j.stat = pxp_read(pxp, HW_PXP_STAT);
		pxp->jobs_timeout++;
		dev_warn(pxp->misc.this_device,
			 "lavoro non completato: ctrl=%08x stat=%08x\n",
			 pxp_read(pxp, HW_PXP_CTRL), j.stat);
		pxp_reset(pxp);
		ret = left ? (int)left : -ETIMEDOUT;
	} else {
		j.stat = pxp->last_stat;
		pxp->jobs_done++;
		ret = (j.stat & (STAT_AXI_READ_ERROR | STAT_AXI_WRITE_ERROR)) ?
		      -EIO : 0;
	}

out_unlock_hw:
	mutex_unlock(&pxp->hw_lock);
out_unlock_file:
	mutex_unlock(&pf->lock);

	if (copy_to_user(uarg, &j, sizeof(j)))
		return -EFAULT;
	return ret;
}

static int pxp_ioc_info(struct pxp_info __user *uarg)
{
	struct brain_pxp *pxp = pxp_dev;
	struct pxp_info info = {
		.version = pxp_read(pxp, HW_PXP_VERSION),
		.ctrl = pxp_read(pxp, HW_PXP_CTRL),
		.stat = pxp_read(pxp, HW_PXP_STAT),
		.irq_count = pxp->irq_count,
		.jobs_done = pxp->jobs_done,
		.jobs_timeout = pxp->jobs_timeout,
	};

	return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long pxp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pxp_file *pf = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case PXP_IOC_ALLOC:
		return pxp_ioc_alloc(pf, uarg);
	case PXP_IOC_FREE:
		return pxp_ioc_free(pf, uarg);
	case PXP_IOC_RUN:
		return pxp_ioc_run(pf, uarg);
	case PXP_IOC_INFO:
		return pxp_ioc_info(uarg);
	case PXP_IOC_IMPORT:
		return pxp_ioc_import(pf, uarg);
	default:
		return -ENOTTY;
	}
}

static int pxp_open(struct inode *inode, struct file *file)
{
	struct pxp_file *pf = kzalloc(sizeof(*pf), GFP_KERNEL);

	if (!pf)
		return -ENOMEM;
	mutex_init(&pf->lock);
	file->private_data = pf;
	return 0;
}

static int pxp_release(struct inode *inode, struct file *file)
{
	struct pxp_file *pf = file->private_data;
	int i;

	for (i = 0; i < PXP_MAX_BUFFERS; i++)
		pxp_free_buf(&pf->bufs[i]);
	kfree(pf);
	return 0;
}

static const struct file_operations pxp_fops = {
	.owner		= THIS_MODULE,
	.open		= pxp_open,
	.release	= pxp_release,
	.unlocked_ioctl	= pxp_ioctl,
	.mmap		= pxp_mmap,
};

/* ---- caricamento ------------------------------------------------------ */

static int __init brain_pxp_init(void)
{
	struct platform_device_info pinfo = {
		.name = DRV_NAME,
		.id = PLATFORM_DEVID_NONE,
		.dma_mask = DMA_BIT_MASK(32),
	};
	struct device_node *np;
	struct brain_pxp *pxp;
	u32 version;
	int ret;

	np = of_find_node_by_name(NULL, "pxp");
	if (!np) {
		pr_err(DRV_NAME ": nodo pxp non trovato nel device tree\n");
		return -ENODEV;
	}

	pxp = kzalloc(sizeof(*pxp), GFP_KERNEL);
	if (!pxp) {
		ret = -ENOMEM;
		goto err_node;
	}
	mutex_init(&pxp->hw_lock);
	init_completion(&pxp->done);

	pxp->base = of_iomap(np, 0);
	if (!pxp->base) {
		ret = -ENOMEM;
		goto err_free;
	}

	pxp->irq = irq_of_parse_and_map(np, 0);
	if (!pxp->irq) {
		pr_err(DRV_NAME ": impossibile mappare l'interrupt\n");
		ret = -EINVAL;
		goto err_unmap;
	}

	pxp->pdev = platform_device_register_full(&pinfo);
	if (IS_ERR(pxp->pdev)) {
		ret = PTR_ERR(pxp->pdev);
		goto err_irqmap;
	}

	version = readl(pxp->base + HW_PXP_VERSION);
	if ((version >> 24) != 2) {
		pr_err(DRV_NAME ": versione PXP inattesa 0x%08x\n", version);
		ret = -ENODEV;
		goto err_pdev;
	}

	ret = stmp_reset_block(pxp->base + HW_PXP_CTRL);
	if (ret) {
		pr_err(DRV_NAME ": reset fallito: %d\n", ret);
		goto err_pdev;
	}
	writel(STAT_ALL_IRQS, pxp->base + HW_PXP_STAT_CLR);

	ret = request_irq(pxp->irq, pxp_irq, 0, DRV_NAME, pxp);
	if (ret)
		goto err_pdev;

	pxp_dev = pxp;
	pxp->misc.minor = MISC_DYNAMIC_MINOR;
	pxp->misc.name = "pxp";
	pxp->misc.fops = &pxp_fops;
	pxp->misc.mode = 0660;
	ret = misc_register(&pxp->misc);
	if (ret)
		goto err_irq;

	of_node_put(np);
	pr_info(DRV_NAME ": PXP v%u.%u pronto su /dev/pxp (irq %d)\n",
		version >> 24, (version >> 16) & 0xff, pxp->irq);
	return 0;

err_irq:
	pxp_dev = NULL;
	free_irq(pxp->irq, pxp);
err_pdev:
	platform_device_unregister(pxp->pdev);
err_irqmap:
	irq_dispose_mapping(pxp->irq);
err_unmap:
	iounmap(pxp->base);
err_free:
	kfree(pxp);
err_node:
	of_node_put(np);
	return ret;
}

static void __exit brain_pxp_exit(void)
{
	struct brain_pxp *pxp = pxp_dev;

	misc_deregister(&pxp->misc);
	/* ferma il blocco e lo lascia in reset/clock gated */
	writel(CTRL_SFTRST | CTRL_CLKGATE, pxp->base + HW_PXP_CTRL_SET);
	free_irq(pxp->irq, pxp);
	platform_device_unregister(pxp->pdev);
	irq_dispose_mapping(pxp->irq);
	iounmap(pxp->base);
	kfree(pxp);
	pxp_dev = NULL;
}

module_init(brain_pxp_init);
module_exit(brain_pxp_exit);

MODULE_DESCRIPTION("PXP (Pixel Pipeline) i.MX28 per SHARP Brain");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
