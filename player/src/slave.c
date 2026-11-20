// SPDX-License-Identifier: MIT
/*
 * Remote control (--slave): line commands on stdin, status lines on stdout.
 *
 * Commands:
 *   play | pause | toggle
 *   seek <seconds>        absolute position
 *   seek +<s> | seek -<s> relative
 *   volume <0-100> | volume +<n> | volume -<n>
 *   status                print everything again
 *   quit
 *
 * Status lines (one "key value" per line):
 *   title <text> / artist <text> / album <text>
 *   video <codec> <w>x<h> ...   audio <codec> <rate> Hz <n> ch
 *   dur <seconds>         0.0 when unknown
 *   volume <0-100>
 *   state playing|paused|ended
 *   pos <seconds>         at most twice per second, and after every seek
 *   error <text>          a command that was not understood
 *
 * If stdin is a pipe or a socket and the other end closes it, the player
 * quits (the launcher is gone); with any other kind of stdin the end of
 * input just ends the remote control.
 */
#include "brainplay.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct {
	int fd;
	char line[256];
	int len;
	bool overflow;
	bool out_dead;
	int last_state;
	int last_volume;
	double last_pos;
	double last_dur;
	double next_pos_at;
} sl = { .fd = -1 };

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void out(const char *fmt, ...)
{
	va_list ap;

	if (sl.out_dead)
		return;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	if (fflush(stdout) == EOF) {
		/* nobody is listening any more */
		sl.out_dead = true;
		clearerr(stdout);
	}
}

int slave_fd(void)
{
	return sl.fd;
}

static const char *state_name(int s)
{
	return s == ST_PLAYING ? "playing" : s == ST_PAUSED ? "paused" : "ended";
}

static void print_all(struct player *p)
{
	out("title %s", p->title);
	if (p->artist[0])
		out("artist %s", p->artist);
	if (p->album[0])
		out("album %s", p->album);
	if (p->vst)
		out("video %s", p->vinfo);
	if (p->ast)
		out("audio %s", p->ainfo);
	out("dur %.1f", p->duration);
	sl.last_dur = p->duration;
	sl.last_volume = atomic_load(&p->volume);
	out("volume %d", sl.last_volume);
	sl.last_state = player_state(p);
	out("state %s", state_name(sl.last_state));
	sl.last_pos = player_pos(p);
	out("pos %.1f", sl.last_pos);
	sl.next_pos_at = now_sec() + 0.5;
}

void slave_init(struct player *p)
{
	sl.fd = STDIN_FILENO;
	setvbuf(stdout, NULL, _IOLBF, 0);
	print_all(p);
}

static void command(struct player *p, char *line, bool *quit)
{
	char *cmd = strtok(line, " \t\r"), *arg = strtok(NULL, " \t\r");

	if (!cmd)
		return;
	if (!strcmp(cmd, "pause")) {
		player_set_paused(p, true);
	} else if (!strcmp(cmd, "play")) {
		if (player_state(p) != ST_PLAYING)
			player_toggle(p);	/* also restarts a file that ended */
	} else if (!strcmp(cmd, "toggle")) {
		player_toggle(p);
	} else if (!strcmp(cmd, "seek") && arg) {
		char *end;
		double v = strtod(arg, &end);

		if (end == arg || !isfinite(v)) {
			out("error bad position: %s", arg);
		} else if (arg[0] == '+' || arg[0] == '-') {
			player_seek_rel(p, v);
		} else {
			player_seek(p, v, 0);
		}
		sl.next_pos_at = 0;	/* report the new position at once */
		sl.last_pos = -1;
	} else if (!strcmp(cmd, "volume") && arg) {
		char *end;
		long v = strtol(arg, &end, 10);

		if (end == arg)
			out("error bad volume: %s", arg);
		else if (arg[0] == '+' || arg[0] == '-')
			player_set_volume(p, atomic_load(&p->volume) + (int)v);
		else
			player_set_volume(p, (int)v);
	} else if (!strcmp(cmd, "status")) {
		print_all(p);
	} else if (!strcmp(cmd, "quit") || !strcmp(cmd, "stop")) {
		*quit = true;
	} else {
		out("error unknown command: %s", cmd);
	}
}

/* called when poll() says that stdin has something (or was closed) */
bool slave_read(struct player *p)
{
	bool quit = false;
	char buf[256];
	ssize_t n, i;

	if (sl.fd < 0)
		return !sl.out_dead;
	do
		n = read(sl.fd, buf, sizeof(buf));
	while (n < 0 && errno == EINTR);
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			sl.fd = -1;
		return !sl.out_dead;
	}
	if (n == 0) {
		struct stat st;

		/* the launcher closed its end: do not play on unattended */
		if (!fstat(sl.fd, &st) && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)))
			quit = true;
		sl.fd = -1;
	}
	for (i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			sl.line[sl.len] = 0;
			if (!sl.overflow)
				command(p, sl.line, &quit);
			sl.len = 0;
			sl.overflow = false;
		} else if (sl.len < (int)sizeof(sl.line) - 1) {
			sl.line[sl.len++] = buf[i];
		} else {
			sl.overflow = true;
		}
	}
	return !quit && !sl.out_dead;
}

double slave_tick(struct player *p, double now, bool force)
{
	int state = player_state(p);
	int volume = atomic_load(&p->volume);
	double pos;

	if (p->duration != sl.last_dur) {
		sl.last_dur = p->duration;
		out("dur %.1f", p->duration);
	}
	if (volume != sl.last_volume) {
		sl.last_volume = volume;
		out("volume %d", volume);
	}
	if (state != sl.last_state || force) {
		sl.last_state = state;
		out("state %s", state_name(state));
		sl.next_pos_at = 0;
	}
	if (now >= sl.next_pos_at) {
		bool moved;

		pos = player_pos(p);
		moved = fabs(pos - sl.last_pos) >= 0.05;
		if (moved) {
			sl.last_pos = pos;
			out("pos %.1f", pos);
		}
		sl.next_pos_at = now + 0.5;
		/* nothing moves while paused: no need to wake up for it */
		if (state != ST_PLAYING && !moved)
			return now + 3600;
	}
	return sl.next_pos_at;
}
