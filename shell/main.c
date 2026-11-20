// SPDX-License-Identifier: MIT
/*
 * brainshell - interfaccia touch di BrainDesk per SHARP Brain PW-SH5.
 *
 * Un solo processo: barra di stato (dock), home (desktop) con le app
 * integrate come viste, impostazioni rapide. Vedi README.md.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "shell.h"

static int ready;
static struct timespec t_start;
static bui_win *w_home, *w_bar;

void bs_notify(int ev)
{
	if (!ready)
		return;
	bs_bar_notify(ev);
	bs_home_notify(ev);
}

/*
 * Codice e dati costanti della shell (font e icone compresi) e delle sue
 * librerie restano in RAM: senza, la prima apertura di una vista legge il
 * codice dalla microSD, lenta e contesa dal resto dell'avvio nel primo minuto,
 * e lo stesso succede quando la memoria scarseggia e il kernel scarta pagine
 * di file. Sono ~2 MB, solo mappature di file in sola lettura o eseguibili.
 * Gira in un thread: il disco si legge senza fermare l'interfaccia.
 */
static void *lock_code(void *arg)
{
	char line[512];
	FILE *f = fopen("/proc/self/maps", "re");
	size_t total = 0;

	(void)arg;
	if (!f)
		return NULL;
	while (fgets(line, sizeof(line), f)) {
		unsigned long a, b, off;
		char perm[8], path[256] = "";

		if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %255s", &a, &b, perm, &off, path) < 4)
			continue;
		/* file veri (non [stack], [heap], dispositivi o memoria anonima),
		 * senza scrittura: testo e costanti */
		if (path[0] != '/' || perm[1] == 'w' || !strncmp(path, "/dev/", 5))
			continue;
		if (mlock((void *)a, b - a) == 0)
			total += b - a;
	}
	fclose(f);
	if (getenv("BRAINSHELL_TIMING"))
		fprintf(stderr, "brainshell: %zu KB di codice fissati in RAM\n", total / 1024);
	return NULL;
}

static void lock_code_async(void)
{
	pthread_t t;
	pthread_attr_t at;

	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 64 * 1024);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, lock_code, NULL) != 0)
		lock_code(NULL);
	pthread_attr_destroy(&at);
}

static void after_first_frame(void *user)
{
	static int tries;
	struct timespec now;

	(void)user;
	/* aspetta che home e barra siano state davvero disegnate e inviate
	 * (ma non per sempre: se il gestore non le mappa si va avanti lo stesso) */
	if ((!w_home->last_flush || !w_bar->last_flush) && ++tries < 500) {
		bui_timer_add(4, 0, after_first_frame, NULL);
		return;
	}
	if (getenv("BRAINSHELL_TIMING")) {
		bui_sync();	/* il server ha finito di copiare i pixel */
		clock_gettime(CLOCK_BOOTTIME, &now);
		fprintf(stderr, "brainshell: primo fotogramma dopo %ld ms\n",
			(long)((now.tv_sec - t_start.tv_sec) * 1000 + (now.tv_nsec - t_start.tv_nsec) / 1000000));
	}
	if (getenv("BRAINSHELL_READY_FILE")) {
		/* per brain-preinit: l'interfaccia e' sullo schermo */
		int fd = open(getenv("BRAINSHELL_READY_FILE"), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

		if (fd >= 0)
			close(fd);
	}
	if (getenv("BRAINSHELL_EXIT_AFTER_FIRST_FRAME")) {	/* solo per le misure */
		bui_quit();
		return;
	}
	/* cio' che richiede altri processi parte solo a schermo gia' disegnato */
	bs_refresh_levels();
	bs_wm_focus_home();
	bs_idle_init();
	lock_code_async();
}

static const char *const screen_names[] = { "on", "dim", "off" };
static const char *const xapp_names[] = { "idle", "starting", "running", "returning" };

/* stato per il comando "expect" degli script del backend mem */
static void state_query(char *buf, int len)
{
	int s = bs_screen_state(), x = bs_xapp_state();

	snprintf(buf, (size_t)len, "view=%s dialog=%d screen=%s xapp=%s power=%s saver=%d term=%s switcher=%d "
		 "quick=%d awake=%d", bs_home_view_id(),
		 bui_dialog_active(), s >= 0 && s <= 2 ? screen_names[s] : "?",
		 x >= 0 && x <= 3 ? xapp_names[x] : "?",
		 bs_power_sleeping() ? "sleeping" : bs_power_menu_open() ? "menu" : "idle", bs_pref.saver,
		 bs_term_state(), bs_switcher_is_open(), bs_bar_quick_open(), bs_term_keep_awake());
}

/*
 * Questo binario non contiene X11 (brainshell, per il Brain): sotto X passa
 * la mano a brainshell-x11 nella stessa cartella.
 */
static void exec_x11(char **argv)
{
	char self[PATH_MAX], *slash;
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 16);

	if (n <= 0)
		return;
	self[n] = 0;
	slash = strrchr(self, '/');
	if (!slash)
		return;
	strcpy(slash + 1, "brainshell-x11");
	execv(self, argv);
	fprintf(stderr, "brainshell: %s: %s\n", self, strerror(errno));
}

int main(int argc, char **argv)
{
	const char *open = NULL;
	bui_win *home, *bar;
	int i;

	clock_gettime(CLOCK_BOOTTIME, &t_start);
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--open") && i + 1 < argc) {
			open = argv[++i];
		} else if (!strcmp(argv[i], "--version")) {
			puts("brainshell " BS_VERSION);
			return 0;
		} else {
			fprintf(stderr, "uso: brainshell [--open ID] [--version]\n");
			return 2;
		}
	}
	i = bui_init();
	if (i == BUI_INIT_NEED_X11) {
		exec_x11(argv);
		return 1;
	}
	if (i < 0) {
		const char *b = getenv("BRAINSHELL_BACKEND"), *d = getenv("DISPLAY");

		/* senza DRM la sessione di brain-preinit ripiega su X (codice 75) */
		if ((b && !strcmp(b, "drm")) || ((!b || !*b) && (!d || !*d))) {
			fprintf(stderr, "brainshell: schermo DRM non disponibile\n");
			return BS_EXIT_NODRM;
		}
		fprintf(stderr, "brainshell: impossibile aprire il display X\n");
		return 1;
	}
	bui_set_state_query(state_query);
	bs_conf_load();
	bs_prefs_apply();
	bs_sys_init();
	bs_power_init();	/* prima di idle: il suo filtro vede per primo il tasto */
	bs_keys_init();		/* tasti del dizionario */
	bs_wm_init();
	home = bs_home_init();
	bar = bs_bar_init();
	if (!home || !bar) {
		fprintf(stderr, "brainshell: impossibile creare le finestre\n");
		return 1;
	}
	w_home = home;
	w_bar = bar;
	bs_wm_set_home(home, bar);
	ready = 1;
	bs_wm_refresh();
	if (open)
		bs_open(open);
	/* il timer scatta al primo giro del ciclo, dopo il primo disegno */
	bui_timer_add(1, 0, after_first_frame, NULL);
	bui_run();
	bs_xapp_shutdown();
	bs_power_shutdown();
	bs_conf_flush();
	bs_idle_shutdown();
	bs_home_shutdown();
	bui_shutdown();
	return 0;
}
