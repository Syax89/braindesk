// SPDX-License-Identifier: MIT
/*
 * batt_est.h - stima dell'autonomia dalla pendenza della carica in una
 * finestra scorrevole (solo interi; provata da test/batt_test.c).
 *
 * Un campione (tempo, carica %) al massimo ogni minuto, gli ultimi
 * BE_WINDOW_MIN minuti. Pendenza con i minimi quadrati su tutti i campioni
 * della finestra (la carica e' in punti interi: due soli estremi darebbero
 * scalini). Nessuna stima con meno di BE_MIN_SPAN_MIN minuti di dati, se la
 * carica non scende o se la stima supera 99 ore.
 */
#ifndef BATT_EST_H
#define BATT_EST_H

#include <stdint.h>

#define BE_N 64			/* campioni (uno al minuto) */
#define BE_WINDOW_MIN 60
#define BE_MIN_SPAN_MIN 20

typedef struct bs_est {
	uint32_t t[BE_N];	/* ms (bui_now: conta anche la sospensione) */
	uint8_t cap[BE_N];
	int head, n;
} bs_est;

void bs_est_reset(bs_est *e);
/* un campione a batteria; una carica che sale (cavo, ricalibrazione) azzera */
void bs_est_add(bs_est *e, uint32_t t_ms, int cap);
/* minuti di autonomia stimati, -1 se non si sa ancora */
int bs_est_minutes(const bs_est *e, uint32_t now_ms);
/* "3 h 20 min", "45 min" */
void bs_est_format(char *buf, int len, int minutes);

#endif
