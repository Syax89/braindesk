// SPDX-License-Identifier: MIT
/*
 * wmsel.c - sceglie, secondo il backend, chi gestisce le finestre esterne
 * e l'inattivita':
 *
 *   x11      wm.c (EWMH con JWM) e idle.c (contatore IDLETIME di SYNC)
 *   drm/mem  nessuna finestra esterna visibile insieme alla shell: un'app
 *            esterna prende tutto lo schermo in una sessione X sua
 *            (xapp.c); l'inattivita' si misura sugli eventi evdev (idle_ev.c)
 *
 * Il binario senza X11 (brainshell) non contiene wm.c e idle.c.
 */
#include "shell.h"

#ifdef BUI_HAVE_X11
#define X11 bui_backend_is_x11()
#else
#define X11 0
#define bs_wmx_init() ((void)0)
#define bs_wmx_own(w) ((void)(w))
#define bs_wmx_set_home(h, b) ((void)(h), (void)(b))
#define bs_wmx_refresh() ((void)0)
#define bs_wmx_clients(l) (*(l) = NULL, 0)
#define bs_wmx_active() 0ul
#define bs_wmx_activate(id) ((void)(id))
#define bs_wmx_close(id) ((void)(id))
#define bs_wmx_show_home() ((void)0)
#define bs_wmx_focus_home() ((void)0)
#define bs_idlex_init() ((void)0)
#define bs_idlex_configure() ((void)0)
#define bs_idlex_shutdown() ((void)0)
#endif

void bs_wm_init(void)
{
	if (X11)
		bs_wmx_init();
}

void bs_wm_own(bui_win *w)
{
	if (X11)
		bs_wmx_own(w);
}

void bs_wm_set_home(bui_win *home, bui_win *bar)
{
	if (X11)
		bs_wmx_set_home(home, bar);
}

void bs_wm_refresh(void)
{
	if (X11)
		bs_wmx_refresh();
	else
		bs_notify(BS_EV_WINDOWS);
}

int bs_wm_clients(const bs_client **list)
{
	/* senza X le app esterne non condividono lo schermo con la barra */
	if (X11)
		return bs_wmx_clients(list);
	*list = NULL;
	return 0;
}

unsigned long bs_wm_active(void)
{
	return X11 ? bs_wmx_active() : 0;
}

void bs_wm_activate(unsigned long id)
{
	if (X11)
		bs_wmx_activate(id);
}

void bs_wm_close(unsigned long id)
{
	if (X11)
		bs_wmx_close(id);
}

void bs_wm_show_home(void)
{
	if (X11)
		bs_wmx_show_home();
}

void bs_wm_focus_home(void)
{
	if (X11)
		bs_wmx_focus_home();
}

void bs_idle_init(void)
{
	if (X11)
		bs_idlex_init();
	else
		bs_idlev_init();
}

void bs_idle_configure(void)
{
	if (X11)
		bs_idlex_configure();
	else
		bs_idlev_configure();
}

void bs_idle_shutdown(void)
{
	if (X11)
		bs_idlex_shutdown();
	else
		bs_idlev_shutdown();
}
