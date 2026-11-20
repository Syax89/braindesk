// SPDX-License-Identifier: MIT
/*
 * User interface for a finger: every target is at least 56 pixels.
 *
 *   +--------------------------------------------------------------+
 *   |                                                              |
 *   |                picture (tap = play / pause)                  |
 *   |                                                              |
 *   +--------------------------------------------------------------+
 *   | play | 0:42 |=====o-------------------| 3:10 | - | 80 | + | X |
 *   +--------------------------------------------------------------+
 *
 * The bar is drawn here, in software, on an RGB565 XShm image (text and
 * icons are baked alpha masks: no font library). It lives in rows that the
 * video overlay never touches. While a video plays the bar hides after a
 * few seconds and the picture takes its rows; any touch or key brings it
 * back. Narrow (rotated) screens get a two-row bar.
 *
 * Files without pictures get the audio view instead: title, artist, a big
 * elapsed time and a wide progress bar.
 */
#include "ui.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ROW_H	56

/* the palette of the BrainDesk shell */
#define C_BG	HEX565(0x0E1116)
#define C_BAR	HEX565(0x0A0C10)
#define C_LINE	HEX565(0x262D39)
#define C_PRESS	HEX565(0x2F3847)
#define C_TRACK	HEX565(0x2F3847)
#define C_TEXT	HEX565(0xECEFF4)
#define C_TEXT2	HEX565(0x9BA5B3)
#define C_TEXT3	HEX565(0x646E7C)
#define C_ACCENT HEX565(0x5CCFBE)

enum { W_NONE, W_PLAY, W_SEEK, W_VOLDN, W_VOLUP, W_CLOSE, W_VIDEO, W_BIGSEEK };

struct sig {
	int state, pos, dur, vol, knob, big, press;
};

struct ui {
	struct win *win;
	struct player *p;
	struct vo *vo;
	bool video;		/* the file has pictures */
	double bar_timeout;

	int bar_h;
	bool bar_visible;
	double bar_hide_at;
	struct ximg *bar;
	struct canvas bar_cv;
	struct rect r_play, r_cur, r_seek, r_total, r_voldn, r_vol, r_volup, r_close;

	/* audio view */
	struct ximg *view;
	struct canvas view_cv;
	struct rect r_big;	/* wide progress bar */
	int y_icon, y_title, y_time, dyn_y0, dyn_y1;

	struct sig bar_sig, view_sig;
	bool bar_dirty, view_dirty, view_static_dirty;

	int press;
	int press_x, press_y;
	bool moved;
	bool dragging;
	double drag_pos;
	double repeat_at;
};

/* ---------------------------------------------------------------- layout --- */

static struct rect video_area(const struct ui *ui)
{
	struct rect r = { 0, 0, ui->win->w, ui->win->h };

	if (ui->bar_visible)
		r.h -= ui->bar_h;
	if (r.h < 0)
		r.h = 0;
	return r;
}

static void layout(struct ui *ui)
{
	struct win *w = ui->win;
	int W = w->w, H = w->h;
	bool two_rows = W < 640;
	int tw = ui->p->duration >= 3600 ? 80 : 62;

	ui->bar_h = two_rows ? 2 * ROW_H : ROW_H;
	if (!two_rows) {
		ui->r_play = (struct rect){ 0, 0, 64, ROW_H };
		ui->r_close = (struct rect){ W - 60, 0, 60, ROW_H };
		ui->r_volup = (struct rect){ ui->r_close.x - 58, 0, 58, ROW_H };
		ui->r_vol = (struct rect){ ui->r_volup.x - 36, 0, 36, ROW_H };
		ui->r_voldn = (struct rect){ ui->r_vol.x - 58, 0, 58, ROW_H };
		ui->r_total = (struct rect){ ui->r_voldn.x - tw - 4, 0, tw, ROW_H };
		ui->r_cur = (struct rect){ 64, 0, tw, ROW_H };
		ui->r_seek = (struct rect){ 64 + tw, 0, ui->r_total.x - 64 - tw, ROW_H };
	} else {
		int cell = W / 5;

		ui->r_cur = (struct rect){ 0, 0, tw, ROW_H };
		ui->r_total = (struct rect){ W - tw, 0, tw, ROW_H };
		ui->r_seek = (struct rect){ tw, 0, W - 2 * tw, ROW_H };
		ui->r_play = (struct rect){ 0, ROW_H, cell, ROW_H };
		ui->r_voldn = (struct rect){ cell, ROW_H, cell, ROW_H };
		ui->r_vol = (struct rect){ 2 * cell, ROW_H, cell, ROW_H };
		ui->r_volup = (struct rect){ 3 * cell, ROW_H, cell, ROW_H };
		ui->r_close = (struct rect){ 4 * cell, ROW_H, W - 4 * cell, ROW_H };
	}

	ximg_free(w, ui->bar);
	ui->bar = ximg_new(w, W, ui->bar_h);
	if (ui->bar)
		ui->bar_cv = (struct canvas){ ui->bar->px, W, ui->bar_h, ui->bar->stride };
	ui->bar_dirty = true;

	ximg_free(w, ui->view);
	ui->view = NULL;
	if (!ui->video && H > ui->bar_h + 8) {
		int vh = H - ui->bar_h;

		ui->view = ximg_new(w, W, vh);
		if (ui->view)
			ui->view_cv = (struct canvas){ ui->view->px, W, vh, ui->view->stride };
		ui->y_icon = vh * 15 / 100;
		ui->y_title = vh * 36 / 100;
		ui->y_time = vh * 69 / 100;
		ui->r_big = (struct rect){ 40, vh * 83 / 100 - ROW_H / 2, W - 80, ROW_H };
		ui->dyn_y0 = ui->y_time - font_big.ascent - 4;
		ui->dyn_y1 = ui->r_big.y + ui->r_big.h;
		if (ui->dyn_y0 < 0)
			ui->dyn_y0 = 0;
		if (ui->dyn_y1 > vh)
			ui->dyn_y1 = vh;
		ui->view_static_dirty = true;
	}
	if (ui->vo)
		vo_set_area(ui->vo, video_area(ui));
}

/* --------------------------------------------------------------- drawing --- */

static void text_center(struct canvas *c, const struct font *f, struct rect r,
			const char *s, uint16_t col)
{
	int tw = cv_text_width(f, s);
	int base = r.y + (r.h + f->ascent - f->descent) / 2;

	cv_text(c, f, r.x + (r.w - tw) / 2, base, s, col, 0);
}

/* progress track with the played part, and a knob */
static int draw_track(struct canvas *c, struct rect r, int margin, int thick,
		      double frac, bool active, bool enabled)
{
	int x0 = r.x + margin, x1 = r.x + r.w - margin;
	int cy = r.y + r.h / 2;
	int kx;

	if (x1 <= x0)
		return x0;
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	kx = x0 + (int)(frac * (x1 - x0) + 0.5);
	cv_fill(c, x0, cy - thick / 2, x1 - x0, thick, C_TRACK);
	if (enabled) {
		cv_fill(c, x0, cy - thick / 2, kx - x0, thick, C_ACCENT);
		cv_icon(c, &icon_knob, kx, cy, active ? C_TEXT : C_ACCENT);
	}
	return kx;
}

static double track_pos(const struct ui *ui, struct rect r, int margin, int x)
{
	int x0 = r.x + margin, x1 = r.x + r.w - margin;
	double frac = x1 > x0 ? (double)(x - x0) / (x1 - x0) : 0;

	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	return frac * ui->p->duration;
}

static void button(struct ui *ui, struct rect r, int id, const struct icon *ic, uint16_t col)
{
	if (ui->press == id)
		cv_fill(&ui->bar_cv, r.x + 3, r.y + 5, r.w - 6, r.h - 9, C_PRESS);
	cv_icon(&ui->bar_cv, ic, r.x + r.w / 2, r.y + r.h / 2 + 1, col);
}

static void render_bar(struct ui *ui, int state, double pos)
{
	struct canvas *c = &ui->bar_cv;
	struct player *p = ui->p;
	bool hours = p->duration >= 3600;
	char buf[16];

	cv_fill(c, 0, 0, c->w, c->h, C_BAR);
	cv_fill(c, 0, 0, c->w, 1, C_LINE);

	button(ui, ui->r_play, W_PLAY, state == ST_PLAYING ? &icon_pause : &icon_play, C_TEXT);
	text_center(c, &font_small, ui->r_cur, fmt_time(buf, pos, hours), C_TEXT);
	draw_track(c, ui->r_seek, 16, 6, p->duration > 0 ? pos / p->duration : 0,
		   ui->press == W_SEEK, p->duration > 0);
	text_center(c, &font_small, ui->r_total,
		    p->duration > 0 ? fmt_time(buf, p->duration, hours) : "--:--", C_TEXT2);
	button(ui, ui->r_voldn, W_VOLDN, &icon_voldown, C_TEXT);
	snprintf(buf, sizeof(buf), "%d", atomic_load(&p->volume));
	text_center(c, &font_small, ui->r_vol, buf, C_TEXT2);
	button(ui, ui->r_volup, W_VOLUP, &icon_volup, C_TEXT);
	button(ui, ui->r_close, W_CLOSE, &icon_close, C_TEXT2);
}

static void render_view_static(struct ui *ui)
{
	struct canvas *c = &ui->view_cv;
	struct player *p = ui->p;
	char sub[280];
	int tw, max_w = c->w - 64;

	cv_fill(c, 0, 0, c->w, c->h, C_BG);
	cv_icon(c, &icon_note, c->w / 2, ui->y_icon, C_ACCENT);

	tw = cv_text_width(&font_title, p->title);
	if (tw > max_w)
		tw = max_w;
	cv_text(c, &font_title, (c->w - tw) / 2, ui->y_title, p->title, C_TEXT, max_w);

	if (p->artist[0] && p->album[0])
		snprintf(sub, sizeof(sub), "%s \xE2\x80\x94 %s", p->artist, p->album);
	else
		snprintf(sub, sizeof(sub), "%s", p->artist[0] ? p->artist : p->album);
	tw = cv_text_width(&font_text, sub);
	if (tw > max_w)
		tw = max_w;
	cv_text(c, &font_text, (c->w - tw) / 2, ui->y_title + 36, sub, C_TEXT2, max_w);
}

static void render_view_dynamic(struct ui *ui, int state, double pos)
{
	struct canvas *c = &ui->view_cv;
	struct player *p = ui->p;
	bool hours = p->duration >= 3600;
	char cur[16], tot[24] = "";
	int wc, wt, x;

	cv_fill(c, 0, ui->dyn_y0, c->w, ui->dyn_y1 - ui->dyn_y0, C_BG);
	fmt_time(cur, pos, hours);
	if (p->duration > 0) {
		char t[16];

		snprintf(tot, sizeof(tot), " / %s", fmt_time(t, p->duration, hours));
	}
	wc = cv_text_width(&font_big, cur);
	wt = cv_text_width(&font_title, tot);
	if (wc + wt > c->w - 16)
		wt = 0;		/* narrow screen: elapsed time only */
	x = (c->w - wc - wt) / 2;
	cv_text(c, &font_big, x, ui->y_time, cur, state == ST_PLAYING ? C_TEXT : C_TEXT2, 0);
	if (wt)
		cv_text(c, &font_title, x + wc, ui->y_time, tot, C_TEXT3, 0);
	draw_track(c, ui->r_big, 16, 10, p->duration > 0 ? pos / p->duration : 0,
		   ui->press == W_BIGSEEK, p->duration > 0);
}

/* ------------------------------------------------------------- bar on/off --- */

static void show_bar(struct ui *ui, double now)
{
	ui->bar_hide_at = now + ui->bar_timeout;
	if (ui->bar_visible)
		return;
	ui->bar_visible = true;
	ui->bar_dirty = true;
	/* first the picture leaves the rows of the bar, then the bar is drawn */
	if (ui->vo)
		vo_set_area(ui->vo, video_area(ui));
}

static void hide_bar(struct ui *ui)
{
	if (!ui->bar_visible)
		return;
	ui->bar_visible = false;
	if (ui->vo)
		vo_set_area(ui->vo, video_area(ui));
	else
		win_clear(ui->win, 0, ui->win->h - ui->bar_h, ui->win->w, ui->bar_h);
}

/* ---------------------------------------------------------------- public --- */

struct ui *ui_new(struct win *win, struct player *p, const struct options *opt)
{
	struct ui *ui = calloc(1, sizeof(*ui));

	if (!ui)
		return NULL;
	ui->win = win;
	ui->p = p;
	ui->video = p->vst != NULL;
	ui->bar_timeout = opt->bar_timeout;
	ui->bar_visible = true;
	ui->bar_hide_at = now_sec() + ui->bar_timeout;
	layout(ui);
	return ui;
}

void ui_free(struct ui *ui)
{
	if (!ui)
		return;
	ximg_free(ui->win, ui->bar);
	ximg_free(ui->win, ui->view);
	free(ui);
}

void ui_set_vo(struct ui *ui, struct vo *vo)
{
	ui->vo = vo;
	if (vo)
		vo_set_area(vo, video_area(ui));
}

static void change_volume(struct ui *ui, int delta)
{
	player_set_volume(ui->p, atomic_load(&ui->p->volume) + delta);
}

static int hit_bar(const struct ui *ui, int x, int y)
{
	if (rect_has(ui->r_play, x, y))
		return W_PLAY;
	if (rect_has(ui->r_seek, x, y))
		return ui->p->duration > 0 ? W_SEEK : W_NONE;
	if (rect_has(ui->r_voldn, x, y))
		return W_VOLDN;
	if (rect_has(ui->r_volup, x, y))
		return W_VOLUP;
	if (rect_has(ui->r_close, x, y))
		return W_CLOSE;
	return W_NONE;
}

static bool on_press(struct ui *ui, int x, int y, double now)
{
	int bar_y = ui->win->h - ui->bar_h;
	bool was_hidden = !ui->bar_visible;

	show_bar(ui, now);
	ui->press_x = x;
	ui->press_y = y;
	ui->moved = false;
	if (was_hidden)
		ui->press = W_VIDEO;	/* nobody aims at a button that is not there */
	else if (y >= bar_y)
		ui->press = hit_bar(ui, x, y - bar_y);
	else if (!ui->video && ui->p->duration > 0 && rect_has(ui->r_big, x, y))
		ui->press = W_BIGSEEK;
	else
		ui->press = W_VIDEO;

	switch (ui->press) {
	case W_SEEK:
		ui->dragging = true;
		ui->drag_pos = track_pos(ui, ui->r_seek, 16, x);
		break;
	case W_BIGSEEK:
		ui->dragging = true;
		ui->drag_pos = track_pos(ui, ui->r_big, 16, x);
		break;
	case W_VOLDN:
	case W_VOLUP:
		change_volume(ui, ui->press == W_VOLUP ? 5 : -5);
		ui->repeat_at = now + 0.4;
		break;
	}
	return true;
}

static bool on_release(struct ui *ui, int x, int y, double now)
{
	int bar_y = ui->win->h - ui->bar_h;
	int press = ui->press;
	bool quit = false;

	ui->press = W_NONE;
	ui->bar_hide_at = now + ui->bar_timeout;
	switch (press) {
	case W_PLAY:
		if (rect_has(ui->r_play, x, y - bar_y))
			player_toggle(ui->p);
		break;
	case W_CLOSE:
		quit = rect_has(ui->r_close, x, y - bar_y);
		break;
	case W_SEEK:
	case W_BIGSEEK:
		ui->dragging = false;
		player_seek(ui->p, ui->drag_pos, 0);
		break;
	case W_VIDEO:
		if (!ui->moved)
			player_toggle(ui->p);
		break;
	}
	return !quit;
}

static bool on_key(struct ui *ui, KeySym k, double now)
{
	show_bar(ui, now);
	switch (k) {
	case XK_space:
	case XK_Return:
	case XK_KP_Enter:
	case XK_p:
		player_toggle(ui->p);
		break;
	case XK_Left:
		player_seek_rel(ui->p, -10);
		break;
	case XK_Right:
		player_seek_rel(ui->p, 10);
		break;
	case XK_Page_Down:
		player_seek_rel(ui->p, -60);
		break;
	case XK_Page_Up:
		player_seek_rel(ui->p, 60);
		break;
	case XK_Home:
		player_seek(ui->p, 0, 0);
		break;
	case XK_Up:
		change_volume(ui, 5);
		break;
	case XK_Down:
		change_volume(ui, -5);
		break;
	case XK_q:
	case XK_Escape:
		return false;
	}
	return true;
}

bool ui_event(struct ui *ui, XEvent *ev)
{
	struct win *w = ui->win;
	double now = now_sec();

	switch (ev->type) {
	case Expose: {
		int bar_y = w->h - ui->bar_h;
		int y0 = ev->xexpose.y, y1 = y0 + ev->xexpose.height;

		/* what X repainted black is put back from our images */
		if (ui->bar_visible && ui->bar && y1 > bar_y)
			ximg_put(w, ui->bar, 0, 0, 0, bar_y, w->w, ui->bar_h, false);
		if (ui->view && y0 < bar_y)
			ximg_put(w, ui->view, 0, 0, 0, 0, w->w, ui->view->h, false);
		if (ui->vo && ev->xexpose.count == 0 && y0 < video_area(ui).h)
			vo_event(ui->vo, VO_EXPOSE);
		break;
	}
	case ConfigureNotify: {
		int ox = w->abs_x, oy = w->abs_y;
		bool resized = ev->xconfigure.width != w->w || ev->xconfigure.height != w->h;

		win_configure(w, ev->xconfigure.width, ev->xconfigure.height);
		if (resized)
			layout(ui);
		else if (ui->vo && (ox != w->abs_x || oy != w->abs_y))
			vo_event(ui->vo, VO_AREA);
		break;
	}
	case MapNotify:
		w->mapped = true;
		win_configure(w, w->w, w->h);
		break;
	case UnmapNotify:
		w->mapped = false;
		break;
	case VisibilityNotify:
		if (w->visibility != ev->xvisibility.state) {
			w->visibility = ev->xvisibility.state;
			dbg("window visibility %d", w->visibility);
			if (ui->vo && w->visibility != VisibilityFullyObscured)
				vo_event(ui->vo, VO_AREA);
		}
		break;
	case ClientMessage:
		if (ev->xclient.message_type == w->wm_protocols &&
		    (Atom)ev->xclient.data.l[0] == w->wm_delete)
			return false;
		break;
	case KeyPress:
		return on_key(ui, XLookupKeysym(&ev->xkey, 0), now);
	case ButtonPress:
		if (ev->xbutton.button == Button1)
			return on_press(ui, ev->xbutton.x, ev->xbutton.y, now);
		if (ev->xbutton.button == Button4 || ev->xbutton.button == Button5) {
			show_bar(ui, now);
			change_volume(ui, ev->xbutton.button == Button4 ? 5 : -5);
		}
		break;
	case ButtonRelease:
		if (ev->xbutton.button == Button1 && ui->press != W_NONE)
			return on_release(ui, ev->xbutton.x, ev->xbutton.y, now);
		break;
	case MotionNotify:
		if (abs(ev->xmotion.x - ui->press_x) + abs(ev->xmotion.y - ui->press_y) > 24)
			ui->moved = true;
		if (ui->dragging)
			ui->drag_pos = track_pos(ui, ui->press == W_SEEK ? ui->r_seek : ui->r_big,
						 16, ev->xmotion.x);
		break;
	default:
		if (ev->type == w->shm_event && ui->vo)
			vo_event(ui->vo, VO_SHM_DONE);
		break;
	}
	return true;
}

double ui_tick(struct ui *ui, double now)
{
	struct win *w = ui->win;
	struct player *p = ui->p;
	int state = player_state(p);
	double pos = ui->dragging ? ui->drag_pos : player_pos(p);
	double next = now + 3600;
	struct sig s;

	if ((ui->press == W_VOLDN || ui->press == W_VOLUP) && now >= ui->repeat_at) {
		change_volume(ui, ui->press == W_VOLUP ? 5 : -5);
		ui->repeat_at = now + 0.12;
	}

	/* the bar hides only while a video is playing and nobody touches it */
	if (!ui->video || ui->bar_timeout <= 0 || state != ST_PLAYING || ui->press != W_NONE)
		show_bar(ui, now);
	else if (ui->bar_visible && now >= ui->bar_hide_at)
		hide_bar(ui);

	s.state = state;
	s.pos = (int)pos;
	s.dur = (int)p->duration;
	s.vol = atomic_load(&p->volume);
	s.press = ui->press;
	s.knob = p->duration > 0 ? (int)(pos / p->duration * (ui->r_seek.w - 32)) : 0;
	s.big = p->duration > 0 ? (int)(pos / p->duration * (ui->r_big.w - 32)) : 0;

	if (w->mapped && w->visibility != VisibilityFullyObscured) {
		if (ui->bar_visible && ui->bar &&
		    (ui->bar_dirty || memcmp(&s, &ui->bar_sig, sizeof(s)))) {
			render_bar(ui, state, pos);
			ximg_put(w, ui->bar, 0, 0, 0, w->h - ui->bar_h, w->w, ui->bar_h, false);
			ui->bar_sig = s;
			ui->bar_dirty = false;
		}
		if (ui->view) {
			bool all = ui->view_static_dirty;

			if (all)
				render_view_static(ui);
			if (all || ui->view_dirty || memcmp(&s, &ui->view_sig, sizeof(s))) {
				render_view_dynamic(ui, state, pos);
				if (all)
					ximg_put(w, ui->view, 0, 0, 0, 0, w->w, ui->view->h, false);
				else
					ximg_put(w, ui->view, 0, ui->dyn_y0, 0, ui->dyn_y0, w->w,
						 ui->dyn_y1 - ui->dyn_y0, false);
				ui->view_sig = s;
				ui->view_dirty = ui->view_static_dirty = false;
			}
		}
	}

	if (state == ST_PLAYING && !ui->dragging && (ui->bar_visible || ui->view)) {
		/* next change of the displayed second */
		double frac = pos - floor(pos);

		next = now + (1 - frac) + 0.02;
	}
	if (ui->video && ui->bar_visible && ui->bar_timeout > 0 && state == ST_PLAYING &&
	    ui->press == W_NONE && ui->bar_hide_at < next)
		next = ui->bar_hide_at;
	if ((ui->press == W_VOLDN || ui->press == W_VOLUP) && ui->repeat_at < next)
		next = ui->repeat_at;
	return next;
}
