// SPDX-License-Identifier: MIT
/*
 * view_demo.c - campionario del toolkit bui: primitive, font, widget.
 *
 * Non compare nella griglia: si apre con "brainshell --open demo". Serve a
 * controllare a colpo d'occhio il disegno (anche sul Brain) e come esempio
 * di vista con pannello scorrevole, lista e widget personalizzati.
 */
#include <stdio.h>

#include "shell.h"

static struct {
	bui_panel p;
	bui_list list;
	bui_px img[48 * 48];
	bui_widget *val;
	char s_val[16];
} D;

static void draw_prims(bui_widget *w, bui_canvas *c, bui_rect r)
{
	const bui_px sw[] = { BUI_C_BG, BUI_C_SURFACE, BUI_C_SURFACE2, BUI_C_SURFACE3, BUI_C_LINE,
				     BUI_C_TEXT3, BUI_C_TEXT2, BUI_C_TEXT, BUI_C_ACCENT, BUI_C_ACCENT_BG,
				     BUI_C_DANGER, BUI_C_WARN };
	int i, x = r.x, y = r.y;

	(void)w;
	/* sfumature con dithering: nessuna banda anche fra colori vicini */
	bui_grad_v(c, bui_r(x, y, r.w, 44), 0x16202E, 0x0A0D14);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(x + 12, y, r.w - 24, 44), "bui_grad_v + dithering",
		     BUI_C_TEXT2, BUI_AL_LEFT);
	y += 52;
	/* tavolozza */
	for (i = 0; i < (int)(sizeof(sw) / sizeof(sw[0])); i++) {
		bui_rrect(c, bui_r(x + i * 36, y, 32, 32), 8, sw[i]);
		bui_rrect_line(c, bui_r(x + i * 36, y, 32, 32), 8, 1, BUI_C_LINE);
	}
	y += 44;
	/* raggi diversi, trasparenza, bordo, ombra */
	for (i = 0; i < 5; i++)
		bui_rrect(c, bui_r(x + i * 60, y, 52, 52), 4 + i * 5, BUI_C_SURFACE2);
	bui_rrect(c, bui_r(x + 300, y, 52, 52), 26, BUI_C_ACCENT);
	bui_rrect_a(c, bui_r(x + 326, y + 8, 52, 36), 12, BUI_C_DANGER, 140);
	bui_rrect_line(c, bui_r(x + 392, y, 52, 52), 16, 2, BUI_C_ACCENT);
	bui_shadow(c, bui_r(x + 462, y + 4, 60, 40), 12, 10);
	bui_rrect(c, bui_r(x + 462, y + 4, 60, 40), 12, BUI_C_SURFACE3);
	/* immagine RGB565 e icone */
	bui_blit(c, x + 540, y + 2, D.img, 48, 48, 48);
	bui_icon_draw(c, BUI_IC(settings, 40), x + 600, y + 6, BUI_C_ACCENT);
	bui_icon_draw(c, BUI_IC(music, 24), x + 648, y + 14, BUI_C_TEXT);
	y += 64;
	/* i font */
	bui_text(c, BUI_FONT_SMALL, x, y + 14, "Piccolo 15 \xE2\x80\x94 perch\xC3\xA9 citt\xC3\xA0 pi\xC3\xB9 \xE2\x82\xAC", BUI_C_TEXT2);
	bui_text(c, BUI_FONT_SMALL_B, x + 260, y + 14, "Grassetto 15", BUI_C_TEXT);
	y += 26;
	bui_text(c, BUI_FONT_BODY, x, y + 16, "Corpo 18 \xE2\x80\x94 Avvia, Tavola, WAVE, 0123456789", BUI_C_TEXT);
	y += 28;
	bui_text(c, BUI_FONT_BODY_B, x, y + 16, "Grassetto 18", BUI_C_TEXT);
	bui_text(c, BUI_FONT_TITLE, x + 160, y + 18, "Titolo 22", BUI_C_TEXT);
	bui_text(c, BUI_FONT_KEY, x + 300, y + 20, "26: 7\xC3\x97" "8=56", BUI_C_ACCENT);
	bui_text(c, BUI_FONT_NUM, x + 460, y + 26, "44,5", BUI_C_TEXT);
}

static void on_slider(bui_widget *w, int final)
{
	(void)final;
	snprintf(D.s_val, sizeof(D.s_val), "%d", w->value);
	bui_widget_damage(D.val);
}

static void on_toggle(bui_widget *w, int final)
{
	(void)w;
	(void)final;
}

static void on_button(bui_widget *w)
{
	bui_toast(w->text ? w->text : "Pulsante", 1500);
}

static void demo_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	char title[32], sub[32];

	(void)l;
	snprintf(title, sizeof(title), "Elemento %d", i + 1);
	snprintf(sub, sizeof(sub), "riga %d di %d", i + 1, l->count);
	bui_list_row(c, r, flags, BUI_IC(folder, 24), title, sub, i % 3 ? NULL : "12 kB");
}

static void demo_select(bui_list *l, int i)
{
	char msg[48];

	(void)l;
	snprintf(msg, sizeof(msg), "Scelto l'elemento %d", i + 1);
	bui_toast(msg, 1500);
}

static void dm_layout(bs_view *v, bui_rect a)
{
	static const struct {
		const char *label;
		int style;
	} btn[] = { { "Tonale", BUI_BS_TONAL }, { "Pieno", BUI_BS_FILLED }, { "Pericolo", BUI_BS_DANGER },
		    { "Testo", BUI_BS_GHOST } };
	bui_panel *p = &D.p;
	int pad = 16, w = a.w - 2 * pad, y = 12, i, bw, lw;
	bui_widget *b;

	(void)v;
	bui_panel_clear(p);
	bui_add_custom(p, bui_r(pad, y, w, 250), draw_prims);
	y += 258;
	bw = (w - 3 * 8) / 4;
	for (i = 0; i < 4; i++) {
		b = bui_add_button(p, bui_r(pad + i * (bw + 8), y, bw, 56), btn[i].label,
				   i == 1 ? BUI_IC(check, 24) : NULL, btn[i].style, on_button);
		if (i == 3)
			b->color = BUI_C_ACCENT;
	}
	y += 68;
	bui_add_card(p, bui_r(pad, y, w, 60 + 60 + 4));
	b = bui_add_toggle(p, bui_r(pad, y, w, 60), "Interruttore", 1, on_toggle);
	b->flags |= BUI_F_DIVIDER;
	y += 60;
	bui_add_slider(p, bui_r(pad + 6, y + 2, w - 80, 56), 0, 100, 40, on_slider);
	snprintf(D.s_val, sizeof(D.s_val), "40");
	D.val = bui_add_label(p, bui_r(pad + w - 72, y + 2, 56, 56), D.s_val, BUI_LS_BODY, BUI_AL_RIGHT);
	y += 60 + 16;
	lw = w;
	bui_add_card(p, bui_r(pad, y, lw, 4 * 64 + 16));
	D.list.count = 40;
	D.list.row_h = 64;
	D.list.draw_row = demo_row;
	D.list.on_select = demo_select;
	D.list.sel = -1;
	bui_add_list(p, bui_r(pad, y + 8, lw, 4 * 64), &D.list);
	y += 4 * 64 + 16 + 16;
	bui_panel_set_viewport(p, a, y);
}

static void dm_enter(bs_view *v)
{
	int x, y;

	bui_panel_init(&D.p, v->win);
	for (y = 0; y < 48; y++)
		for (x = 0; x < 48; x++)
			D.img[y * 48 + x] = bui_pack_dither((x * 255 / 47) << 12, (y * 255 / 47) << 12,
							    (128 + ((x ^ y) & 8) * 8) << 12, x, y);
}

static void dm_leave(bs_view *v)
{
	(void)v;
	bui_panel_clear(&D.p);
}

static void dm_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&D.p, c, clip);
}

static int dm_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	return bui_panel_pointer(&D.p, ev);
}

static int dm_key(bs_view *v, const bui_key *k)
{
	(void)v;
	return bui_panel_key(&D.p, k);
}

bs_view bs_view_demo = {
	.title = "Campionario bui",
	.enter = dm_enter,
	.leave = dm_leave,
	.layout = dm_layout,
	.draw = dm_draw,
	.pointer = dm_pointer,
	.key = dm_key,
};
