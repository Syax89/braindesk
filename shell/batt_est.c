// SPDX-License-Identifier: MIT
/*
 * batt_est.c - stima dell'autonomia della batteria (vedi batt_est.h).
 */
#include <stdio.h>
#include <string.h>

#include "batt_est.h"

void bs_est_reset(bs_est *e)
{
	memset(e, 0, sizeof(*e));
}

static int idx(const bs_est *e, int i)	/* i-esimo dal piu' vecchio */
{
	return (e->head - e->n + i + BE_N) % BE_N;
}

void bs_est_add(bs_est *e, uint32_t t, int cap)
{
	int last;

	if (cap < 0 || cap > 100)
		return;
	if (e->n) {
		last = idx(e, e->n - 1);
		if (cap > e->cap[last] + 1) {
			bs_est_reset(e);	/* la carica e' salita: si ricomincia */
		} else if ((int32_t)(t - e->t[last]) < 60000) {
			e->cap[last] = (uint8_t)cap;	/* stesso minuto: l'ultimo valore */
			return;
		}
	}
	e->t[e->head] = t;
	e->cap[e->head] = (uint8_t)cap;
	e->head = (e->head + 1) % BE_N;
	if (e->n < BE_N)
		e->n++;
}

int bs_est_minutes(const bs_est *e, uint32_t now)
{
	long long st = 0, sc = 0, stt = 0, stc = 0, num, den, m;
	int i, k = 0, first = -1, lastc = -1;
	uint32_t t0 = 0, span = 0;

	for (i = 0; i < e->n; i++) {
		int j = idx(e, i);
		long long t;

		if ((int32_t)(now - e->t[j]) > BE_WINDOW_MIN * 60000)
			continue;	/* fuori dalla finestra */
		if (first < 0) {
			first = j;
			t0 = e->t[j];
		}
		t = (long long)(uint32_t)(e->t[j] - t0) / 1000;	/* secondi */
		st += t;
		sc += e->cap[j];
		stt += t * t;
		stc += t * e->cap[j];
		span = e->t[j] - t0;
		lastc = e->cap[j];
		k++;
	}
	if (k < 3 || span < (uint32_t)BE_MIN_SPAN_MIN * 60000u)
		return -1;
	num = k * stc - st * sc;	/* pendenza = num / den (punti al secondo) */
	den = k * stt - st * st;
	if (num >= 0 || den <= 0)
		return -1;		/* non scende */
	/* minuti = carica / (-pendenza) / 60 */
	m = (long long)lastc * den / (-num) / 60;
	return m > 99 * 60 ? -1 : (int)m;
}

void bs_est_format(char *buf, int len, int min)
{
	if (min < 0)
		snprintf(buf, (size_t)len, "\xE2\x80\x94");
	else if (min < 60)
		snprintf(buf, (size_t)len, "%d min", min);
	else if (min % 60 == 0 || min >= 600)
		snprintf(buf, (size_t)len, "%d h", min / 60);
	else
		snprintf(buf, (size_t)len, "%d h %d min", min / 60, min % 60);
}
