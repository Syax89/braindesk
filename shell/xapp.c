// SPDX-License-Identifier: MIT
/*
 * xapp.c - app X esterne su richiesta, quando la shell gira senza X
 * (backend drm, o mem nelle prove).
 *
 * Una sola app alla volta, che prende tutto lo schermo in una sessione X
 * fatta apposta e che finisce con lei:
 *
 *   IDLE --bs_xapp_launch--> STARTING --250 ms--> (cede master DRM e
 *   ingresso) RUNNING: "$BRAINSHELL_XORG ARGV..." --uscita del processo-->
 *   RETURNING (riprende master e ingresso; se il master non e' ancora
 *   libero riprova) --> IDLE, schermo ricomposto dalle tele.
 *
 * I 250 ms di STARTING lasciano salire il toast "Apertura di ..." che resta
 * sul pannello mentre X parte (nessuno ridisegna quelle righe finche' X non
 * imposta il suo framebuffer).
 *
 * BRAINSHELL_XORG e' il comando (parole separate da spazi) che deve
 * avviare il server X, eseguire l'app che segue e uscire quando l'app
 * esce: di default "sudo -n /usr/local/sbin/brain-xapp" (os/early/brain-xapp:
 * xinit con la stessa configurazione di brain-ui, nessun gestore di
 * finestre). Se scrive "ready" sul suo stdout (l'app sta partendo) il
 * tempo viene annotato con BRAINSHELL_TIMING. Le prove usano un finto Xorg
 * (test/fake-xorg).
 *
 * Il ritorno: l'uscita dell'app. In piu', mentre l'app ha lo schermo, i
 * dispositivi evdev restano aperti senza cattura e Ctrl+Alt+Esc (sul Brain:
 * PagSu + Onsei + Home) manda SIGTERM al comando, che chiude app e X.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

#define MAX_ARGS 32
#define HANDOVER_MS 250

static struct {
	int state;
	pid_t pid;
	int out_fd;
	char label[48];
	char store[2048];
	char *argv[MAX_ARGS + 1];
	int nargv;
	int timer, kill_timer, retry_timer;
	int kills, retries;
	int filter;
	int ready;
	uint32_t t_launch, t_start;
	int status;
} A = { .out_fd = -1 };

static int timing(void)
{
	return getenv("BRAINSHELL_TIMING") != NULL;
}

int bs_xapp_state(void)
{
	return A.state;
}

static int add_arg(const char *s, size_t *used)
{
	size_t n = strlen(s) + 1;

	if (A.nargv >= MAX_ARGS || *used + n > sizeof(A.store))
		return -1;
	memcpy(A.store + *used, s, n);
	A.argv[A.nargv++] = A.store + *used;
	A.argv[A.nargv] = NULL;
	*used += n;
	return 0;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');

	return s ? s + 1 : p;
}

/*
 * Comando completo: le parole di BRAINSHELL_XORG, poi l'app. Senza gestore
 * di finestre st si apre grande quanto chiede: se non ha gia' -g gli si da'
 * la griglia che riempie lo schermo (celle di BRAINSHELL_ST_CELL, "8x16"
 * per DejaVu Sans Mono 10 a 96 dpi).
 */
static int build(const char *const argv[])
{
	const char *x = getenv("BRAINSHELL_XORG");
	char words[512], *w, *save = NULL, geo[32];
	size_t used = 0;
	int i, has_g = 0, cw = 8, ch = 16;

	A.nargv = 0;
	snprintf(words, sizeof(words), "%s", x && *x ? x : "sudo -n /usr/local/sbin/brain-xapp");
	for (w = strtok_r(words, " \t", &save); w; w = strtok_r(NULL, " \t", &save))
		if (add_arg(w, &used) < 0)
			return -1;
	for (i = 0; argv[i]; i++) {
		if (add_arg(argv[i], &used) < 0)
			return -1;
		if (!strcmp(argv[i], "-g"))
			has_g = 1;
	}
	if (!strcmp(base_name(argv[0]), "st") && !has_g) {
		const char *cell = getenv("BRAINSHELL_ST_CELL");

		if (cell && sscanf(cell, "%dx%d", &cw, &ch) != 2)
			cw = 8, ch = 16;
		if (cw < 4 || ch < 6)
			cw = 8, ch = 16;
		/* st lascia 2 px di bordo per lato */
		snprintf(geo, sizeof(geo), "%dx%d", (bui_screen_w() - 4) / cw, (bui_screen_h() - 4) / ch);
		if (add_arg("-g", &used) < 0 || add_arg(geo, &used) < 0)
			return -1;
	}
	return 0;
}

static void try_resume(void *user);

static void on_out(int fd, void *user)
{
	char buf[256];
	ssize_t n;

	(void)user;
	for (;;) {
		n = read(fd, buf, sizeof(buf) - 1);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		buf[n] = 0;
		if (!A.ready && strstr(buf, "ready")) {
			A.ready = 1;
			if (timing())
				fprintf(stderr, "brainshell: %s: sessione X pronta dopo %u ms\n", A.label,
					(unsigned)(bui_now() - A.t_start));
		}
	}
	if (n == 0 || (n < 0 && errno != EAGAIN)) {
		bui_fd_del(fd);
		close(fd);
		if (A.out_fd == fd)
			A.out_fd = -1;
	}
}

static void finish(void)
{
	uint32_t ran = bui_now() - A.t_start;
	int code = WIFEXITED(A.status) ? WEXITSTATUS(A.status) : -1;
	char msg[96];

	A.state = BS_XAPP_IDLE;
	A.retries = 0;
	if (timing())
		fprintf(stderr, "brainshell: %s: di nuovo la shell dopo %u ms (uscita %d)\n", A.label,
			(unsigned)(bui_now() - A.t_launch), code);
	/* partita male: il comando non c'e', sudo rifiuta, X non parte */
	if (code == 127 || (code != 0 && !A.ready && ran < 5000)) {
		snprintf(msg, sizeof(msg), "Impossibile avviare %s", A.label);
		bui_toast(msg, 3000);
	}
	bs_notify(BS_EV_WINDOWS);
}

static void try_resume(void *user)
{
	(void)user;
	A.retry_timer = 0;
	if (bui_suspend(0) == 0) {
		finish();
		return;
	}
	/* il master non e' ancora libero (X sta finendo): riprova */
	A.retries++;
	if (A.retries == 30)
		fprintf(stderr, "brainshell: lo schermo non torna libero, continuo a riprovare\n");
	A.retry_timer = bui_timer_add(A.retries < 30 ? 100 : 1000, 0, try_resume, NULL);
}

static void on_exit_child(int status, void *user)
{
	(void)user;
	A.pid = 0;
	A.status = status;
	bui_timer_del(A.kill_timer);
	A.kill_timer = 0;
	if (A.out_fd >= 0)
		on_out(A.out_fd, NULL);	/* l'ultimo "ready" e la chiusura */
	if (A.out_fd >= 0) {
		bui_fd_del(A.out_fd);
		close(A.out_fd);
		A.out_fd = -1;
	}
	A.state = BS_XAPP_RETURNING;
	try_resume(NULL);
}

static void handover(void *user)
{
	int pfd[2];
	pid_t pid;
	char scr[32];

	(void)user;
	A.timer = 0;
	if (bui_suspend(1) < 0) {
		A.state = BS_XAPP_IDLE;
		bui_toast("Impossibile cedere lo schermo", 3000);
		return;
	}
	if (pipe(pfd) < 0) {
		pfd[0] = pfd[1] = -1;
	} else {
		fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
		fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	}
	snprintf(scr, sizeof(scr), "%dx%d", bui_screen_w(), bui_screen_h());
	A.t_start = bui_now();
	pid = fork();
	if (pid == 0) {
		bs_child_setup(pfd[1]);
		setenv("BRAIN_SCREEN", scr, 1);
		execvp(A.argv[0], A.argv);
		_exit(127);
	}
	if (pfd[1] >= 0)
		close(pfd[1]);
	if (pid < 0) {
		if (pfd[0] >= 0)
			close(pfd[0]);
		A.status = 127 << 8;
		A.state = BS_XAPP_RETURNING;
		try_resume(NULL);
		return;
	}
	A.pid = pid;
	A.ready = 0;
	A.kills = 0;
	A.out_fd = pfd[0];
	if (A.out_fd >= 0)
		bui_fd_add(A.out_fd, on_out, NULL);
	bs_child_watch(pid, on_exit_child, NULL);
	A.state = BS_XAPP_RUNNING;
}

static void kill_again(void *user)
{
	static const int sigs[] = { SIGTERM, SIGINT, SIGHUP };

	(void)user;
	A.kill_timer = 0;
	if (!A.pid)
		return;
	/* niente SIGKILL: sudo non lo puo' inoltrare e X resterebbe su */
	kill(A.pid, sigs[A.kills % 3]);
	A.kills++;
	A.kill_timer = bui_timer_add(3000, 0, kill_again, NULL);
}

void bs_xapp_stop(void)
{
	if (A.state != BS_XAPP_RUNNING || !A.pid || A.kill_timer)
		return;
	kill_again(NULL);
}

/* a schermo ceduto: Ctrl+Alt+Esc riporta alla shell */
static int filter(int kind, const bui_key *k, int suspended)
{
	if (suspended && kind == BUI_IN_KEY && k && k->key == BUI_K_ESC &&
	    (k->mods & (BUI_MOD_CTRL | BUI_MOD_ALT)) == (BUI_MOD_CTRL | BUI_MOD_ALT))
		bs_xapp_stop();
	return 0;
}

int bs_xapp_launch(const char *const argv[], const char *label)
{
	char msg[96];

	if (!argv || !argv[0])
		return -1;
	if (A.state != BS_XAPP_IDLE) {
		snprintf(msg, sizeof(msg), "%s e' gia' aperto", A.label);
		bui_toast(msg, 2000);
		return -1;
	}
	if (build(argv) < 0) {
		bui_toast("Comando troppo lungo", 2000);
		return -1;
	}
	if (!A.filter) {
		bui_add_input_filter(filter);
		A.filter = 1;
	}
	snprintf(A.label, sizeof(A.label), "%s", label ? label : argv[0]);
	A.state = BS_XAPP_STARTING;
	A.t_launch = bui_now();
	A.timer = bui_timer_add(HANDOVER_MS, 0, handover, NULL);
	return 0;
}

void bs_xapp_shutdown(void)
{
	int i, st;

	bui_timer_del(A.timer);
	bui_timer_del(A.kill_timer);
	bui_timer_del(A.retry_timer);
	A.timer = A.kill_timer = A.retry_timer = 0;
	if (!A.pid)
		return;
	/* X non deve sopravvivere alla shell: terrebbe il master e la shell
	 * riavviata non potrebbe ridisegnare. Si aspetta al massimo ~3 s */
	kill(A.pid, SIGTERM);
	for (i = 0; i < 60; i++) {
		struct timespec ts = { 0, 50 * 1000000L };

		if (waitpid(A.pid, &st, WNOHANG) == A.pid)
			break;
		if (i == 40)
			kill(A.pid, SIGHUP);
		nanosleep(&ts, NULL);
	}
	A.pid = 0;
	A.state = BS_XAPP_IDLE;
}
