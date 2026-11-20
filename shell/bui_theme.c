// SPDX-License-Identifier: MIT
/*
 * bui_theme.c - i temi incorporati e le curve delle animazioni.
 *
 * Un tema e' una struttura di colori, raggi e parametri dello sfondo: per
 * aggiungerne uno basta una voce nella tabella. I colori sono scelti perche'
 * sopravvivano all'arrotondamento a RGB565 (5-6-5 bit) senza cambiare tinta.
 */
#include "bui.h"

#define H(x) BUI_HEX(x)

static const bui_theme themes[] = {
	{
		/* il tema originale: scuro, neutri blu-grigi, accento verde acqua */
		.id = "teal", .name = "Teal",
		.bg = H(0x0E1116), .bar = H(0x0A0C10), .surface = H(0x171C24), .surface2 = H(0x222935),
		.surface3 = H(0x2F3847), .line = H(0x262D39),
		.text = H(0xECEFF4), .text2 = H(0x9BA5B3), .text3 = H(0x646E7C),
		.accent = H(0x5CCFBE), .accent_p = H(0x47A698), .accent_bg = H(0x1B3A3B),
		.on_accent = H(0x04211D),
		.danger = H(0xF2707A), .danger_p = H(0xC4525C), .on_danger = H(0x2B0609), .warn = H(0xF0B45C),
		.r_tile = 20, .r_card = 16, .r_btn = 16, .tile_style = BUI_TILE_FILLED,
		.wall0 = 0x161D2E, .wall1 = 0x0A0D14,
		.blob = { { 88, 10, 75, 0x104446 }, { 10, 100, 67, 0x2A2258 } },
	},
	{
		/* scuro caldo: bruni, accento ambra */
		.id = "brace", .name = "Brace",
		.bg = H(0x15100D), .bar = H(0x0E0A08), .surface = H(0x221A15), .surface2 = H(0x30261F),
		.surface3 = H(0x41342A), .line = H(0x362B22),
		.text = H(0xF5EEE6), .text2 = H(0xB5A797), .text3 = H(0x7A6D60),
		.accent = H(0xF4A857), .accent_p = H(0xC98541), .accent_bg = H(0x45301A),
		.on_accent = H(0x2B1803),
		.danger = H(0xF07A6E), .danger_p = H(0xC25B51), .on_danger = H(0x2D0906), .warn = H(0xF2CE5E),
		.r_tile = 24, .r_card = 20, .r_btn = 18, .tile_style = BUI_TILE_FILLED,
		.wall0 = 0x2C1B14, .wall1 = 0x0F0A08,
		.blob = { { 85, 5, 70, 0x582808 }, { 5, 95, 60, 0x40101C }, { 55, 60, 35, 0x1C1000 } },
		.grain = 3,
	},
	{
		/* nero pieno ad alto contrasto, riquadri a contorno */
		.id = "nero", .name = "Nero",
		.bg = H(0x000000), .bar = H(0x000000), .surface = H(0x0D0D0D), .surface2 = H(0x1C1C1C),
		.surface3 = H(0x303030), .line = H(0x2C2C2C),
		.text = H(0xFFFFFF), .text2 = H(0xBDBDBD), .text3 = H(0x757575),
		.accent = H(0x4DE8FF), .accent_p = H(0x37B4C7), .accent_bg = H(0x06303A),
		.on_accent = H(0x00181D),
		.danger = H(0xFF6B6B), .danger_p = H(0xCC5050), .on_danger = H(0x2B0000), .warn = H(0xFFC83D),
		.r_tile = 12, .r_card = 10, .r_btn = 10, .tile_style = BUI_TILE_OUTLINE,
		.wall0 = 0x000000, .wall1 = 0x000000,
		.blob = { { 100, 100, 55, 0x00222A } },
	},
	{
		/* chiaro: fondo quasi bianco, accento blu, ombre leggere */
		.id = "chiaro", .name = "Chiaro",
		.bg = H(0xF2F4F7), .bar = H(0xE4E7EC), .surface = H(0xFFFFFF), .surface2 = H(0xE4E8EE),
		.surface3 = H(0xCFD5DE), .line = H(0xD8DCE3),
		.text = H(0x141920), .text2 = H(0x566070), .text3 = H(0x9CA4B0),
		.accent = H(0x2468D8), .accent_p = H(0x1B52AE), .accent_bg = H(0xD4E2FA),
		.on_accent = H(0xFFFFFF),
		.danger = H(0xD63B4A), .danger_p = H(0xAE2E3B), .on_danger = H(0xFFFFFF), .warn = H(0xB86E00),
		.r_tile = 20, .r_card = 16, .r_btn = 16, .tile_style = BUI_TILE_FILLED, .light = 1,
		.tile_shadow = 8,
		.tint = { H(0x2468D8), H(0xC2410C), H(0x0F8A6A), H(0x8B3FD1), H(0xC02672), H(0x3B6B8F) },
		.wall0 = 0xDCE6F4, .wall1 = 0xC4D0E4,
		.blob = { { 90, 0, 70, 0x181008 }, { 0, 100, 65, 0x100818 } },
	},
	{
		/* freddo, desaturato (tavolozza "nord") */
		.id = "nord", .name = "Nord",
		.bg = H(0x2E3440), .bar = H(0x262B35), .surface = H(0x3B4252), .surface2 = H(0x434C5E),
		.surface3 = H(0x55607A), .line = H(0x49536A),
		.text = H(0xECEFF4), .text2 = H(0xB9C2D2), .text3 = H(0x7D889D),
		.accent = H(0x88C0D0), .accent_p = H(0x6CA1B1), .accent_bg = H(0x3B5565),
		.on_accent = H(0x1B2A31),
		.danger = H(0xD9737E), .danger_p = H(0xB25A64), .on_danger = H(0x2B0A0E), .warn = H(0xEBCB8B),
		.r_tile = 14, .r_card = 12, .r_btn = 12, .tile_style = BUI_TILE_FILLED,
		.tint = { H(0x88C0D0), H(0xD08770), H(0xA3BE8C), H(0xB48EAD), H(0xEBCB8B), H(0x81A1C1) },
		.wall0 = 0x3A4254, .wall1 = 0x222731,
		.blob = { { 80, 15, 60, 0x14283C }, { 15, 90, 55, 0x0C2826 } },
		.grain = 2,
	},
	{
		/* sfumatura vivace viola-blu, riquadri di vetro, icone colorate */
		.id = "aurora", .name = "Aurora",
		.bg = H(0x130C26), .bar = H(0x0C0719), .surface = H(0x20163A), .surface2 = H(0x2D2050),
		.surface3 = H(0x3E2D69), .line = H(0x35275C),
		.text = H(0xF5F0FF), .text2 = H(0xB7ABD6), .text3 = H(0x776BA3),
		.accent = H(0xFF7FC8), .accent_p = H(0xD15FA2), .accent_bg = H(0x4B2152),
		.on_accent = H(0x35062B),
		.danger = H(0xFF7A85), .danger_p = H(0xCC5C66), .on_danger = H(0x2E060B), .warn = H(0xFFC46B),
		.r_tile = 26, .r_card = 20, .r_btn = 20, .tile_style = BUI_TILE_GLASS,
		.tint = { H(0xFF7FC8), H(0x6FD3FF), H(0xFFB86B), H(0x8CF0B8), H(0xC79BFF), H(0xFFE27A) },
		.wall0 = 0x3A1C8C, .wall1 = 0x0C2458,
		.blob = { { 95, 5, 60, 0x901058 }, { 0, 100, 60, 0x086878 }, { 50, 45, 30, 0x381000 } },
		.grain = 3,
	},
};

#define NTHEMES ((int)(sizeof(themes) / sizeof(themes[0])))

/* il tema di partenza e' gia' valido prima di bui_theme_set() */
bui_theme bui_th = {
	.id = "teal", .name = "Teal",
	.bg = H(0x0E1116), .bar = H(0x0A0C10), .surface = H(0x171C24), .surface2 = H(0x222935),
	.surface3 = H(0x2F3847), .line = H(0x262D39),
	.text = H(0xECEFF4), .text2 = H(0x9BA5B3), .text3 = H(0x646E7C),
	.accent = H(0x5CCFBE), .accent_p = H(0x47A698), .accent_bg = H(0x1B3A3B),
	.on_accent = H(0x04211D),
	.danger = H(0xF2707A), .danger_p = H(0xC4525C), .on_danger = H(0x2B0609), .warn = H(0xF0B45C),
	.r_tile = 20, .r_card = 16, .r_btn = 16, .tile_style = BUI_TILE_FILLED,
	.wall0 = 0x161D2E, .wall1 = 0x0A0D14,
	.blob = { { 88, 10, 75, 0x104446 }, { 10, 100, 67, 0x2A2258 } },
};

static int cur_theme;

int bui_theme_count(void)
{
	return NTHEMES;
}

const bui_theme *bui_theme_get(int i)
{
	return &themes[i < 0 || i >= NTHEMES ? 0 : i];
}

int bui_theme_find(const char *id)
{
	int i, j;

	for (i = 0; id && i < NTHEMES; i++) {
		for (j = 0; themes[i].id[j] && themes[i].id[j] == id[j]; j++)
			;
		if (!themes[i].id[j] && !id[j])
			return i;
	}
	return -1;
}

int bui_theme_current(void)
{
	return cur_theme;
}

void bui_theme_set(int i)
{
	if (i < 0 || i >= NTHEMES)
		i = 0;
	cur_theme = i;
	bui_th = themes[i];
}

/* ------------------------------------------------------------------ */
/* animazioni: livello e curve                                        */
/* ------------------------------------------------------------------ */

static int anim_level = BUI_ANIM_FULL;

void bui_anim_set_level(int level)
{
	anim_level = level < BUI_ANIM_NONE ? BUI_ANIM_NONE : level > BUI_ANIM_FULL ? BUI_ANIM_FULL : level;
}

int bui_anim_level(void)
{
	return anim_level;
}

int bui_tween(uint32_t start, int dur, uint32_t now)
{
	int el = (int32_t)(now - start);

	if (dur <= 0 || el >= dur)
		return 1024;
	return el <= 0 ? 0 : el * 1024 / dur;
}

int bui_ease_out(int t)
{
	int u;

	if (t <= 0)
		return 0;
	if (t >= 1024)
		return 1024;
	u = 1024 - t;
	return 1024 - ((u * u >> 10) * u >> 10);
}

int bui_ease_in_out(int t)
{
	if (t <= 0)
		return 0;
	if (t >= 1024)
		return 1024;
	/* 3t^2 - 2t^3 */
	return (t * t >> 10) * (3 * 1024 - 2 * t) >> 10;
}
