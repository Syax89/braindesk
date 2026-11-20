// SPDX-License-Identifier: MIT
/*
 * apps.c - registro delle app mostrate nella home.
 *
 * Per aggiungere una vista integrata: scrivi view_xxx.c con una bs_view,
 * dichiarala qui sotto e metti (o sostituisci) una riga nella tabella.
 * Nient'altro va toccato.
 */
#include "shell.h"

static const char *const argv_terminal[] = { "st", "-f", "DejaVu Sans Mono:size=10", NULL };

const bs_app bs_apps[] = {
	/* id          etichetta       icona                  vista               programma  */
	{ "settings", "Impostazioni", BUI_IC(settings, 40), &bs_view_settings, NULL, 0 },
	{ "clock",    "Orologio",     BUI_IC(clock, 40),    &bs_view_clock,    NULL, 0 },
	{ "calc",     "Calcolatrice", BUI_IC(calc, 40),     &bs_view_calc,     NULL, 0 },
	/* vista integrata; argv e' il ripiego X (pressione lunga sul terminale
	 * -> "Apri in X", oppure BRAINSHELL_TERMINAL=x11) */
	{ "terminal", "Terminale",    BUI_IC(terminal, 40), &bs_view_term, argv_terminal, 0 },
	{ "files",    "Esplora",      BUI_IC(files, 40),    &bs_view_files, NULL, 0 },
	{ "gallery",  "Galleria",     BUI_IC(gallery, 40),  &bs_view_gallery, NULL, 0 },
	{ "music",    "Musica",       BUI_IC(music, 40),    &bs_view_music, NULL, 0 },
	{ "video",    "Video",        BUI_IC(video, 40),    &bs_view_video, NULL, 0 },
	{ "notes",    "Note",         BUI_IC(note, 40),     &bs_view_notes, NULL, 0 },
	{ "dict",     "Dizionario",   BUI_IC(dict, 40),     &bs_view_dict, NULL, 0 },
	{ "games",    "Giochi",       BUI_IC(games, 40),    &bs_view_games, NULL, 0 },
	/* campionario del toolkit: brainshell --open demo */
	{ "demo",     "Campionario",  BUI_IC(apps, 40),     &bs_view_demo, NULL, BS_APP_HIDDEN },
	/* Impostazioni -> Tasti */
	{ "keys",     "Tasti",        BUI_IC(settings, 40), &bs_view_keys, NULL, BS_APP_HIDDEN },
	{ "datetime", "Data e ora",   BUI_IC(clock, 40),    &bs_view_time, NULL, BS_APP_HIDDEN },
};

const int bs_napps = (int)(sizeof(bs_apps) / sizeof(bs_apps[0]));
