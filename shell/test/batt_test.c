// SPDX-License-Identifier: MIT
/*
 * batt_test.c - stima dell'autonomia (batt_est.c): finestra, minimo di dati,
 * carica che risale, rumore della carica intera, formato.
 *   make tests && build/native/batt_test
 */
#include <stdio.h>
#include <string.h>

#include "batt_est.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLITO %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	bs_est e;
	char buf[32];
	uint32_t t = 4000000000u;	/* vicino al giro del contatore a 32 bit */
	int i, m;

	bs_est_reset(&e);
	/* 1 punto ogni 3 minuti, letture ogni 30 s: 100% -> */
	for (i = 0; i <= 19 * 2; i++)
		bs_est_add(&e, t + (uint32_t)i * 30000u, 90 - i / 6);
	CHECK(bs_est_minutes(&e, t + 19 * 60000u) == -1, "meno di 20 minuti: nessuna stima");
	for (; i <= 40 * 2; i++)
		bs_est_add(&e, t + (uint32_t)i * 30000u, 90 - i / 6);
	m = bs_est_minutes(&e, t + 40 * 60000u);
	/* a 40 min la carica e' 77%, 1 punto ogni 3 min: ~231 min */
	CHECK(m >= 215 && m <= 250, "pendenza costante: %d min (atteso ~231)", m);
	CHECK(e.n <= BE_N, "al massimo un campione al minuto (%d)", e.n);
	/* la carica risale (cavo collegato e staccato): si ricomincia */
	bs_est_add(&e, t + 41 * 60000u, 85);
	CHECK(bs_est_minutes(&e, t + 41 * 60000u) == -1, "carica risalita: azzerata");
	/* carica ferma: nessuna stima */
	bs_est_reset(&e);
	for (i = 0; i < 30; i++)
		bs_est_add(&e, t + (uint32_t)i * 60000u, 50);
	CHECK(bs_est_minutes(&e, t + 30 * 60000u) == -1, "carica ferma: nessuna stima");
	/* finestra di 60 minuti: conta la pendenza recente */
	bs_est_reset(&e);
	for (i = 0; i < 120; i++)	/* prima ora lenta (1 punto/10 min), poi veloce (1/2 min) */
		bs_est_add(&e, t + (uint32_t)i * 60000u, i < 60 ? 80 - i / 10 : 74 - (i - 60) / 2);
	m = bs_est_minutes(&e, t + 119 * 60000u);
	CHECK(m >= 70 && m <= 100, "solo l'ultima ora: %d min (atteso ~88)", m);
	/* sospensione: un buco di 30 minuti in cui la carica cala poco */
	bs_est_reset(&e);
	for (i = 0; i < 20; i++)
		bs_est_add(&e, t + (uint32_t)i * 60000u, 60 - i / 4);
	bs_est_add(&e, t + 50 * 60000u, 54);
	m = bs_est_minutes(&e, t + 50 * 60000u);
	CHECK(m > 0, "con la sospensione in mezzo c'e' una stima (%d)", m);
	bs_est_format(buf, sizeof(buf), 200);
	CHECK(!strcmp(buf, "3 h 20 min"), "formato %s", buf);
	bs_est_format(buf, sizeof(buf), 45);
	CHECK(!strcmp(buf, "45 min"), "formato %s", buf);
	bs_est_format(buf, sizeof(buf), 720);
	CHECK(!strcmp(buf, "12 h"), "formato %s", buf);
	printf("batt_test: %d controlli, %d falliti\n", checks, fails);
	return fails != 0;
}
