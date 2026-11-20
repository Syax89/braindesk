/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * brain-pxp: interfaccia utente per il PXP (Pixel Pipeline) v2 dell'i.MX28
 * nei dizionari SHARP Brain.
 *
 * Uso tipico:
 *   fd = open("/dev/pxp", O_RDWR);
 *   ioctl(fd, PXP_IOC_ALLOC, &a);            // buffer DMA contiguo
 *   p = mmap(NULL, a.size, ..., fd, a.mmap_offset);
 *   ioctl(fd, PXP_IOC_RUN, &job);            // esegue e aspetta l'IRQ
 */
#ifndef _UAPI_MISC_BRAIN_PXP_H
#define _UAPI_MISC_BRAIN_PXP_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PXP_MAX_BUFFERS		8

/* Formati sorgente S0 (campo CTRL.S0_FORMAT) */
#define PXP_S0_ARGB8888		0x0
#define PXP_S0_RGB888		0x1	/* 32 bit per pixel, byte alto ignorato */
#define PXP_S0_RGB565		0x4
#define PXP_S0_RGB555		0x5
#define PXP_S0_YUV422		0x8	/* planare: Y, U, V */
#define PXP_S0_YUV420		0x9	/* planare: Y, U, V */
#define PXP_S0_UYVY		0xA
#define PXP_S0_VYUY		0xB

/* Formati di uscita (campo CTRL.OUTBUF_FORMAT) */
#define PXP_OUT_ARGB8888	0x0
#define PXP_OUT_RGB888		0x1	/* 32 bit per pixel */
#define PXP_OUT_RGB888P		0x2	/* 24 bit impacchettato */
#define PXP_OUT_ARGB1555	0x3
#define PXP_OUT_RGB565		0x4
#define PXP_OUT_RGB555		0x5
#define PXP_OUT_YUV444		0x7
#define PXP_OUT_UYVY		0xA
#define PXP_OUT_VYUY		0xB

struct pxp_alloc {
	__u32 size;		/* in: byte richiesti */
	__u32 index;		/* out: indice del buffer (0..7) */
	__u64 mmap_offset;	/* out: offset da passare a mmap() */
};

/*
 * Tutte le coordinate sono in pixel e devono essere multipli di 8
 * (il PXP lavora a blocchi 8x8). Le coordinate src_* e dst_* sono
 * espresse prima della rotazione; out_width/out_height sono le
 * dimensioni fisiche del buffer di uscita.
 */
struct pxp_job {
	/* Sorgente S0 */
	__u32 s0_index;		/* buffer sorgente */
	__u32 s0_format;	/* PXP_S0_* */
	__u32 s0_width;		/* larghezza dell'immagine sorgente */
	__u32 s0_height;
	__u32 src_x, src_y;	/* origine del ritaglio nella sorgente */
	__u32 src_w, src_h;	/* dimensione del ritaglio (0 = come dst) */

	/* Uscita */
	__u32 out_index;	/* buffer di destinazione */
	__u32 out_format;	/* PXP_OUT_* */
	__u32 out_width;
	__u32 out_height;
	__u32 dst_x, dst_y;	/* dove mettere S0 nell'uscita */
	__u32 dst_w, dst_h;	/* dimensione nell'uscita (0 = tutta) */

	__u32 rotate;		/* 0, 90, 180, 270 */
	__u32 flags;		/* PXP_FLAG_* */
	__u32 background;	/* colore di sfondo fuori da S0 (RGB888) */
	__u32 timeout_ms;	/* 0 = 1000 ms */

	/* Risultati */
	__u32 stat;		/* valore di HW_PXP_STAT a fine lavoro */
	__u32 elapsed_us;	/* tempo hardware misurato */

	/*
	 * Prima riga, dentro il buffer di uscita, da cui parte l'immagine
	 * out_width x out_height (le righe sono larghe out_width pixel).
	 */
	__u32 out_y;
	__u32 reserved[3];	/* devono valere 0 */
};

/*
 * out_index = PXP_BUF_SCREEN: il PXP scrive direttamente nel frame buffer
 * che il pannello sta mostrando, nelle righe [out_y, out_y + out_height),
 * e a lavoro finito il kernel invia al pannello proprio quelle righe
 * (overlay hardware: nessuna copia fatta dalla CPU). Vincoli: out_width
 * uguale alla larghezza dello schermo (il PXP scrive righe intere e riempie
 * con "background" cio' che sta fuori dal rettangolo dst), out_format
 * PXP_OUT_RGB565, rotate 0. Il server grafico non sa di questi pixel:
 * chi li usa deve possedere quell'area di schermo e non ridisegnarla.
 */
#define PXP_BUF_SCREEN		0xffffffffu

struct pxp_screen {
	__u32 width;		/* pixel */
	__u32 height;
	__u32 pitch;		/* byte per riga */
	__u32 format;		/* PXP_OUT_RGB565 */
};

#define PXP_FLAG_HFLIP		(1 << 0)
#define PXP_FLAG_VFLIP		(1 << 1)
#define PXP_FLAG_YUV_FULL	(1 << 2)	/* coefficienti YUV invece di YCbCr */

struct pxp_info {
	__u32 version;		/* HW_PXP_VERSION */
	__u32 ctrl;		/* HW_PXP_CTRL */
	__u32 stat;		/* HW_PXP_STAT */
	__u32 irq_count;	/* interrupt ricevuti dal caricamento */
	__u32 jobs_done;
	__u32 jobs_timeout;
};

/*
 * Importa un dma-buf contiguo (ad esempio un dumb buffer DRM esportato
 * con PRIME) come buffer del PXP: cosi' il PXP scrive direttamente nella
 * memoria dello schermo. I buffer importati non si mappano con mmap()
 * su /dev/pxp: si usano dal loro driver di origine.
 */
struct pxp_import {
	__s32 fd;		/* in: file descriptor del dma-buf */
	__u32 index;		/* out: indice del buffer */
	__u32 size;		/* out: dimensione in byte */
	__u32 reserved;
};

#define PXP_IOC_MAGIC	'X'
#define PXP_IOC_ALLOC	_IOWR(PXP_IOC_MAGIC, 0, struct pxp_alloc)
#define PXP_IOC_FREE	_IOW(PXP_IOC_MAGIC, 1, __u32)
#define PXP_IOC_RUN	_IOWR(PXP_IOC_MAGIC, 2, struct pxp_job)
#define PXP_IOC_INFO	_IOR(PXP_IOC_MAGIC, 3, struct pxp_info)
#define PXP_IOC_IMPORT	_IOWR(PXP_IOC_MAGIC, 4, struct pxp_import)
#define PXP_IOC_SCREEN	_IOR(PXP_IOC_MAGIC, 5, struct pxp_screen)

#endif /* _UAPI_MISC_BRAIN_PXP_H */
