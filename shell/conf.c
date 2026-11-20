// SPDX-License-Identifier: MIT
/*
 * conf.c - preferenze della shell in ~/.config/brainshell/config.
 *
 * Un file di testo "chiave=valore", una voce per riga; le righe sconosciute
 * vengono ignorate (e non riscritte). La scrittura e' atomica: file
 * temporaneo nella stessa cartella, fsync, rename. Le modifiche ravvicinate
 * (un cursore, piu' tocchi) producono una sola scrittura.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

#define SAVE_DELAY_MS 700

bs_prefs bs_pref = {
	.theme = 0,
	.anim = BUI_ANIM_FULL,
	.tile_style = -1,
	.dim_s = 120,
	.off_s = 0,
	.sleep_s = 300,
	.keys = { "switcher", "quick", "music", "note", "screenshot" },
};
int bs_pref_battery_percent = 1;

static int save_timer;
static const char *const anim_names[] = { "none", "reduced", "full" };
static const char *const tile_names[] = { "filled", "glass", "outline" };
/* tasti del dizionario, nell'ordine di BS_KEY_* */
static const char *const key_conf[5] = { "key_kokugo", "key_eiwa", "key_english", "key_mydict", "key_history" };

static int conf_path(char *buf, size_t len, const char *suffix)
{
	const char *home = getenv("HOME");

	if (!home || !*home)
		return -1;
	return snprintf(buf, len, "%s/.config/brainshell/config%s", home, suffix) < (int)len ? 0 : -1;
}

static int lookup(const char *v, const char *const *names, int n, int def)
{
	int i;

	for (i = 0; i < n; i++)
		if (!strcmp(v, names[i]))
			return i;
	return def;
}

static int seconds(const char *v)
{
	int n = atoi(v);

	return n < 0 ? 0 : n > 3600 ? 3600 : n;
}

void bs_conf_load(void)
{
	char path[320], line[320];
	FILE *f;

	if (conf_path(path, sizeof(path), "") < 0 || !(f = fopen(path, "re")))
		return;
	while (fgets(line, sizeof(line), f)) {
		char *v = strchr(line, '=');

		if (line[0] == '#' || !v)
			continue;
		*v++ = 0;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(line, "theme")) {
			int t = bui_theme_find(v);

			if (t >= 0)
				bs_pref.theme = t;
		} else if (!strcmp(line, "animations")) {
			bs_pref.anim = lookup(v, anim_names, 3, BUI_ANIM_FULL);
		} else if (!strcmp(line, "tiles")) {
			bs_pref.tile_style = lookup(v, tile_names, 3, -1);
		} else if (!strcmp(line, "battery_percent")) {
			bs_pref_battery_percent = atoi(v) != 0;
		} else if (!strcmp(line, "dim")) {
			bs_pref.dim_s = seconds(v);
		} else if (!strcmp(line, "off")) {
			bs_pref.off_s = seconds(v);
		} else if (!strcmp(line, "sleep_after")) {
			bs_pref.sleep_s = seconds(v);
		} else if (!strcmp(line, "language")) {
			bs_pref.lang = bs_lang_find(v);
		} else if (!strcmp(line, "power_saver")) {
			bs_pref.saver = atoi(v) != 0;
		} else if (!strcmp(line, "order")) {
			snprintf(bs_pref.order, sizeof(bs_pref.order), "%s", v);
		} else if (!strcmp(line, "hidden")) {
			snprintf(bs_pref.hidden, sizeof(bs_pref.hidden), "%s", v);
		} else {
			int i;

			for (i = 0; i < 5; i++)
				if (!strcmp(line, key_conf[i]))
					snprintf(bs_pref.keys[i], sizeof(bs_pref.keys[i]), "%s", v);
		}
	}
	fclose(f);
}

void bs_conf_flush(void)
{
	char path[320], tmp[320], dir[320], buf[1280];
	const char *home = getenv("HOME");
	int fd, n;

	if (save_timer)
		bui_timer_del(save_timer);
	else
		return;		/* niente da scrivere */
	save_timer = 0;
	if (!home || conf_path(path, sizeof(path), "") < 0 || conf_path(tmp, sizeof(tmp), ".tmp") < 0)
		return;
	snprintf(dir, sizeof(dir), "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.config/brainshell", home);
	mkdir(dir, 0755);
	n = snprintf(buf, sizeof(buf),
		     "# brainshell: preferenze (riscritto dalla shell)\n"
		     "theme=%s\nanimations=%s\ntiles=%s\nbattery_percent=%d\ndim=%d\noff=%d\n"
		     "sleep_after=%d\npower_saver=%d\nlanguage=%s\norder=%s\nhidden=%s\n",
		     bui_theme_get(bs_pref.theme)->id, anim_names[bs_pref.anim],
		     bs_pref.tile_style < 0 ? "theme" : tile_names[bs_pref.tile_style],
		     bs_pref_battery_percent, bs_pref.dim_s, bs_pref.off_s, bs_pref.sleep_s, bs_pref.saver,
		     bs_lang_code(bs_pref.lang),
		     bs_pref.order, bs_pref.hidden);
	if (n < 0 || n >= (int)sizeof(buf))
		return;
	{
		int i, m;

		for (i = 0; i < 5; i++) {
			m = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s=%s\n", key_conf[i], bs_pref.keys[i]);
			if (m < 0 || n + m >= (int)sizeof(buf))
				return;
			n += m;
		}
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	if (write(fd, buf, (size_t)n) != n || fsync(fd) < 0) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) < 0)
		unlink(tmp);
}

static void save_fire(void *user)
{
	(void)user;
	bs_conf_flush();
}

void bs_conf_save(void)
{
	if (save_timer)
		bui_timer_del(save_timer);
	save_timer = bui_timer_add(SAVE_DELAY_MS, 0, save_fire, NULL);
}

void bs_prefs_apply(void)
{
	if (bs_pref.anim < BUI_ANIM_NONE || bs_pref.anim > BUI_ANIM_FULL)
		bs_pref.anim = BUI_ANIM_FULL;
	if (bs_pref.theme < 0 || bs_pref.theme >= bui_theme_count())
		bs_pref.theme = 0;
	bui_theme_set(bs_pref.theme);
	if (bs_pref.tile_style >= 0)
		bui_th.tile_style = (uint8_t)bs_pref.tile_style;
	/* il risparmio energetico toglie le animazioni grandi */
	bui_anim_set_level(bs_pref.saver && bs_pref.anim > BUI_ANIM_REDUCED ? BUI_ANIM_REDUCED : bs_pref.anim);
}

int bs_eff_dim(void)
{
	return bs_pref.saver ? 15 : bs_pref.dim_s;
}

int bs_eff_off(void)
{
	return bs_pref.saver ? 30 : bs_pref.off_s;
}

int bs_eff_sleep(void)
{
	return bs_pref.saver ? 60 : bs_pref.sleep_s;
}
