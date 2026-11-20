// SPDX-License-Identifier: MIT
/*
 * bui_core.c - nucleo del toolkit comune a tutti i backend: scelta del
 * backend, ciclo eventi, timer, animazioni, descrittori, rettangoli sporchi.
 *
 * Il ciclo dorme in poll() sui descrittori registrati (piu' quello del
 * server X col backend x11): a riposo il processo non consuma CPU. I
 * fotogrammi a ~45 fps esistono solo mentre c'e' un'animazione o un
 * trascinamento in corso.
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bui.h"
#include "bui_be.h"

#define MAX_TIMERS 24
#define MAX_ANIMS 8
#define MAX_FDS 24
#define MAX_FILTERS 6

struct timer {
	int id;
	uint32_t due;
	int period;
	void (*cb)(void *);
	void *user;
};

struct anim {
	int (*step)(void *, uint32_t);
	void *user;
};

struct fdent {
	int fd;
	void (*cb)(int, void *);
	void *user;
};

static struct {
	const bui_backend *be;
	struct timer timers[MAX_TIMERS];
	int timer_seq;
	struct anim anims[MAX_ANIMS];
	int nanim;
	uint32_t next_frame;
	struct fdent fds[MAX_FDS];
	int nfds;
	int quit;
	int suspended;
	uint32_t popup_closed;
	int (*filters[MAX_FILTERS])(int, const bui_key *, int);
	uint32_t epoch;
	int has_epoch;
	int nfilters;
	void (*query)(char *, int);
	int (*key_hook)(const bui_key *);
} C;

uint32_t bui_now(void)
{
	struct timespec ts;

	clock_gettime(BUI_CLOCK, &ts);
	return (uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* backend                                                            */
/* ------------------------------------------------------------------ */

int bui_init(void)
{
	const char *b = getenv("BRAINSHELL_BACKEND");
	const char *d = getenv("DISPLAY");

	if (!b || !*b)
		b = (!d || !*d) && access("/dev/dri/card0", R_OK | W_OK) == 0 ? "drm" : "x11";
	if (!strcmp(b, "drm"))
		C.be = &bui_be_drm;
	else if (!strcmp(b, "mem"))
		C.be = &bui_be_mem;
	else if (!strcmp(b, "x11"))
#ifdef BUI_HAVE_X11
		C.be = &bui_be_x11;
#else
		return BUI_INIT_NEED_X11;
#endif
	else {
		fprintf(stderr, "bui: backend sconosciuto \"%s\" (drm, x11, mem)\n", b);
		return -1;
	}
	if (C.be->init() < 0) {
		C.be = NULL;
		return -1;
	}
	return 0;
}

const char *bui_backend_name(void) { return C.be ? C.be->name : "none"; }
int bui_backend_is_x11(void) { return C.be && !strcmp(C.be->name, "x11"); }
int bui_screen_w(void) { return C.be->screen_w(); }
int bui_screen_h(void) { return C.be->screen_h(); }

bui_win *bui_win_create(int kind, bui_rect r, const char *name) { return C.be->win_create(kind, r, name); }
void bui_win_destroy(bui_win *w) { if (w) C.be->win_destroy(w); }
void bui_win_show(bui_win *w) { C.be->win_show(w); }
void bui_win_hide(bui_win *w) { C.be->win_hide(w); }
void bui_win_move_resize(bui_win *w, bui_rect r) { C.be->win_move_resize(w, r); }
void bui_win_set_strut_top(bui_win *w, int h) { C.be->win_set_strut_top(w, h); }
bui_win *bui_popup_open(bui_rect r, unsigned flags) { return C.be->popup_open(r, flags); }
bui_win *bui_popup_open_round(bui_rect r, int radius) { return C.be->popup_open_round(r, radius); }
void bui_popup_close(bui_win *w) { bui_win_destroy(w); }

void bui_sync(void)
{
	if (C.be && C.be->sync)
		C.be->sync();
}

int bui_suspend(int on)
{
	on = !!on;
	if (!C.be || !C.be->suspend)
		return -1;
	if (on == C.suspended)
		return 0;
	if (C.be->suspend(on) < 0)
		return -1;
	C.suspended = on;
	return 0;
}

int bui_is_suspended(void) { return C.suspended; }
int bui__suspended(void) { return C.suspended; }

void bui_quit(void)
{
	C.quit = 1;
}

void bui_shutdown(void)
{
	if (!C.be)
		return;
	C.be->shutdown();
	bui_corner_inset(-1, 0);	/* svuota la cache degli angoli */
	C.be = NULL;
}

/* ------------------------------------------------------------------ */
/* ingresso, popup                                                    */
/* ------------------------------------------------------------------ */

int bui_add_input_filter(int (*f)(int kind, const bui_key *k, int suspended))
{
	int i;

	for (i = 0; i < C.nfilters; i++)
		if (C.filters[i] == f)
			return 0;
	if (C.nfilters >= MAX_FILTERS)
		return -1;
	C.filters[C.nfilters++] = f;
	return 0;
}

int bui__input_filter(int kind, const bui_key *k, int suspended)
{
	int i, drop = 0;

	/* tutti i filtri vedono l'evento (l'inattivita' va annotata comunque) */
	for (i = 0; i < C.nfilters; i++)
		if (C.filters[i](kind, k, suspended))
			drop = 1;
	return drop;
}

void bui_set_key_hook(int (*hook)(const bui_key *k))
{
	C.key_hook = hook;
}

int bui__key_hook(const bui_key *k)
{
	if (C.key_hook && C.key_hook(k))
		return 1;
	return (k->key & BUI_K_RELEASED) != 0;	/* i rilasci non arrivano mai alle finestre */
}

void bui_win_present(bui_win *w, bui_rect r)
{
	if (!w || !C.be || !bui_rect_isect(r, bui_r(0, 0, w->w, w->h), &r))
		return;
	if (C.be->win_present)
		C.be->win_present(w, r);
	else
		bui_win_damage(w, r);
}

void bui_input_ignore_before(uint32_t t)
{
	C.epoch = t;
	C.has_epoch = 1;
}

int bui__input_stale(uint32_t t)
{
	return C.has_epoch && (int32_t)(t - C.epoch) < 0;
}

void bui_redraw_all(void)
{
	if (C.be && C.be->redraw_all)
		C.be->redraw_all();
}

void bui_anim_finish_all(void)
{
	uint32_t later = bui_now() + 600000;
	int i, n = C.nanim;

	/* ogni animazione e' funzione del tempo: molto piu' avanti e' finita */
	for (i = 0; i < n && i < C.nanim; i++)
		if (C.anims[i].step)
			C.anims[i].step(C.anims[i].user, later);
	C.nanim = 0;
}

void bui_set_state_query(void (*q)(char *buf, int len)) { C.query = q; }

/* usata dal backend mem */
void bui__state_query(char *buf, int len);
void bui__state_query(char *buf, int len)
{
	buf[0] = 0;
	if (C.query)
		C.query(buf, len);
}

void bui__popup_closed_now(void) { C.popup_closed = bui_now(); }
uint32_t bui_popup_last_close(void) { return C.popup_closed; }

void bui__dim(bui_px *px, int w, int h, int stride)
{
	int x, y;

	for (y = 0; y < h; y++) {
		bui_px *p = px + y * stride;
		const uint8_t *th = &bui_bayer8[(y & 7) << 3];

		/* 3/8 della luminosita'; il resto della divisione viene distribuito
		 * col dithering, altrimenti le sfumature scure farebbero bande */
		for (x = 0; x < w; x++) {
			unsigned v = p[x], t = th[x & 7] >> 3;
			unsigned r = (((v >> 11) & 31) * 3 + t) >> 3;
			unsigned gr = (((v >> 5) & 63) * 3 + t) >> 3;
			unsigned b = ((v & 31) * 3 + t) >> 3;

			p[x] = (bui_px)((r << 11) | (gr << 5) | b);
		}
	}
}

void bui_popup_backdrop(bui_win *w, bui_canvas *c, bui_rect clip)
{
	if (w->backdrop)
		bui_blit(c, 0, 0, w->backdrop, w->w, w->h, w->w);
	else
		bui_fill(c, clip, BUI_C_BG);
}

/* ------------------------------------------------------------------ */
/* rettangoli sporchi                                                 */
/* ------------------------------------------------------------------ */

void bui_win_damage(bui_win *w, bui_rect r)
{
	int i, best = -1, best_grow = 0;

	if (!w || !bui_rect_isect(r, bui_r(0, 0, w->w, w->h), &r))
		return;
	for (;;) {
		/* fonde con un rettangolo esistente se l'unione non spreca area */
		for (i = 0; i < w->ndirty; i++) {
			bui_rect u = bui_rect_union(w->dirty[i], r);
			int au = u.w * u.h, as = w->dirty[i].w * w->dirty[i].h + r.w * r.h;

			if (au <= as + as / 8 + 256)
				break;
		}
		if (i == w->ndirty)
			break;
		r = bui_rect_union(w->dirty[i], r);
		w->dirty[i] = w->dirty[--w->ndirty];
	}
	if (w->ndirty < BUI_MAX_DIRTY) {
		w->dirty[w->ndirty++] = r;
		return;
	}
	/* lista piena: allarga quello che cresce meno */
	for (i = 0; i < w->ndirty; i++) {
		bui_rect u = bui_rect_union(w->dirty[i], r);
		int grow = u.w * u.h - w->dirty[i].w * w->dirty[i].h;

		if (best < 0 || grow < best_grow) {
			best = i;
			best_grow = grow;
		}
	}
	w->dirty[best] = bui_rect_union(w->dirty[best], r);
}

void bui_win_damage_all(bui_win *w)
{
	w->ndirty = 1;
	w->dirty[0] = bui_r(0, 0, w->w, w->h);
}

/* ------------------------------------------------------------------ */
/* timer, animazioni, descrittori                                     */
/* ------------------------------------------------------------------ */

int bui_timer_add(int delay_ms, int period_ms, void (*cb)(void *), void *user)
{
	int i;

	for (i = 0; i < MAX_TIMERS; i++)
		if (!C.timers[i].id) {
			if (++C.timer_seq <= 0)
				C.timer_seq = 1;
			C.timers[i].id = C.timer_seq;
			C.timers[i].due = bui_now() + (uint32_t)(delay_ms < 0 ? 0 : delay_ms);
			C.timers[i].period = period_ms;
			C.timers[i].cb = cb;
			C.timers[i].user = user;
			return C.timers[i].id;
		}
	fprintf(stderr, "bui: troppi timer\n");
	return 0;
}

void bui_timer_del(int id)
{
	int i;

	if (id <= 0)
		return;
	for (i = 0; i < MAX_TIMERS; i++)
		if (C.timers[i].id == id)
			C.timers[i].id = 0;
}

static void run_timers(uint32_t now)
{
	int i;

	for (i = 0; i < MAX_TIMERS; i++) {
		struct timer t = C.timers[i];

		if (!t.id || (int32_t)(now - t.due) < 0)
			continue;
		if (t.period > 0) {
			C.timers[i].due += (uint32_t)t.period;
			if ((int32_t)(now - C.timers[i].due) >= 0)
				C.timers[i].due = now + (uint32_t)t.period;
		} else {
			C.timers[i].id = 0;
		}
		t.cb(t.user);
	}
}

void bui_anim_add(int (*step)(void *, uint32_t), void *user)
{
	int i;

	for (i = 0; i < C.nanim; i++)
		if (C.anims[i].step == step && C.anims[i].user == user)
			return;
	if (C.nanim >= MAX_ANIMS)
		return;
	if (!C.nanim)
		C.next_frame = bui_now() + BUI_FRAME_MS;
	C.anims[C.nanim].step = step;
	C.anims[C.nanim].user = user;
	C.nanim++;
}

void bui_anim_del(int (*step)(void *, uint32_t), void *user)
{
	int i;

	for (i = 0; i < C.nanim; i++)
		if (C.anims[i].step == step && C.anims[i].user == user)
			C.anims[i].step = NULL;	/* tolto al prossimo giro */
}

static void run_anims(uint32_t now)
{
	int i, n = C.nanim, k = 0;

	for (i = 0; i < n; i++)
		if (C.anims[i].step && !C.anims[i].step(C.anims[i].user, now))
			C.anims[i].step = NULL;
	for (i = 0; i < C.nanim; i++)
		if (C.anims[i].step)
			C.anims[k++] = C.anims[i];
	C.nanim = k;
}

int bui_fd_add(int fd, void (*cb)(int, void *), void *user)
{
	if (C.nfds >= MAX_FDS) {
		fprintf(stderr, "bui: troppi descrittori\n");
		return -1;
	}
	C.fds[C.nfds].fd = fd;
	C.fds[C.nfds].cb = cb;
	C.fds[C.nfds].user = user;
	C.nfds++;
	return 0;
}

void bui_fd_del(int fd)
{
	int i;

	for (i = 0; i < C.nfds; i++)
		if (C.fds[i].fd == fd) {
			C.fds[i] = C.fds[--C.nfds];
			return;
		}
}

/* ------------------------------------------------------------------ */
/* ciclo                                                              */
/* ------------------------------------------------------------------ */

void bui_run(void)
{
	struct pollfd pfd[1 + MAX_FDS];
	struct fdent snap[MAX_FDS];

	while (!C.quit) {
		uint32_t now;
		int timeout, d, i, n, cfd, base;

		if (C.be->pump)
			C.be->pump();
		now = bui_now();
		run_timers(now);
		if (C.nanim && (int32_t)(now - C.next_frame) >= 0) {
			run_anims(now);
			/* cadenza fissa: il tempo del disegno non allunga il passo;
			 * se si e' rimasti indietro si riparte da adesso */
			C.next_frame += BUI_FRAME_MS;
			if ((int32_t)(now - C.next_frame) >= 0)
				C.next_frame = now + BUI_FRAME_MS / 2;
		}
		timeout = C.be->flush(now);
		if (C.quit)
			break;
		if (C.be->pending && C.be->pending())
			continue;

		/* quanto si puo' dormire? */
		now = bui_now();
		if (C.nanim) {
			d = (int32_t)(C.next_frame - now);
			if (d < 0)
				d = 0;
			if (timeout < 0 || d < timeout)
				timeout = d;
		}
		for (i = 0; i < MAX_TIMERS; i++) {
			if (!C.timers[i].id)
				continue;
			d = (int32_t)(C.timers[i].due - now);
			if (d < 0)
				d = 0;
			if (timeout < 0 || d < timeout)
				timeout = d;
		}

		base = 0;
		cfd = C.be->conn_fd ? C.be->conn_fd() : -1;
		if (cfd >= 0) {
			pfd[0].fd = cfd;
			pfd[0].events = POLLIN;
			pfd[0].revents = 0;
			base = 1;
		}
		n = C.nfds;
		memcpy(snap, C.fds, sizeof(snap));
		for (i = 0; i < n; i++) {
			pfd[base + i].fd = snap[i].fd;
			pfd[base + i].events = POLLIN;
			pfd[base + i].revents = 0;
		}
		if (poll(pfd, (nfds_t)(base + n), timeout) < 0) {
			if (errno != EINTR)
				break;
			continue;
		}
		if (base && (pfd[0].revents & (POLLHUP | POLLERR)))
			break;		/* il server X se n'e' andato */
		for (i = 0; i < n; i++)
			if (pfd[base + i].revents & (POLLIN | POLLHUP | POLLERR)) {
				int j;

				/* la callback precedente puo' averlo tolto */
				for (j = 0; j < C.nfds; j++)
					if (C.fds[j].fd == snap[i].fd && C.fds[j].cb == snap[i].cb) {
						snap[i].cb(snap[i].fd, snap[i].user);
						break;
					}
			}
	}
}
