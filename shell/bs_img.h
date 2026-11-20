// SPDX-License-Identifier: MIT
/*
 * bs_img.h - immagini per Galleria ed Esplora: decodifica (JPEG, PNG, BMP,
 * GIF), riduzione con filtro a media d'area, conversione in RGB565 con
 * dithering ordinato, miniature con cache su disco.
 *
 * Tutte le funzioni sono rientranti e non toccano bui: sono fatte per girare
 * nel filo di lavoro (bs_work). Solo interi.
 */
#ifndef BS_IMG_H
#define BS_IMG_H

#include "bui.h"

enum {
	BS_IMG_OK,
	BS_IMG_E_OPEN,		/* file illeggibile */
	BS_IMG_E_FORMAT,	/* non e' un'immagine che sappiamo leggere, o e' rovinata */
	BS_IMG_E_TOOBIG,	/* troppi pixel per la memoria del Brain */
	BS_IMG_E_MEM,
	BS_IMG_E_CANCEL,
};

#define BS_IMG_FIT	0	/* tutta l'immagine dentro il riquadro, mai ingrandita */
#define BS_IMG_COVER	1	/* riempie il riquadro, ritagliando al centro (miniature) */

#define BS_IMG_MAX_PIXELS	(40 * 1000 * 1000)	/* oltre: rifiutata */
/* PNG, BMP, JPEG progressivi: vanno decodificati per intero in memoria */
#define BS_IMG_MAX_PIXELS_FULL	(4 * 1000 * 1000)
#define BS_IMG_MAX_PIXELS_GIF	(2 * 1000 * 1000)

#define BS_THUMB 160		/* lato delle miniature (quadrate) */

typedef struct bs_img {
	bui_px *px;		/* w * h pixel, da liberare con bs_img_free() */
	int w, h;
	int src_w, src_h;	/* dimensioni originali (gia' ruotate secondo EXIF) */
} bs_img;

/* vero se l'estensione e' di un formato leggibile (jpg jpeg png bmp gif) */
int bs_img_is_image(const char *name);
/* decodifica e adatta al riquadro; cancel puo' essere NULL */
int bs_img_load(const char *path, int box_w, int box_h, int mode, const volatile int *cancel,
		bs_img *out);
void bs_img_free(bs_img *im);
/* miniatura BS_THUMB x BS_THUMB: dalla cache se c'e', altrimenti decodifica e la salva */
int bs_img_thumb(const char *path, long long mtime, long long size, const volatile int *cancel,
		 bs_img *out);
/* nome del file di cache (16 cifre esadecimali) per path + mtime + size */
unsigned long long bs_img_thumb_key(const char *path, long long mtime, long long size);
/* cancella dalla cache i file la cui chiave non e' in keys[] (ordinate) */
void bs_img_thumb_prune(const unsigned long long *keys, int n, const volatile int *cancel);
/* messaggio in italiano per un codice di errore */
const char *bs_img_strerror(int err);

#endif
