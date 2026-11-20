// SPDX-License-Identifier: MIT
/*
 * sys.c - contatto col sistema: batteria (sysfs), brain-ctl (asincrono),
 * avvio dei programmi esterni, informazioni per le Impostazioni.
 *
 * Niente qui blocca il ciclo dell'interfaccia: brain-ctl gira in un
 * processo figlio, il suo stdout arriva da una pipe dentro poll() e la
 * fine del figlio da SIGCHLD (tramite una pipe su se stessi).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

#define MAX_JOBS 6
#define MAX_LAUNCH 8
#define BATTERY_POLL_MS 30000
#define SET_INTERVAL_MS 140	/* distanza minima fra due "brain-ctl <livello> N" */

#define PS "/sys/class/power_supply/"

struct job {
	pid_t pid;
	int fd;
	int len;
	char out[256];
	void (*done)(int status, const char *out, void *user);
	void *user;
};

/* un livello regolabile (luminosita' o volume) */
struct chan {
	const char *name;
	int ev;
	int cur;	/* valore noto, -1 se sconosciuto */
	int want;	/* valore in attesa di essere inviato, -1 nessuno */
	int sent;	/* ultimo valore inviato */
	int busy;	/* un brain-ctl di impostazione e' in corso */
	unsigned set_seq, get_seq;	/* per scartare letture superate da una scrittura */
	uint32_t last;
	int timer;
};

static struct {
	char root[160];
	const char *ctl;
	bs_battery bat;
	int sig[2];
	struct job jobs[MAX_JOBS];
	struct chan ch[2];
	int screen;		/* BS_SCREEN_* */
	int screen_verb;	/* "brain-ctl screen": 0 da provare, 1 c'e', -1 manca */
	struct {
		pid_t pid;
		uint32_t t;
		char label[32];
	} launch[MAX_LAUNCH];
	struct {
		pid_t pid;
		void (*cb)(int, void *);
		void *user;
	} watch[4];
} S;

/* ------------------------------------------------------------------ */
/* file di sistema                                                    */
/* ------------------------------------------------------------------ */

int bs_read_line(const char *path, char *buf, int len)
{
	char full[320];
	int fd, n;

	snprintf(full, sizeof(full), "%s%s", S.root, path);
	fd = open(full, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, (size_t)len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	buf[strcspn(buf, "\r\n")] = 0;
	return 0;
}

static int read_int(const char *path, long *out)
{
	char b[32], *end;
	long v;

	if (bs_read_line(path, b, sizeof(b)) < 0)
		return -1;
	v = strtol(b, &end, 10);
	if (end == b)
		return -1;
	*out = v;
	return 0;
}

static void battery_read(void)
{
	bs_battery n = { -1, BS_BAT_UNKNOWN, -1, -1 }, old;
	char b[32];
	long v;

	if (read_int(PS "brain-battery/capacity", &v) == 0)
		n.capacity = v < 0 ? 0 : v > 100 ? 100 : (int)v;
	if (bs_read_line(PS "brain-battery/status", b, sizeof(b)) == 0) {
		if (!strcmp(b, "Charging"))
			n.status = BS_BAT_CHARGING;
		else if (!strcmp(b, "Discharging"))
			n.status = BS_BAT_DISCHARGING;
		else if (!strcmp(b, "Full"))
			n.status = BS_BAT_FULL;
		else if (!strcmp(b, "Not charging"))
			n.status = BS_BAT_NOT_CHARGING;
	}
	if (read_int(PS "brain-battery/voltage_now", &v) == 0 && v > 0)
		n.voltage_mv = (int)(v / 1000);
	if (read_int(PS "brain-usb/online", &v) == 0)
		n.usb = v != 0;
	old = S.bat;
	S.bat = n;
	if (memcmp(&n, &old, sizeof(n)))
		bs_notify(BS_EV_BATTERY);
	/* a ogni lettura: la stima dell'autonomia vuole campioni regolari */
	bs_power_battery(&old, &n);
}

void bs_battery_refresh(void)
{
	battery_read();
}

static void battery_timer(void *user)
{
	(void)user;
	battery_read();
}

const bs_battery *bs_battery_get(void)
{
	return &S.bat;
}

const char *bs_battery_status_text(const bs_battery *b)
{
	switch (b->status) {
	case BS_BAT_CHARGING:
		return "In carica";
	case BS_BAT_DISCHARGING:
		return "A batteria";
	case BS_BAT_FULL:
		return "Carica completa";
	case BS_BAT_NOT_CHARGING:
		return "Non in carica";
	default:
		return "\xE2\x80\x94";
	}
}

void bs_sys_kernel(char *buf, int len)
{
	struct utsname u;

	/* con un sysroot di prova si puo' fingere anche la versione */
	if (S.root[0] && bs_read_line("/proc/sys/kernel/osrelease", buf, len) == 0)
		return;
	if (uname(&u) == 0)
		snprintf(buf, (size_t)len, "%s", u.release);
	else
		snprintf(buf, (size_t)len, "\xE2\x80\x94");
}

int bs_sys_meminfo(long *total_kb, long *avail_kb)
{
	char full[320], line[128];
	FILE *f;
	int got = 0;

	snprintf(full, sizeof(full), "%s/proc/meminfo", S.root);
	f = fopen(full, "re");
	if (!f)
		return -1;
	while (got != 3 && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "MemTotal:", 9)) {
			*total_kb = strtol(line + 9, NULL, 10);
			got |= 1;
		} else if (!strncmp(line, "MemAvailable:", 13)) {
			*avail_kb = strtol(line + 13, NULL, 10);
			got |= 2;
		}
	}
	fclose(f);
	return got == 3 ? 0 : -1;
}

int bs_sys_storage(long long *total_bytes, long long *free_bytes)
{
	struct statvfs sv;
	const char *home = getenv("HOME");

	if (statvfs(home && *home ? home : "/", &sv) < 0 && statvfs("/", &sv) < 0)
		return -1;
	*total_bytes = (long long)sv.f_blocks * (long long)sv.f_frsize;
	*free_bytes = (long long)sv.f_bavail * (long long)sv.f_frsize;
	return 0;
}

void bs_fmt_date(char *buf, int len, int with_year /* stile */)
{
	static const char *const days[] = { "domenica", "luned\xC3\xAC", "marted\xC3\xAC",
					    "mercoled\xC3\xAC", "gioved\xC3\xAC", "venerd\xC3\xAC",
					    "sabato" };
	static const char *const months[] = { "gennaio", "febbraio", "marzo", "aprile", "maggio",
					      "giugno", "luglio", "agosto", "settembre", "ottobre",
					      "novembre", "dicembre" };
	time_t t = time(NULL);
	struct tm tm;

	localtime_r(&t, &tm);
	if (with_year == BS_DATE_SHORT)
		snprintf(buf, (size_t)len, "%.3s %d %s", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);
	else if (with_year)
		snprintf(buf, (size_t)len, "%s %d %s %d", days[tm.tm_wday], tm.tm_mday,
			 months[tm.tm_mon], tm.tm_year + 1900);
	else
		snprintf(buf, (size_t)len, "%s %d %s", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon]);
	if (buf[0] >= 'a' && buf[0] <= 'z')
		buf[0] -= 32;
}

/* ------------------------------------------------------------------ */
/* processi figli                                                     */
/* ------------------------------------------------------------------ */

static void on_sigchld(int sig)
{
	int e = errno;
	char c = 1;

	(void)sig;
	if (write(S.sig[1], &c, 1) < 0) {
		/* pipe piena: un byte basta gia' a svegliare il ciclo */
	}
	errno = e;
}

void bs_child_setup(int out_fd)
{
	struct sigaction sa;
	sigset_t none;
	int nul;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	nul = open("/dev/null", O_RDWR);
	if (nul >= 0) {
		dup2(nul, 0);
		if (out_fd < 0 && !getenv("BRAINSHELL_CHILD_OUTPUT")) {
			dup2(nul, 1);
			dup2(nul, 2);
		}
		if (nul > 2)
			close(nul);
	}
	if (out_fd >= 0) {
		dup2(out_fd, 1);
		if (out_fd > 2)
			close(out_fd);
	}
}

static void job_read(struct job *j)
{
	for (;;) {
		char tmp[128];
		int room = (int)sizeof(j->out) - 1 - j->len;
		ssize_t n = read(j->fd, room > 0 ? j->out + j->len : tmp,
				 room > 0 ? (size_t)room : sizeof(tmp));

		if (n > 0) {
			if (room > 0)
				j->len += (int)n;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			/* fine del flusso: smette di sorvegliarlo */
			bui_fd_del(j->fd);
			close(j->fd);
			j->fd = -1;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return;
	}
}

static void job_readable(int fd, void *user)
{
	struct job *j = user;

	if (j->pid > 0 && j->fd == fd)
		job_read(j);
}

static int job_spawn(const char *const argv[], void (*done)(int, const char *, void *), void *user)
{
	struct job *j = NULL;
	int i, pfd[2];
	pid_t pid;

	for (i = 0; i < MAX_JOBS && !j; i++)
		if (!S.jobs[i].pid)
			j = &S.jobs[i];
	if (!j || pipe(pfd) < 0)
		return -1;
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		bs_child_setup(pfd[1]);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pfd[1]);
	j->pid = pid;
	j->fd = pfd[0];
	j->len = 0;
	j->done = done;
	j->user = user;
	bui_fd_add(j->fd, job_readable, j);
	return 0;
}

int bs_child_watch(int pid, void (*cb)(int status, void *user), void *user)
{
	int i;

	for (i = 0; i < 4; i++)
		if (!S.watch[i].pid) {
			S.watch[i].pid = pid;
			S.watch[i].cb = cb;
			S.watch[i].user = user;
			return 0;
		}
	return -1;
}

static void reap(pid_t pid, int st)
{
	int i, code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;

	for (i = 0; i < 4; i++)
		if (S.watch[i].pid == pid) {
			void (*cb)(int, void *) = S.watch[i].cb;
			void *user = S.watch[i].user;

			S.watch[i].pid = 0;
			cb(st, user);
			return;
		}

	for (i = 0; i < MAX_JOBS; i++) {
		struct job *j = &S.jobs[i];
		void (*done)(int, const char *, void *);
		void *user;
		char out[sizeof(j->out)];

		if (j->pid != pid)
			continue;
		if (j->fd >= 0) {
			job_read(j);
			if (j->fd >= 0) {
				bui_fd_del(j->fd);
				close(j->fd);
			}
		}
		j->out[j->len] = 0;
		while (j->len > 0 && (j->out[j->len - 1] == '\n' || j->out[j->len - 1] == '\r'))
			j->out[--j->len] = 0;
		memcpy(out, j->out, sizeof(out));
		done = j->done;
		user = j->user;
		memset(j, 0, sizeof(*j));	/* libero prima della callback: puo' avviarne un altro */
		if (done)
			done(code, out, user);
		return;
	}
	for (i = 0; i < MAX_LAUNCH; i++) {
		if (S.launch[i].pid != pid)
			continue;
		S.launch[i].pid = 0;
		if (code == 127 && (int32_t)(bui_now() - S.launch[i].t) < 5000) {
			char msg[96];

			snprintf(msg, sizeof(msg), "Impossibile avviare %s", S.launch[i].label);
			bui_toast(msg, 3000);
		}
		return;
	}
}

static void on_sigterm(int sig)
{
	int e = errno;
	char c = 'q';

	(void)sig;
	if (write(S.sig[1], &c, 1) < 0) {
		/* niente da fare */
	}
	errno = e;
}

static void sig_readable(int fd, void *user)
{
	char buf[32];
	pid_t pid;
	ssize_t n;
	int st;

	(void)user;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		if (memchr(buf, 'q', (size_t)n))
			bui_quit();	/* SIGTERM / SIGINT: esce dal ciclo e chiude con ordine */
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
		reap(pid, st);
}

int bs_launch(const char *const argv[], const char *label)
{
	const char *home = getenv("HOME");
	pid_t pid;
	int i;

	/* senza X le app esterne partono in una sessione X tutta loro */
	if (!bui_backend_is_x11())
		return bs_xapp_launch(argv, label);
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		setsid();
		bs_child_setup(-1);
		if (home && chdir(home) < 0) {
			/* si resta dove si e' */
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	for (i = 0; i < MAX_LAUNCH; i++)
		if (!S.launch[i].pid) {
			S.launch[i].pid = pid;
			S.launch[i].t = bui_now();
			snprintf(S.launch[i].label, sizeof(S.launch[i].label), "%s", label ? label : argv[0]);
			break;
		}
	return 0;
}

/* ------------------------------------------------------------------ */
/* brain-ctl                                                          */
/* ------------------------------------------------------------------ */

void bs_ctl_arg(const char *verb, const char *arg, void (*done)(int, const char *, void *), void *user)
{
	const char *argv[4] = { S.ctl, verb, arg, NULL };

	if (job_spawn(argv, done, user) < 0 && done)
		done(-1, "", user);
}

void bs_ctl(const char *verb, void (*done)(int, const char *, void *), void *user)
{
	const char *argv[3] = { S.ctl, verb, NULL };

	if (job_spawn(argv, done, user) < 0 && done)
		done(-1, "", user);
}

static void chan_send(struct chan *c);

static void chan_timer(void *user)
{
	struct chan *c = user;

	c->timer = 0;
	if (!c->busy && c->want >= 0)
		chan_send(c);
}

static void chan_set_done(int status, const char *out, void *user)
{
	struct chan *c = user;

	(void)status;
	(void)out;
	c->busy = 0;
	if (c->want >= 0 && !c->timer) {
		int wait = SET_INTERVAL_MS - (int32_t)(bui_now() - c->last);

		if (wait > 0)
			c->timer = bui_timer_add(wait, 0, chan_timer, c);
		else
			chan_send(c);
	}
}

static void chan_send(struct chan *c)
{
	char num[16];
	const char *argv[4] = { S.ctl, c->name, num, NULL };

	snprintf(num, sizeof(num), "%d", c->want);
	c->sent = c->want;
	c->want = -1;
	c->set_seq++;
	c->last = bui_now();
	if (job_spawn(argv, chan_set_done, c) == 0)
		c->busy = 1;
}

static void chan_set(struct chan *c, int v, int final)
{
	int wait;

	v = v < 0 ? 0 : v > 100 ? 100 : v;
	if (v != c->cur) {
		c->cur = v;
		bs_notify(c->ev);
	}
	if (v == c->sent && c->want < 0)
		return;			/* gia' inviato, niente in attesa */
	c->want = v;
	if (c->busy)
		return;			/* partira' quando finisce quello in corso */
	wait = SET_INTERVAL_MS - (int32_t)(bui_now() - c->last);
	if (wait > 0 && !final) {
		if (!c->timer)
			c->timer = bui_timer_add(wait, 0, chan_timer, c);
		return;
	}
	if (c->timer) {
		bui_timer_del(c->timer);
		c->timer = 0;
	}
	chan_send(c);
}

static void chan_get_done(int status, const char *out, void *user)
{
	struct chan *c = user;
	char *end;
	long v = strtol(out, &end, 10);

	if (status != 0 || end == out || c->busy || c->want >= 0 || c->get_seq != c->set_seq)
		return;		/* nessuna risposta utile, o l'utente ha regolato nel frattempo */
	if (c == &S.ch[0] && S.screen != BS_SCREEN_ON)
		return;		/* schermo attenuato: quello letto non e' il livello dell'utente */
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	if ((int)v != c->cur) {
		c->cur = (int)v;
		bs_notify(c->ev);
	}
}

void bs_refresh_levels(void)
{
	int i;

	for (i = 0; i < 2; i++) {
		const char *argv[3] = { S.ctl, S.ch[i].name, NULL };

		S.ch[i].get_seq = S.ch[i].set_seq;
		job_spawn(argv, chan_get_done, &S.ch[i]);
	}
}

/* ------------------------------------------------------------------ */
/* schermo: attenuato / spento per inattivita'                        */
/* ------------------------------------------------------------------ */

/* invia un livello di luminosita' senza toccare quello scelto dall'utente */
static void bright_raw(int v)
{
	struct chan *c = &S.ch[0];

	if (v == c->sent && c->want < 0)
		return;
	c->want = v;
	if (c->busy)
		return;		/* partira' quando finisce quello in corso */
	if (c->timer) {
		bui_timer_del(c->timer);
		c->timer = 0;
	}
	chan_send(c);
}

static void screen_off_done(int status, const char *out, void *user)
{
	(void)out;
	(void)user;
	if (status == 0) {
		S.screen_verb = 1;
		return;
	}
	/* helper senza il verbo "screen": si ripiega sulla luminosita' a zero */
	S.screen_verb = -1;
	if (S.screen == BS_SCREEN_OFF && S.ch[0].cur >= 0)
		bright_raw(0);
}

static void screen_verb(const char *arg, void (*done)(int, const char *, void *))
{
	const char *argv[4] = { S.ctl, "screen", arg, NULL };

	if (job_spawn(argv, done, NULL) < 0 && done)
		done(-1, "", NULL);
}

int bs_screen_state(void)
{
	return S.screen;
}

void bs_screen_set(int state)
{
	int old = S.screen, cur = S.ch[0].cur;

	if (state == old)
		return;
	S.screen = state;
	S.ch[0].set_seq++;	/* una lettura in corso non vale piu' */
	switch (state) {
	case BS_SCREEN_DIM:
		if (cur >= 0)
			bright_raw(cur * 3 / 10 > 3 ? cur * 3 / 10 : cur < 3 ? cur : 3);
		break;
	case BS_SCREEN_OFF:
		if (S.screen_verb >= 0)
			screen_verb("off", screen_off_done);
		else if (cur >= 0)
			bright_raw(0);
		break;
	default:
		/* anche se la risposta a "screen off" non e' ancora arrivata */
		if (old == BS_SCREEN_OFF && S.screen_verb >= 0)
			screen_verb("on", NULL);
		if (cur >= 0)
			bright_raw(cur);
		break;
	}
	bs_notify(BS_EV_SCREEN);
}

int bs_brightness(void) { return S.ch[0].cur; }
int bs_volume(void) { return S.ch[1].cur; }
void bs_set_brightness(int v, int final)
{
	/* risparmio energetico: al massimo il 40%; al rilascio il cursore torna li' */
	if (bs_pref.saver && v > BS_SAVER_BRIGHTNESS) {
		chan_set(&S.ch[0], BS_SAVER_BRIGHTNESS, final);
		if (final)
			bs_notify(BS_EV_BRIGHTNESS);
		return;
	}
	chan_set(&S.ch[0], v, final);
}
void bs_set_volume(int v, int final) { chan_set(&S.ch[1], v, final); }

/* ------------------------------------------------------------------ */

void bs_sys_init(void)
{
	const char *root = getenv("BRAIN_SYSROOT");
	const char *ctl = getenv("BRAIN_CTL");
	struct sigaction sa;

	snprintf(S.root, sizeof(S.root), "%s", root ? root : "");
	S.ctl = ctl && *ctl ? ctl : "/usr/local/bin/brain-ctl";
	S.ch[0].name = "brightness";
	S.ch[0].ev = BS_EV_BRIGHTNESS;
	S.ch[1].name = "volume";
	S.ch[1].ev = BS_EV_VOLUME;
	S.ch[0].cur = S.ch[1].cur = -1;
	S.ch[0].want = S.ch[1].want = -1;
	S.ch[0].sent = S.ch[1].sent = -1;
	S.bat.capacity = S.bat.voltage_mv = S.bat.usb = -1;

	if (pipe(S.sig) == 0) {
		fcntl(S.sig[0], F_SETFD, FD_CLOEXEC);
		fcntl(S.sig[1], F_SETFD, FD_CLOEXEC);
		fcntl(S.sig[0], F_SETFL, O_NONBLOCK);
		fcntl(S.sig[1], F_SETFL, O_NONBLOCK);
		bui_fd_add(S.sig[0], sig_readable, NULL);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigchld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	sa.sa_handler = on_sigterm;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	battery_read();
	{
		/* BRAINSHELL_BATTERY_MS: lettura piu' frequente, solo per le prove */
		const char *e = getenv("BRAINSHELL_BATTERY_MS");
		int ms = e && atoi(e) >= 100 ? atoi(e) : BATTERY_POLL_MS;

		bui_timer_add(ms, ms, battery_timer, NULL);
	}
}
