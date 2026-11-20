// SPDX-License-Identifier: MIT
/*
 * idle.c - schermo attenuato e spento dopo un periodo di inattivita'.
 *
 * L'inattivita' e' quella di tutto il server X, non solo delle nostre
 * finestre: il contatore di sistema IDLETIME dell'estensione SYNC (in
 * libXext, nessuna libreria in piu'). Tre allarmi fanno tutto il lavoro: due
 * scattano quando l'inattivita' supera le soglie, uno quando torna a zero.
 * Nessun timer nostro e nessuna interrogazione periodica: a riposo la shell
 * non viene svegliata, tranne nell'istante in cui lo schermo cambia stato.
 *
 * A schermo spento una finestra InputOnly copre tutto: il primo tocco (o
 * tasto) arriva a lei, riaccende e viene scartato, cosi' non attiva cio' che
 * c'e' sotto, nemmeno in un'app esterna.
 *
 * Senza SYNC (server minimali) si ripiega su un timer che guarda solo
 * l'attivita' sulle finestre della shell.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/sync.h>

#include "bui_x.h"
#include "shell.h"

static struct {
	Display *d;
	int have_sync, ev_base;
	XSyncCounter counter;
	XSyncAlarm a_dim, a_off, a_wake;
	Window shield;		/* copre lo schermo spento */
	int shield_up, shield_down;
	int dim_ms, off_ms;
	/* ripiego senza SYNC */
	uint32_t last_input;
	int timer;
} I;

static void shield_hide(void)
{
	if (!I.shield_up)
		return;
	XUngrabKeyboard(I.d, CurrentTime);
	XUnmapWindow(I.d, I.shield);
	I.shield_up = I.shield_down = 0;
}

static void shield_show(void)
{
	XSetWindowAttributes a;

	if (!I.shield) {
		a.override_redirect = True;
		a.event_mask = ButtonPressMask | ButtonReleaseMask | KeyPressMask;
		I.shield = XCreateWindow(I.d, bui_x_root(), 0, 0, 1, 1, 0, 0, InputOnly, CopyFromParent,
					 CWOverrideRedirect | CWEventMask, &a);
	}
	XMoveResizeWindow(I.d, I.shield, 0, 0, (unsigned)bui_screen_w(), (unsigned)bui_screen_h());
	XMapRaised(I.d, I.shield);
	XGrabKeyboard(I.d, I.shield, False, GrabModeAsync, GrabModeAsync, CurrentTime);
	I.shield_up = 1;
	I.shield_down = 0;
}

static void fallback_arm(int ms);

static void set_state(int st)
{
	if (st == bs_screen_state())
		return;
	if (st == BS_SCREEN_OFF)
		shield_show();
	bs_screen_set(st);
	if (!I.have_sync && st == BS_SCREEN_ON)
		fallback_arm(0);
}

static void wake(void)
{
	/* lo scudo resta finche' il dito non si alza: anche il rilascio va scartato */
	if (I.shield_up && !I.shield_down)
		shield_hide();
	set_state(BS_SCREEN_ON);
}

/* ---- SYNC ---- */

static XSyncAlarm alarm_make(int ms, int test)
{
	XSyncAlarmAttributes at;

	memset(&at, 0, sizeof(at));
	at.trigger.counter = I.counter;
	at.trigger.value_type = XSyncAbsolute;
	at.trigger.test_type = test;
	XSyncIntToValue(&at.trigger.wait_value, ms);
	XSyncIntToValue(&at.delta, 0);
	at.events = True;
	return XSyncCreateAlarm(I.d, XSyncCACounter | XSyncCAValueType | XSyncCATestType | XSyncCAValue |
				XSyncCADelta | XSyncCAEvents, &at);
}

static void alarms_clear(void)
{
	if (I.a_dim)
		XSyncDestroyAlarm(I.d, I.a_dim);
	if (I.a_off)
		XSyncDestroyAlarm(I.d, I.a_off);
	if (I.a_wake)
		XSyncDestroyAlarm(I.d, I.a_wake);
	I.a_dim = I.a_off = I.a_wake = None;
}

static int ext_hook(XEvent *e)
{
	if (I.shield && e->xany.window == I.shield &&
	    (e->type == ButtonPress || e->type == ButtonRelease || e->type == KeyPress)) {
		if (e->type == ButtonPress) {
			I.shield_down = 1;
			set_state(BS_SCREEN_ON);
		} else {
			I.shield_down = 0;
			wake();
		}
		return 1;
	}
	if (I.have_sync && e->type == I.ev_base + XSyncAlarmNotify) {
		XSyncAlarmNotifyEvent *ae = (XSyncAlarmNotifyEvent *)e;

		if (ae->alarm == I.a_off && I.a_off)
			set_state(BS_SCREEN_OFF);
		else if (ae->alarm == I.a_dim && I.a_dim && bs_screen_state() == BS_SCREEN_ON)
			set_state(BS_SCREEN_DIM);
		else if (ae->alarm == I.a_wake && I.a_wake)
			wake();
		return 1;
	}
	return 0;
}

/* ---- ripiego: solo l'attivita' sulle nostre finestre ---- */

static void fallback_fire(void *user)
{
	int idle = (int32_t)(bui_now() - I.last_input);

	(void)user;
	I.timer = 0;
	if (I.off_ms && idle >= I.off_ms) {
		set_state(BS_SCREEN_OFF);
		return;
	}
	if (I.dim_ms && idle >= I.dim_ms && bs_screen_state() == BS_SCREEN_ON)
		set_state(BS_SCREEN_DIM);
	fallback_arm(idle);
}

/* un solo timer, riarmato quando scade: l'attivita' annota soltanto l'ora */
static void fallback_arm(int idle)
{
	int next = -1;

	if (I.timer)
		bui_timer_del(I.timer);
	I.timer = 0;
	if (I.dim_ms && idle < I.dim_ms)
		next = I.dim_ms - idle;
	else if (I.off_ms && idle < I.off_ms)
		next = I.off_ms - idle;
	if (next >= 0)
		I.timer = bui_timer_add(next + 20, 0, fallback_fire, NULL);
}

/* ogni tocco o tasto sulle nostre finestre passa di qui */
static int input_filter(int kind, const bui_key *k, int suspended)
{
	(void)kind;
	(void)k;
	(void)suspended;
	I.last_input = bui_now();
	if (bs_screen_state() == BS_SCREEN_DIM)
		wake();		/* subito, senza aspettare l'allarme; il tocco vale */
	return 0;
}

void bs_idlex_configure(void)
{
	const char *test = getenv("BRAINSHELL_IDLE_MS");	/* "attenua,spegni" in ms: solo per le prove */
	int lo;

	I.dim_ms = bs_pref.dim_s * 1000;
	I.off_ms = bs_pref.off_s * 1000;
	if (test)
		sscanf(test, "%d,%d", &I.dim_ms, &I.off_ms);
	/* spegnere prima di attenuare non ha senso: vince lo spegnimento */
	if (I.off_ms && I.dim_ms >= I.off_ms)
		I.dim_ms = 0;
	if (!I.d)
		return;
	if (!I.dim_ms && !I.off_ms)
		wake();
	if (!I.have_sync) {
		fallback_arm((int32_t)(bui_now() - I.last_input));
		return;
	}
	alarms_clear();
	lo = I.dim_ms ? I.dim_ms : I.off_ms;
	if (!lo)
		return;
	if (I.dim_ms)
		I.a_dim = alarm_make(I.dim_ms, XSyncPositiveTransition);
	if (I.off_ms)
		I.a_off = alarm_make(I.off_ms, XSyncPositiveTransition);
	/* l'inattivita' torna sotto la soglia piu' bassa: qualcuno ha toccato qualcosa */
	I.a_wake = alarm_make(lo, XSyncNegativeTransition);
}

void bs_idlex_init(void)
{
	XSyncSystemCounter *list;
	int n = 0, i, err, maj = 3, min = 0;

	I.d = bui_x_display();
	I.last_input = bui_now();
	if (!getenv("BRAINSHELL_NO_SYNC") && XSyncQueryExtension(I.d, &I.ev_base, &err) &&
	    XSyncInitialize(I.d, &maj, &min)) {
		list = XSyncListSystemCounters(I.d, &n);
		for (i = 0; list && i < n; i++)
			if (!strcmp(list[i].name, "IDLETIME")) {
				I.counter = list[i].counter;
				I.have_sync = 1;
			}
		if (list)
			XSyncFreeSystemCounterList(list);
	}
	bui_x_set_ext_hook(ext_hook);
	bui_add_input_filter(input_filter);
	bs_idlex_configure();
}

void bs_idlex_shutdown(void)
{
	if (!I.d)
		return;
	/* non lasciare lo schermo buio se la shell esce */
	shield_hide();
	if (bs_screen_state() != BS_SCREEN_ON)
		bs_screen_set(BS_SCREEN_ON);
	if (I.have_sync)
		alarms_clear();
	if (I.shield)
		XDestroyWindow(I.d, I.shield);
	I.shield = None;
}
