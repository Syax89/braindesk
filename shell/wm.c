// SPDX-License-Identifier: MIT
/*
 * wm.c - collaborazione col gestore di finestre (JWM) tramite EWMH:
 * elenco delle finestre esterne, attivazione, chiusura, "mostra la home".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "bui_x.h"
#include "shell.h"

enum {
	A_CLIENT_LIST, A_ACTIVE_WINDOW, A_CLOSE_WINDOW, A_WM_NAME, A_UTF8,
	A_SHOWING_DESKTOP, A_SUPPORTING_WM_CHECK, A_WINDOW_TYPE, A_TYPE_DOCK, A_TYPE_DESKTOP,
	A_TYPE_SPLASH, A_TYPE_MENU, A_TYPE_NOTIFICATION, A_TYPE_TOOLTIP, A_STATE,
	A_STATE_SKIP_TASKBAR, A_COUNT
};
static const char *const names[A_COUNT] = {
	"_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW", "_NET_CLOSE_WINDOW", "_NET_WM_NAME", "UTF8_STRING",
	"_NET_SHOWING_DESKTOP", "_NET_SUPPORTING_WM_CHECK", "_NET_WM_WINDOW_TYPE",
	"_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP", "_NET_WM_WINDOW_TYPE_SPLASH",
	"_NET_WM_WINDOW_TYPE_MENU", "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_TOOLTIP",
	"_NET_WM_STATE", "_NET_WM_STATE_SKIP_TASKBAR",
};

static struct {
	Display *d;
	Window root;
	Atom a[A_COUNT];
	bs_client clients[BS_MAX_CLIENTS];
	int n;
	Window active;
	Window own[8];
	int nown;
	bui_win *home, *bar;
	int verify_timer, guard_timer;
	int wm_ok;		/* c'e' un gestore EWMH */
} W;

static void *get_prop(Window w, Atom prop, Atom type, unsigned long *n)
{
	Atom at;
	int fmt;
	unsigned long after;
	unsigned char *data = NULL;

	*n = 0;
	if (XGetWindowProperty(W.d, w, prop, 0, 4096, False, type, &at, &fmt, n, &after, &data) != Success ||
	    !data)
		return NULL;
	if (!*n) {
		XFree(data);
		return NULL;
	}
	return data;
}

static int is_own(Window w)
{
	int i;

	for (i = 0; i < W.nown; i++)
		if (W.own[i] == w)
			return 1;
	return 0;
}

void bs_wmx_own(bui_win *w)
{
	if (w && W.nown < 8)
		W.own[W.nown++] = bui_x_window(w);
}

void bs_wmx_set_home(bui_win *home, bui_win *bar)
{
	W.home = home;
	W.bar = bar;
}

static int has_wm(void)
{
	unsigned long n;
	void *p = get_prop(W.root, W.a[A_SUPPORTING_WM_CHECK], XA_WINDOW, &n);

	if (p)
		XFree(p);
	return p != NULL;
}

/* la finestra merita una linguetta nella barra? */
static int wanted(Window w)
{
	unsigned long n, i;
	Atom *at;
	int ok = 1;

	at = get_prop(w, W.a[A_WINDOW_TYPE], XA_ATOM, &n);
	for (i = 0; at && i < n; i++)
		if (at[i] == W.a[A_TYPE_DOCK] || at[i] == W.a[A_TYPE_DESKTOP] ||
		    at[i] == W.a[A_TYPE_SPLASH] || at[i] == W.a[A_TYPE_MENU] ||
		    at[i] == W.a[A_TYPE_NOTIFICATION] || at[i] == W.a[A_TYPE_TOOLTIP])
			ok = 0;
	if (at)
		XFree(at);
	at = get_prop(w, W.a[A_STATE], XA_ATOM, &n);
	for (i = 0; at && i < n; i++)
		if (at[i] == W.a[A_STATE_SKIP_TASKBAR])
			ok = 0;
	if (at)
		XFree(at);
	return ok;
}

static void read_title(bs_client *c)
{
	unsigned long n, i;
	unsigned char *s;
	size_t o = 0;

	c->title[0] = 0;
	s = get_prop(c->id, W.a[A_WM_NAME], W.a[A_UTF8], &n);
	if (s) {
		if (n >= sizeof(c->title))
			n = sizeof(c->title) - 1;
		/* non spezzare un carattere UTF-8 a meta' */
		while (n > 0 && (s[n] & 0xC0) == 0x80)
			n--;
		memcpy(c->title, s, n);
		c->title[n] = 0;
		XFree(s);
	} else if ((s = get_prop(c->id, XA_WM_NAME, XA_STRING, &n)) != NULL) {
		/* Latin-1 -> UTF-8 */
		for (i = 0; i < n && o + 3 < sizeof(c->title); i++) {
			if (s[i] < 0x80) {
				c->title[o++] = (char)s[i];
			} else {
				c->title[o++] = (char)(0xC0 | (s[i] >> 6));
				c->title[o++] = (char)(0x80 | (s[i] & 0x3F));
			}
		}
		c->title[o] = 0;
		XFree(s);
	}
	if (!c->title[0]) {
		XClassHint ch = { NULL, NULL };

		if (XGetClassHint(W.d, c->id, &ch)) {
			snprintf(c->title, sizeof(c->title), "%s", ch.res_class ? ch.res_class : "");
			if (ch.res_name)
				XFree(ch.res_name);
			if (ch.res_class)
				XFree(ch.res_class);
		}
	}
	if (!c->title[0])
		snprintf(c->title, sizeof(c->title), "Finestra");
}

static void read_active(void)
{
	unsigned long n;
	Window *p = get_prop(W.root, W.a[A_ACTIVE_WINDOW], XA_WINDOW, &n);

	W.active = p ? p[0] : None;
	if (p)
		XFree(p);
}

static void read_clients(void)
{
	unsigned long n, i;
	Window *list = get_prop(W.root, W.a[A_CLIENT_LIST], XA_WINDOW, &n);
	bs_client next[BS_MAX_CLIENTS];
	int nn = 0, j;

	for (i = 0; list && i < n && nn < BS_MAX_CLIENTS; i++) {
		Window w = list[i];
		bs_client *old = NULL;

		if (is_own(w))
			continue;
		for (j = 0; j < W.n; j++)
			if (W.clients[j].id == w)
				old = &W.clients[j];
		if (old) {
			next[nn++] = *old;
			continue;
		}
		if (!wanted(w))
			continue;
		XSelectInput(W.d, w, PropertyChangeMask);
		next[nn].id = w;
		read_title(&next[nn]);
		nn++;
	}
	if (list)
		XFree(list);
	memcpy(W.clients, next, sizeof(next[0]) * (size_t)nn);
	W.n = nn;
	bui_x_error_pending();	/* finestre sparite nel frattempo: errori da scartare */
}

void bs_wmx_refresh(void)
{
	W.wm_ok = has_wm();
	read_clients();
	read_active();
	bs_notify(BS_EV_WINDOWS);
}

int bs_wmx_clients(const bs_client **list)
{
	*list = W.clients;
	return W.n;
}

unsigned long bs_wmx_active(void)
{
	return W.active;
}

static void send_root(Window w, Atom type, long l0, long l1, long l2)
{
	XEvent e;

	memset(&e, 0, sizeof(e));
	e.xclient.type = ClientMessage;
	e.xclient.window = w;
	e.xclient.message_type = type;
	e.xclient.format = 32;
	e.xclient.data.l[0] = l0;
	e.xclient.data.l[1] = l1;
	e.xclient.data.l[2] = l2;
	XSendEvent(W.d, W.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
}

void bs_wmx_activate(unsigned long id)
{
	/* 2 = richiesta da un pager/barra: il gestore deve obbedire */
	send_root(id, W.a[A_ACTIVE_WINDOW], 2, (long)bui_x_time(), 0);
}

void bs_wmx_close(unsigned long id)
{
	send_root(id, W.a[A_CLOSE_WINDOW], (long)bui_x_time(), 2, 0);
}

/*
 * I tasti devono arrivare alla home ogni volta che nessuna finestra esterna
 * e' attiva. JWM, quando riduce a icona o chiude la finestra attiva, lascia
 * il fuoco sulla radice (e le sue richieste portano un'ora vecchia, quindi
 * non si puo' contare che lo sistemi lui): poco dopo ogni cambiamento
 * controlliamo e, se serve, lo riprendiamo.
 */
static void focus_guard(void *user)
{
	Window h, f = None;
	int revert;

	(void)user;
	W.guard_timer = 0;
	if (!W.home || !W.home->mapped)
		return;
	h = bui_x_window(W.home);
	if (W.active != None && W.active != h)
		return;		/* una finestra esterna ha il fuoco: va bene cosi' */
	XGetInputFocus(W.d, &f, &revert);
	if (f != h)
		XSetInputFocus(W.d, h, RevertToPointerRoot, CurrentTime);
	bui_x_error_pending();
}

static void arm_focus_guard(void)
{
	if (W.guard_timer)
		bui_timer_del(W.guard_timer);
	W.guard_timer = bui_timer_add(200, 0, focus_guard, NULL);
}

void bs_wmx_focus_home(void)
{
	if (!W.home || !W.home->mapped)
		return;
	XSetInputFocus(W.d, bui_x_window(W.home), RevertToPointerRoot, CurrentTime);
	arm_focus_guard();
}

/*
 * Dopo aver chiesto "mostra il desktop" controlla che sia successo davvero:
 * JWM tratta il messaggio come un interruttore e non azzera lo stato quando
 * si apre una nuova finestra, altri gestori lo ignorano. Cio' che e' ancora
 * visibile viene ridotto a icona una finestra alla volta.
 */
static void verify_home(void *user)
{
	int i;

	(void)user;
	W.verify_timer = 0;
	for (i = 0; i < W.n; i++) {
		XWindowAttributes wa;

		if (XGetWindowAttributes(W.d, W.clients[i].id, &wa) && wa.map_state == IsViewable)
			XIconifyWindow(W.d, W.clients[i].id, DefaultScreen(W.d));
	}
	bs_wmx_focus_home();
	bui_x_error_pending();
}

void bs_wmx_show_home(void)
{
	unsigned long n;
	long *p;
	int showing = 0;

	W.wm_ok = has_wm();
	if (!W.wm_ok) {
		/* senza gestore di finestre basta rialzare le nostre */
		if (W.home)
			XRaiseWindow(W.d, bui_x_window(W.home));
		if (W.bar)
			XRaiseWindow(W.d, bui_x_window(W.bar));
		bs_wmx_focus_home();
		return;
	}
	p = get_prop(W.root, W.a[A_SHOWING_DESKTOP], XA_CARDINAL, &n);
	if (p) {
		showing = p[0] != 0;
		XFree(p);
	}
	if (!showing && W.n > 0)
		send_root(W.root, W.a[A_SHOWING_DESKTOP], 1, 0, 0);
	if (!W.verify_timer)
		W.verify_timer = bui_timer_add(W.n > 0 ? 150 : 0, 0, verify_home, NULL);
}

static void wm_hook(XEvent *e)
{
	int i;

	if (e->type == ConfigureNotify && e->xconfigure.window == W.root) {
		bs_bar_screen_changed();
		bs_home_screen_changed();
		return;
	}
	if (e->type != PropertyNotify)
		return;
	if (e->xproperty.window == W.root) {
		if (e->xproperty.atom == W.a[A_SUPPORTING_WM_CHECK]) {
			W.wm_ok = has_wm();
			bs_wmx_refresh();
		} else if (e->xproperty.atom == W.a[A_CLIENT_LIST]) {
			read_clients();
			bs_notify(BS_EV_WINDOWS);
		} else if (e->xproperty.atom == W.a[A_ACTIVE_WINDOW]) {
			Window prev = W.active;

			read_active();
			arm_focus_guard();
			if (W.active != prev)
				bs_notify(BS_EV_WINDOWS);
		}
		return;
	}
	if (e->xproperty.atom != W.a[A_WM_NAME] && e->xproperty.atom != XA_WM_NAME)
		return;
	for (i = 0; i < W.n; i++)
		if (W.clients[i].id == e->xproperty.window) {
			char old[sizeof(W.clients[i].title)];

			memcpy(old, W.clients[i].title, sizeof(old));
			read_title(&W.clients[i]);
			bui_x_error_pending();
			if (strcmp(old, W.clients[i].title))
				bs_notify(BS_EV_WINDOWS);
			return;
		}
}

void bs_wmx_init(void)
{
	W.d = bui_x_display();
	W.root = bui_x_root();
	XInternAtoms(W.d, (char **)names, A_COUNT, False, W.a);
	XSelectInput(W.d, W.root, PropertyChangeMask | StructureNotifyMask);
	bui_x_set_hook(wm_hook);
}
