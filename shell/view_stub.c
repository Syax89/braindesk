// SPDX-License-Identifier: MIT
/*
 * view_stub.c - segnaposto per le app non ancora pronte ("in arrivo").
 * E' anche l'esempio piu' piccolo possibile di vista.
 */
#include "shell.h"

static void stub_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	bui_rect a = v->area;
	int cy = a.y + a.h / 2 - 76;
	bui_rect badge = bui_r(a.x + (a.w - 88) / 2, cy, 88, 88);

	(void)clip;
	bui_rrect(c, badge, 44, BUI_C_SURFACE);
	bui_icon_in(c, v->app->icon, badge, BUI_C_TEXT2);
	bui_text_box(c, BUI_FONT_TITLE, bui_r(a.x + 16, cy + 104, a.w - 32, 32), "In arrivo", BUI_C_TEXT,
		     BUI_AL_CENTER);
	bui_text_wrap(c, BUI_FONT_BODY, bui_r(a.x + (a.w - 360) / 2, cy + 140, 360, 60),
		      "Questa app sar\xC3\xA0 disponibile in un prossimo aggiornamento.", BUI_C_TEXT2, 26,
		      BUI_AL_CENTER);
}

bs_view bs_view_stub = {
	.draw = stub_draw,
};
