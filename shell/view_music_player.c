// SPDX-License-Identifier: MIT
/*
 * view_music_player.c - brainplay come processo figlio in modalita' --slave.
 *
 *	brainplay --slave --no-window FILE	($BRAIN_PLAYER al posto di brainplay)
 *
 * Comandi sul suo stdin (una riga ciascuno): pause, play, toggle,
 * seek <secondi>, volume <0-100>, quit. Dal suo stdout arrivano righe
 * "chiave valore": state playing|paused|ended, pos 12.3, dur 240.0,
 * title ..., artist ..., volume N. Lo stdout e' una pipe non bloccante
 * dentro il poll() del toolkit (bui_fd_add): la shell non aspetta mai il
 * lettore e, quando il lettore tace (pausa), non si sveglia.
 *
 * Un processo per brano. Lo stato sopravvive alla vista Musica: la musica
 * continua mentre si usa altro. Alla chiusura della shell (atexit) il
 * lettore viene fermato; se la shell muore di colpo ci pensano
 * PR_SET_PDEATHSIG e la chiusura del suo stdin. I figli vengono raccolti dal
 * SIGCHLD di sys.c (waitpid(-1)): niente zombie.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "media.h"

/* fuori dalla struttura qui sotto: resta in .bss (4 kB di percorso) */
static mp_status ST;

static struct {
	pid_t pid;
	int in, out;		/* stdin e stdout del lettore (-1: nessun processo) */
	char line[512];
	int len;
	unsigned gen;		/* cambia ogni volta che i canali vengono chiusi */
	int got_status;		/* il lettore ha detto qualcosa: e' partito davvero */
	int want_volume;	/* volume scelto dall'utente (-1: quello del lettore) */
	int atexit_set;
	void (*listener)(int ev);
} P = { .in = -1, .out = -1, .want_volume = -1 };

const mp_status *mp_get(void)
{
	static int init;

	if (!init) {
		ST.volume = 100;	/* finche' il lettore non dice il suo */
		init = 1;
	}
	return &ST;
}

void mp_set_listener(void (*cb)(int ev))
{
	P.listener = cb;
}

const char *mp_program(void)
{
	const char *p = getenv("BRAIN_PLAYER");

	return p && *p ? p : "brainplay";
}

static void emit(int ev)
{
	if (P.listener)
		P.listener(ev);
}

static void cmd(const char *fmt, int arg)
{
	char buf[48];
	int n = snprintf(buf, sizeof(buf), fmt, arg);

	/* pipe non bloccante: se il lettore non legge, il comando si perde */
	if (P.in >= 0 && write(P.in, buf, (size_t)n) < 0 && errno == EPIPE) {
		close(P.in);
		P.in = -1;
	}
}

/* chiude i canali verso il lettore; kill = 1 se deve anche smettere di suonare */
static void drop(int kill_it)
{
	struct pollfd pf;

	if (P.out < 0 && P.in < 0)
		return;
	if (kill_it) {
		cmd("quit\n", 0);
		/* SIGTERM solo se e' ancora vivo: uno stdout gia' chiuso (POLLHUP)
		 * vuol dire che e' uscito e il suo pid potrebbe non essere piu' suo */
		pf.fd = P.out;
		pf.events = POLLIN;
		pf.revents = 0;
		if (P.out >= 0 && P.pid > 0 && poll(&pf, 1, 0) >= 0 && !(pf.revents & POLLHUP))
			kill(P.pid, SIGTERM);
	}
	if (P.out >= 0) {
		bui_fd_del(P.out);
		close(P.out);
	}
	if (P.in >= 0)
		close(P.in);
	P.in = P.out = -1;
	P.pid = 0;
	P.len = 0;
	P.gen++;
}

/* "12.3" -> 123 (decimi), senza virgola mobile */
static int parse_ds(const char *s)
{
	long v = 0;
	int frac = 0;

	while (*s >= '0' && *s <= '9' && v < 100000000)
		v = v * 10 + (*s++ - '0');
	if (*s == '.' && s[1] >= '0' && s[1] <= '9')
		frac = s[1] - '0';
	return (int)(v * 10 + frac);
}

static void handle_line(char *s)
{
	char *val = strchr(s, ' ');
	int v;

	if (val)
		*val++ = 0;
	else
		val = s + strlen(s);
	P.got_status = 1;
	if (!strcmp(s, "pos")) {
		v = parse_ds(val);
		if (v != ST.pos_ds) {
			ST.pos_ds = v;
			emit(MP_EV_POS);
		}
	} else if (!strcmp(s, "dur")) {
		v = parse_ds(val);
		if (v != ST.dur_ds) {
			ST.dur_ds = v;
			emit(MP_EV_POS);
		}
	} else if (!strcmp(s, "state")) {
		if (!strcmp(val, "ended")) {
			/* il brano e' finito: il processo non serve piu' */
			drop(1);
			ST.state = MP_STOPPED;
			ST.pos_ds = 0;
			emit(MP_EV_ENDED);
			return;
		}
		v = !strcmp(val, "playing") ? MP_PLAYING : MP_PAUSED;
		if (v != ST.state) {
			ST.state = v;
			emit(MP_EV_STATE);
		}
	} else if (!strcmp(s, "title")) {
		ms_clean(ST.title, sizeof(ST.title), val, -1);
		emit(MP_EV_META);
	} else if (!strcmp(s, "artist")) {
		ms_clean(ST.artist, sizeof(ST.artist), val, -1);
		emit(MP_EV_META);
	} else if (!strcmp(s, "volume") && P.want_volume < 0) {
		/* finche' l'utente non lo tocca vale il volume del lettore */
		v = atoi(val);
		v = v < 0 ? 0 : v > 100 ? 100 : v;
		if (v != ST.volume) {
			ST.volume = v;
			emit(MP_EV_VOLUME);
		}
	}
	/* il resto (audio, video, album, error) non interessa */
}

static void out_readable(int fd, void *user)
{
	unsigned gen = P.gen;

	(void)user;
	/* una riga puo' chiudere il processo e avviarne un altro (anche con lo
	 * stesso numero di descrittore): gen dice se e' ancora quello di prima */
	while (P.out == fd && P.gen == gen) {
		ssize_t n = read(fd, P.line + P.len, sizeof(P.line) - 1 - (size_t)P.len);
		char *p, *nl;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n <= 0) {
			/* il lettore e' uscito da solo: mai partito, oppure morto */
			int started = P.got_status;

			drop(0);
			ST.state = MP_STOPPED;
			ST.pos_ds = 0;
			emit(started ? MP_EV_STATE : MP_EV_ERROR);
			return;
		}
		P.len += (int)n;
		P.line[P.len] = 0;
		for (p = P.line; P.gen == gen && (nl = memchr(p, '\n', (size_t)(P.line + P.len - p))); p = nl + 1) {
			*nl = 0;
			handle_line(p);
		}
		if (P.gen != gen)
			return;
		P.len -= (int)(p - P.line);
		memmove(P.line, p, (size_t)P.len);
		if (P.len >= (int)sizeof(P.line) - 1)
			P.len = 0;	/* riga troppo lunga: scartata */
	}
}

static void at_exit(void)
{
	pid_t pid = P.pid;
	int i;

	if (pid <= 0)
		return;
	drop(1);
	/* un attimo per lasciarlo chiudere l'audio, senza mai restare appesi */
	for (i = 0; i < 20 && waitpid(pid, NULL, WNOHANG) == 0; i++)
		usleep(10000);
}

int mp_play(const char *path)
{
	int to[2], from[2], nul;
	sigset_t all, old;
	pid_t pid;

	mp_get();
	drop(1);
	snprintf(ST.path, sizeof(ST.path), "%s", path);
	ST.title[0] = ST.artist[0] = 0;
	ST.pos_ds = ST.dur_ds = 0;
	ST.state = MP_STOPPED;
	P.got_status = 0;
	if (!P.atexit_set) {
		atexit(at_exit);
		P.atexit_set = 1;
	}
	if (pipe(to) < 0)
		goto fail;
	if (pipe(from) < 0) {
		close(to[0]);
		close(to[1]);
		goto fail;
	}
	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, &old);
	pid = fork();
	if (pid == 0) {
		ms_child_reset();
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		dup2(to[0], 0);
		dup2(from[1], 1);
		if (!getenv("BRAINSHELL_CHILD_OUTPUT") && (nul = open("/dev/null", O_WRONLY)) >= 0) {
			dup2(nul, 2);
			if (nul > 2)
				close(nul);
		}
		close(to[0]);
		close(to[1]);
		close(from[0]);
		close(from[1]);
		execlp(mp_program(), mp_program(), "--slave", "--no-window", path, (char *)NULL);
		_exit(127);
	}
	sigprocmask(SIG_SETMASK, &old, NULL);
	close(to[0]);
	close(from[1]);
	if (pid < 0) {
		close(to[1]);
		close(from[0]);
		goto fail;
	}
	fcntl(to[1], F_SETFD, FD_CLOEXEC);
	fcntl(to[1], F_SETFL, O_NONBLOCK);
	fcntl(from[0], F_SETFD, FD_CLOEXEC);
	fcntl(from[0], F_SETFL, O_NONBLOCK);
	P.pid = pid;
	P.in = to[1];
	P.out = from[0];
	P.len = 0;
	bui_fd_add(P.out, out_readable, NULL);
	if (P.want_volume >= 0)
		cmd("volume %d\n", P.want_volume);
	/* ottimista: se il lettore non parte arriva MP_EV_ERROR */
	ST.state = MP_PLAYING;
	emit(MP_EV_STATE);
	return 0;
fail:
	emit(MP_EV_ERROR);
	return -1;
}

void mp_stop(void)
{
	drop(1);
	ST.state = MP_STOPPED;
	ST.pos_ds = 0;
	emit(MP_EV_STATE);
}

void mp_toggle(void)
{
	if (P.out < 0) {
		/* fermo (fine dell'elenco o errore): riparte dall'inizio */
		if (ST.path[0]) {
			char path[sizeof(ST.path)];

			memcpy(path, ST.path, sizeof(path));
			mp_play(path);
		}
		return;
	}
	cmd(ST.state == MP_PLAYING ? "pause\n" : "play\n", 0);
}

void mp_pause(void)
{
	if (P.out >= 0 && ST.state == MP_PLAYING)
		cmd("pause\n", 0);
}

void mp_seek(int seconds)
{
	if (P.out < 0)
		return;
	cmd("seek %d\n", seconds < 0 ? 0 : seconds);
}

void mp_volume(int v)
{
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	mp_get();
	P.want_volume = v;
	if (v == ST.volume)
		return;
	ST.volume = v;
	cmd("volume %d\n", v);
}
