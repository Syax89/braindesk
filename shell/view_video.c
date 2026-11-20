// SPDX-License-Identifier: MIT
/*
 * view_video.c - Video: elenco dei filmati di ~/Video.
 *
 * Un tocco avvia "brainplay FILE" ($BRAIN_PLAYER) come programma esterno a
 * tutto schermo, con lo stesso meccanismo delle altre app esterne
 * (bs_launch); se c'e' musica in riproduzione viene prima messa in pausa.
 */
#include <stdio.h>
#include <string.h>

#include "media.h"

#define ROW_H 72

static struct {
	bui_panel p;
	bui_list list;
	int active;
} V;

static void lib_changed(ms_lib *l);

static ms_lib lib = {
	.subdir = "Video",
	.exts = "mp4 mkv avi webm mov mpg mpeg ts flv",
	.recursive = 1,
	.cmp = ms_cmp_path,
	.changed = lib_changed,
};

static void draw_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	const ms_item *it = &lib.it[i];
	char name[MS_TEXT], folder[96], date[32], sub[160], size[24];

	(void)l;
	ms_clean(name, sizeof(name), it->name, -1);
	ms_folder(it, folder, sizeof(folder));
	ms_fmt_date(date, sizeof(date), it->mtime);
	ms_fmt_size(size, sizeof(size), it->size);
	if (folder[0])
		snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", date, folder);
	else
		snprintf(sub, sizeof(sub), "%s", date);
	bui_list_row(c, r, flags, BUI_IC(video, 24), name, sub, size);
	if (i + 1 < lib.n)
		bui_fill(c, bui_r(r.x + 56, r.y + r.h - 1, r.w - 72, 1), BUI_C_LINE);
}

static void on_select(bui_list *l, int i)
{
	const char *argv[3];
	char name[80], msg[128];

	(void)l;
	if (i < 0 || i >= lib.n)
		return;
	mp_pause();	/* il video porta il suo audio */
	argv[0] = mp_program();
	argv[1] = lib.it[i].path;
	argv[2] = NULL;
	ms_title(&lib.it[i], name, sizeof(name));
	if (bs_launch(argv, "il lettore video") == 0) {
		snprintf(msg, sizeof(msg), T("Apertura di %s\xE2\x80\xA6"), name);
		bui_toast(msg, 1800);
	}
}

static const char *empty_text(void)
{
	return lib.loaded ? "Nessun video nella cartella Video" : "Ricerca dei video\xE2\x80\xA6";
}

static void lib_changed(ms_lib *l)
{
	if (!V.active)
		return;
	V.list.empty_text = empty_text();
	bui_list_set_count(&V.list, l->n);
}

static void vd_enter(bs_view *v)
{
	bui_panel_init(&V.p, v->win);
	V.active = 1;
	ms_scan(&lib);
}

static void vd_leave(bs_view *v)
{
	(void)v;
	V.active = 0;
	bui_panel_clear(&V.p);
	ms_cancel(&lib);
}

static void vd_layout(bs_view *v, bui_rect a)
{
	bui_widget *w;

	(void)v;
	bui_panel_clear(&V.p);
	bui_panel_set_viewport(&V.p, a, a.h);
	V.list.count = lib.n;
	V.list.row_h = ROW_H;
	V.list.draw_row = draw_row;
	V.list.on_select = on_select;
	V.list.empty_text = empty_text();
	V.list.sel = -1;
	w = bui_add_list(&V.p, bui_r(0, 0, a.w, a.h), &V.list);
	bui_panel_focus(&V.p, w, 0);
}

static void vd_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&V.p, c, clip);
}

static int vd_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&V.p, ev);
}

static int vd_key(bs_view *v, const bui_key *k)
{
	(void)v;
	return bui_panel_key(&V.p, k);
}

bs_view bs_view_video = {
	.enter = vd_enter,
	.leave = vd_leave,
	.layout = vd_layout,
	.draw = vd_draw,
	.pointer = vd_pointer,
	.key = vd_key,
};
