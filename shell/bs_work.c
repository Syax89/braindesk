// SPDX-License-Identifier: MIT
/*
 * bs_work.c - filo di lavoro unico con coda (vedi bs_work.h).
 *
 * Il filo nasce al primo lavoro e poi dorme su una condizione: a riposo non
 * consuma nulla. Tutti i segnali sono bloccati nel filo, cosi' SIGCHLD e
 * SIGTERM continuano ad arrivare al filo principale (sys.c).
 */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "bui.h"
#include "bs_work.h"

struct job {
	struct job *next;
	int id;
	volatile int cancel;
	void (*run)(void *, const volatile int *);
	void (*done)(void *, int);
	void *arg;
};

static struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	pthread_t th;
	int started, next_id;
	int pipe[2];
	struct job *todo, *todo_tail;	/* in attesa */
	struct job *running;
	struct job *fin, *fin_tail;	/* finiti: la callback done e' da chiamare */
} W = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .pipe = { -1, -1 } };

static void fin_push(struct job *j)
{
	j->next = NULL;
	if (W.fin_tail)
		W.fin_tail->next = j;
	else
		W.fin = j;
	W.fin_tail = j;
}

static void wake_main(void)
{
	char c = 1;

	while (write(W.pipe[1], &c, 1) < 0 && errno == EINTR)
		;
}

static void *worker(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&W.mu);
	for (;;) {
		struct job *j;

		while (!W.todo)
			pthread_cond_wait(&W.cv, &W.mu);
		j = W.todo;
		W.todo = j->next;
		if (!W.todo)
			W.todo_tail = NULL;
		W.running = j;
		pthread_mutex_unlock(&W.mu);
		if (!j->cancel)
			j->run(j->arg, &j->cancel);
		pthread_mutex_lock(&W.mu);
		W.running = NULL;
		fin_push(j);
		wake_main();
	}
	return NULL;
}

/* filo principale: consegna i risultati */
static void on_pipe(int fd, void *user)
{
	char buf[64];

	(void)user;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	for (;;) {
		struct job *j;

		pthread_mutex_lock(&W.mu);
		j = W.fin;
		if (j) {
			W.fin = j->next;
			if (!W.fin)
				W.fin_tail = NULL;
		}
		pthread_mutex_unlock(&W.mu);
		if (!j)
			break;
		j->done(j->arg, j->cancel);
		free(j);
	}
}

static int start(void)
{
	pthread_attr_t at;
	sigset_t all, old;
	int rc, i;

	if (W.started)
		return 0;
	if (W.pipe[0] < 0) {
		if (pipe(W.pipe) < 0)
			return -1;
		for (i = 0; i < 2; i++) {
			fcntl(W.pipe[i], F_SETFD, FD_CLOEXEC);
			fcntl(W.pipe[i], F_SETFL, O_NONBLOCK);
		}
		if (bui_fd_add(W.pipe[0], on_pipe, NULL) < 0) {
			close(W.pipe[0]);
			close(W.pipe[1]);
			W.pipe[0] = W.pipe[1] = -1;
			return -1;
		}
	}
	/* i buffer grandi delle decodifiche (centinaia di kB) tornano subito al
	 * sistema invece di restare nell'heap: soglia di mmap fissa, non adattiva */
	mallopt(M_MMAP_THRESHOLD, 128 * 1024);
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, &old);
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 256 * 1024);
	rc = pthread_create(&W.th, &at, worker, NULL);
	pthread_attr_destroy(&at);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	if (rc)
		return -1;
	W.started = 1;
	return 0;
}

int bs_work_submit(void (*run)(void *, const volatile int *), void (*done)(void *, int), void *arg,
		   int front)
{
	struct job *j;

	if (start() < 0)
		return 0;
	j = calloc(1, sizeof(*j));
	if (!j)
		return 0;
	j->run = run;
	j->done = done;
	j->arg = arg;
	pthread_mutex_lock(&W.mu);
	if (++W.next_id <= 0)
		W.next_id = 1;
	j->id = W.next_id;
	if (front && W.todo) {
		j->next = W.todo;
		W.todo = j;
	} else {
		if (W.todo_tail)
			W.todo_tail->next = j;
		else
			W.todo = j;
		W.todo_tail = j;
	}
	pthread_cond_signal(&W.cv);
	pthread_mutex_unlock(&W.mu);
	return j->id;
}

void bs_work_cancel(int id)
{
	struct job *j, *prev = NULL;

	if (id <= 0)
		return;
	pthread_mutex_lock(&W.mu);
	if (W.running && W.running->id == id) {
		W.running->cancel = 1;
	} else {
		for (j = W.todo; j && j->id != id; j = j->next)
			prev = j;
		if (j) {
			/* ancora in coda: non partira' mai, passa subito fra i finiti */
			if (prev)
				prev->next = j->next;
			else
				W.todo = j->next;
			if (W.todo_tail == j)
				W.todo_tail = prev;
			j->cancel = 1;
			fin_push(j);
			wake_main();
		}
	}
	pthread_mutex_unlock(&W.mu);
}
