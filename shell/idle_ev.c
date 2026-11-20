// SPDX-License-Identifier: MIT
/*
 * idle_ev.c - schermo attenuato, spento e sospensione automatica per
 * inattivita', senza X.
 *
 * Tutto l'ingresso passa dalla shell (evdev), anche mentre un'app X ha lo
 * schermo: i dispositivi restano aperti senza cattura e i loro eventi
 * arrivano ai filtri del toolkit con suspended = 1. Quindi l'ora
 * dell'ultimo tocco o tasto e' gia' l'inattivita' di tutto il sistema, come
 * il contatore IDLETIME di X. Un solo timer, riarmato quando scade:
 * l'attivita' annota soltanto l'ora (nessun lavoro per evento).
 *
 * A schermo spento il primo tocco o tasto riaccende soltanto e viene
 * scartato, col resto del tocco fino al rilascio. Mentre un'app X ha lo
 * schermo lo si riaccende ma non si puo' scartare nulla: il tocco arriva
 * anche all'app.
 *
 * Sospensione automatica ("Sospendi dopo", bs_eff_sleep()): contata da
 * quando lo schermo si e' spento, oppure dall'ultimo tocco se lo
 * spegnimento e' "Mai". Solo a batteria, mai con la musica che suona o
 * un'app X aperta (bs_power_can_autosleep): finche' non si puo' si
 * ricontrolla ogni 30 s. Il tasto di accensione lo gestisce power.c.
 */
#include <stdio.h>
#include <stdlib.h>

#include "shell.h"

#define RETRY_MS 30000

static struct {
	int on;
	int dim_ms, off_ms, sleep_ms;
	uint32_t last;		/* ultimo tocco o tasto */
	uint32_t off_at;	/* quando lo schermo si e' spento */
	int timer;
	int swallow;		/* il tocco che ha riacceso: scartato fino al rilascio */
	int retry;		/* sospensione dovuta ma impedita: si ricontrolla */
} V;

static void arm(void);

static void set_state(int st)
{
	if (st == bs_screen_state())
		return;
	if (st == BS_SCREEN_OFF)
		V.off_at = bui_now();
	bs_screen_set(st);
	arm();
}

/* da quando si conta per la sospensione; 0 = non ancora */
static int sleep_base(uint32_t *base)
{
	if (!V.sleep_ms)
		return 0;
	if (V.off_ms || bs_screen_state() == BS_SCREEN_OFF) {
		if (bs_screen_state() != BS_SCREEN_OFF)
			return 0;
		*base = V.off_at;
		return 1;
	}
	*base = V.last;
	return 1;
}

static void fire(void *user)
{
	uint32_t now = bui_now(), base;
	int idle = (int32_t)(now - V.last);

	(void)user;
	V.timer = 0;
	if (bs_power_sleeping())
		return;		/* al risveglio bs_idlev_resumed() riarma */
	if (V.off_ms && idle >= V.off_ms && bs_screen_state() != BS_SCREEN_OFF)
		set_state(BS_SCREEN_OFF);
	else if (V.dim_ms && idle >= V.dim_ms && bs_screen_state() == BS_SCREEN_ON)
		set_state(BS_SCREEN_DIM);
	V.retry = 0;
	if (sleep_base(&base) && (int32_t)(now - base) >= V.sleep_ms) {
		if (bs_power_can_autosleep()) {
			bs_power_sleep(0);
			return;
		}
		V.retry = 1;	/* musica, app X, cavo: si riprova */
	}
	arm();
}

static void arm(void)
{
	uint32_t now = bui_now(), base;
	int idle = (int32_t)(now - V.last), next = -1, st = bs_screen_state(), d;

#define CAND(x) do { d = (x); if (d < 0) d = 0; if (next < 0 || d < next) next = d; } while (0)
	if (V.timer)
		bui_timer_del(V.timer);
	V.timer = 0;
	if (!V.on || bs_power_sleeping())
		return;
	if (V.dim_ms && st == BS_SCREEN_ON && idle < V.dim_ms)
		CAND(V.dim_ms - idle);
	if (V.off_ms && st != BS_SCREEN_OFF && idle < V.off_ms)
		CAND(V.off_ms - idle);
	if (sleep_base(&base)) {
		int left = V.sleep_ms - (int32_t)(now - base);

		CAND(left > 0 ? left : V.retry ? RETRY_MS : 0);
	}
	if (next >= 0)
		V.timer = bui_timer_add(next + 20, 0, fire, NULL);
#undef CAND
}

static int filter(int kind, const bui_key *k, int suspended)
{
	int st = bs_screen_state();

	if (bs_power_sleeping())
		return 0;
	/* il tasto di accensione ha regole sue (power.c) */
	if (kind == BUI_IN_KEY && k && (k->key == BUI_K_POWER || k->key == BUI_K_POWER_UP))
		return 0;
	V.last = bui_now();
	V.retry = 0;
	if (st == BS_SCREEN_OFF) {
		set_state(BS_SCREEN_ON);
		if (suspended)
			return 0;
		if (kind == BUI_PRESS)
			V.swallow = 1;
		return 1;	/* sveglia soltanto */
	}
	if (V.swallow && kind != BUI_IN_KEY) {
		if (kind == BUI_RELEASE || kind == BUI_PRESS)
			V.swallow = 0;
		if (kind != BUI_PRESS)
			return 1;
	}
	if (st == BS_SCREEN_DIM)
		set_state(BS_SCREEN_ON);	/* subito; il tocco vale */
	else if (!V.timer)
		arm();
	return 0;
}

void bs_idlev_configure(void)
{
	const char *test = getenv("BRAINSHELL_IDLE_MS");	/* "attenua,spegni[,sospendi]" in ms: prove */

	V.dim_ms = bs_eff_dim() * 1000;
	V.off_ms = bs_eff_off() * 1000;
	V.sleep_ms = bs_eff_sleep() * 1000;
	if (test)
		sscanf(test, "%d,%d,%d", &V.dim_ms, &V.off_ms, &V.sleep_ms);
	if (V.off_ms && V.dim_ms >= V.off_ms)
		V.dim_ms = 0;
	if (!V.on)
		return;
	if (!V.dim_ms && !V.off_ms && bs_screen_state() != BS_SCREEN_OFF)
		set_state(BS_SCREEN_ON);
	arm();
}

void bs_idlev_screen_off(void)
{
	V.swallow = 0;
	set_state(BS_SCREEN_OFF);
}

void bs_idlev_wake(void)
{
	V.last = bui_now();
	V.retry = 0;
	set_state(BS_SCREEN_ON);
	arm();
}

void bs_idlev_resumed(void)
{
	V.swallow = 0;
	/* brain-ctl ha gia' riacceso la retroilluminazione */
	bs_idlev_wake();
}

void bs_idlev_init(void)
{
	V.on = 1;
	V.last = bui_now();
	bui_add_input_filter(filter);
	bs_idlev_configure();
}

void bs_idlev_shutdown(void)
{
	if (!V.on)
		return;
	bui_timer_del(V.timer);
	V.timer = 0;
	if (bs_screen_state() != BS_SCREEN_ON)
		bs_screen_set(BS_SCREEN_ON);
	V.on = 0;
}
