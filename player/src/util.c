// SPDX-License-Identifier: MIT
#include "brainplay.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int g_loglevel = LOG_INFO;
int g_wake_fd = -1;

void logmsg(int level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	if (level > g_loglevel)
		return;
	n = snprintf(buf, sizeof(buf), "brainplay: ");
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = sizeof(buf) - 2;
	buf[n++] = '\n';
	/* one write per line: lines of different threads do not mix */
	if (write(STDERR_FILENO, buf, n) < 0) {
	}
}

double now_sec(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

void sleep_sec(double s)
{
	struct timespec ts;

	if (s <= 0)
		return;
	ts.tv_sec = (time_t)s;
	ts.tv_nsec = (long)((s - ts.tv_sec) * 1e9);
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

void deadline_after(struct timespec *ts, double s)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	if (s < 0)
		s = 0;
	ts->tv_sec += (time_t)s;
	ts->tv_nsec += (long)((s - (time_t)s) * 1e9);
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_nsec -= 1000000000L;
		ts->tv_sec++;
	}
}

void cond_init_monotonic(pthread_cond_t *c)
{
	pthread_condattr_t a;

	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(c, &a);
	pthread_condattr_destroy(&a);
}

const char *fmt_time(char *buf, double sec, bool hours)
{
	int s = sec > 0 ? (int)sec : 0;

	if (hours || s >= 3600)
		snprintf(buf, 16, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, 16, "%d:%02d", s / 60, s % 60);
	return buf;
}

void wake_main(void)
{
	uint64_t one = 1;
	int saved = errno;

	if (g_wake_fd >= 0 && write(g_wake_fd, &one, sizeof(one)) < 0) {
	}
	errno = saved;
}
