// SPDX-License-Identifier: MIT
/*
 * media_scan.c - elenco dei file di una cartella senza fermare l'interfaccia.
 *
 * Su una scheda SD lenta opendir/readdir/stat (e la lettura dei tag) possono
 * bloccare per secondi: li fa un processo figlio (fork senza exec: costa
 * poco, la shell occupa ~4 MB) che scrive una riga per file su una pipe.
 * Il padre la legge dal poll() del toolkit (bui_fd_add), in modo non
 * bloccante. Il figlio viene raccolto dal SIGCHLD di sys.c come ogni altro.
 *
 * Riga:  mtime \t dimensione \t percorso \t testo1 \t testo2 \n
 * (i file col nome che contiene tab o a-capo vengono saltati).
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "media.h"

#define MAX_DEPTH 8
#define LINE_MAX_LEN (4096 + 2 * MS_TEXT + 64)

/* ------------------------------------------------------------------ */
/* testi                                                              */
/* ------------------------------------------------------------------ */

void ms_utf8_trunc(char *s, int max_bytes)
{
	int n = (int)strlen(s);

	if (n <= max_bytes)
		return;
	n = max_bytes;
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
		n--;
	s[n] = 0;
}

void ms_clean(char *dst, int cap, const char *src, int len)
{
	int i, n = 0;

	if (len < 0)
		len = (int)strlen(src);
	for (i = 0; i < len && n < cap - 1; i++) {
		unsigned char ch = (unsigned char)src[i];

		if (ch < 0x20 || ch == 0x7F) {
			/* un solo spazio per ogni gruppo di caratteri di controllo */
			if (n && dst[n - 1] != ' ')
				dst[n++] = ' ';
		} else {
			dst[n++] = (char)ch;
		}
	}
	dst[n] = 0;
	if (i < len)	/* troncato: non lasciare mezzo carattere */
		while (n > 0 && ((unsigned char)dst[n - 1] & 0x80)) {
			int lead = ((unsigned char)dst[n - 1] & 0xC0) == 0xC0;

			dst[--n] = 0;
			if (lead)
				break;
		}
	while (n > 0 && dst[n - 1] == ' ')
		dst[--n] = 0;
}

int ms_natcmp(const char *a, const char *b)
{
	for (;;) {
		unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;

		if (isdigit(ca) && isdigit(cb)) {
			const char *ea, *eb;
			int la, lb, d;

			while (*a == '0')
				a++;
			while (*b == '0')
				b++;
			for (ea = a; isdigit((unsigned char)*ea); ea++)
				;
			for (eb = b; isdigit((unsigned char)*eb); eb++)
				;
			la = (int)(ea - a);
			lb = (int)(eb - b);
			if (la != lb)
				return la - lb;
			d = la ? memcmp(a, b, (size_t)la) : 0;
			if (d)
				return d;
			a = ea;
			b = eb;
			continue;
		}
		ca = (unsigned char)tolower(ca);
		cb = (unsigned char)tolower(cb);
		if (ca != cb || !ca)
			return (int)ca - (int)cb;
		a++;
		b++;
	}
}

void ms_fmt_date(char *buf, int len, time_t t)
{
	static const char *const mon[] = { "gen", "feb", "mar", "apr", "mag", "giu",
					   "lug", "ago", "set", "ott", "nov", "dic" };
	time_t now = time(NULL);
	struct tm tm, tn;

	if (!localtime_r(&t, &tm) || !localtime_r(&now, &tn)) {
		snprintf(buf, (size_t)len, "%s", "");
		return;
	}
	if (tm.tm_year == tn.tm_year && tm.tm_yday == tn.tm_yday)
		snprintf(buf, (size_t)len, "oggi %d:%02d", tm.tm_hour, tm.tm_min);
	else if (tm.tm_year == tn.tm_year)
		snprintf(buf, (size_t)len, "%d %s", tm.tm_mday, mon[tm.tm_mon]);
	else
		snprintf(buf, (size_t)len, "%d %s %d", tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900);
}

void ms_fmt_size(char *buf, int len, long long b)
{
	static const char *const unit[] = { "kB", "MB", "GB", "TB" };
	int u = 0;

	if (b < 1000) {
		snprintf(buf, (size_t)len, "%d B", (int)(b < 0 ? 0 : b));
		return;
	}
	/* decimi dell'unita', arrotondati */
	b = (b + 50) / 100;
	while (b >= 10000 && u < 3) {
		b = (b + 500) / 1000;
		u++;
	}
	if (u == 0 || b >= 100)
		snprintf(buf, (size_t)len, "%d %s", (int)((b + 5) / 10), unit[u]);
	else
		snprintf(buf, (size_t)len, "%d,%d %s", (int)(b / 10), (int)(b % 10), unit[u]);
}

/* ------------------------------------------------------------------ */
/* il figlio                                                          */
/* ------------------------------------------------------------------ */

void ms_child_reset(void)
{
	struct sigaction sa;
	sigset_t none;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
}

static int ext_ok(const char *name, const char *exts)
{
	const char *dot = strrchr(name, '.'), *e = exts;
	size_t n;

	if (!dot || dot == name || !dot[1])
		return 0;
	n = strlen(++dot);
	while (*e) {
		const char *sp = strchr(e, ' ');
		size_t l = sp ? (size_t)(sp - e) : strlen(e);

		if (l == n && !strncasecmp(e, dot, n))
			return 1;
		e += l;
		while (*e == ' ')
			e++;
	}
	return 0;
}

static void walk(FILE *o, const ms_lib *l, char *path, size_t plen, int depth, int *count)
{
	DIR *d = opendir(path);
	struct dirent *de;

	if (!d)
		return;
	while (*count < MS_MAX_ITEMS && (de = readdir(d))) {
		size_t nl = strlen(de->d_name);
		char s1[MS_TEXT], s2[MS_TEXT];
		struct stat st;

		if (de->d_name[0] == '.' || plen + 1 + nl >= 4096)
			continue;
		if (strpbrk(de->d_name, "\t\n\r"))
			continue;
		path[plen] = '/';
		memcpy(path + plen + 1, de->d_name, nl + 1);
		if (stat(path, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (l->recursive && depth < MAX_DEPTH)
				walk(o, l, path, plen + 1 + nl, depth + 1, count);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !ext_ok(de->d_name, l->exts))
			continue;
		s1[0] = s2[0] = 0;
		if (l->probe)
			l->probe(path, s1, s2, MS_TEXT);
		fprintf(o, "%lld\t%lld\t%s\t%s\t%s\n", (long long)st.st_mtime, (long long)st.st_size,
			path, s1, s2);
		/* poche voci per volta: l'elenco si riempie mentre la lettura continua */
		if ((++*count & 7) == 0)
			fflush(o);
	}
	closedir(d);
	path[plen] = 0;
}

static void child_main(const ms_lib *l, int fd)
{
	char path[4096];
	FILE *o = fdopen(fd, "w");
	int count = 0;

	if (!o)
		_exit(1);
	ms_dir(l, path, sizeof(path));
	walk(o, l, path, strlen(path), 0, &count);
	fflush(o);
	_exit(0);
}

/* ------------------------------------------------------------------ */
/* il padre                                                           */
/* ------------------------------------------------------------------ */

void ms_dir(const ms_lib *l, char *buf, int len)
{
	const char *home = getenv("HOME");

	snprintf(buf, (size_t)len, "%s/%s", home && *home ? home : "/root", l->subdir);
}

static void item_free(ms_item *it)
{
	free(it->path);
	free(it->s1);
	free(it->s2);
}

static void list_free(ms_item *it, int n)
{
	int i;

	for (i = 0; i < n; i++)
		item_free(&it[i]);
	free(it);
}

static int item_set(const ms_lib *l, ms_item *it, const char *path, time_t mtime, long long size,
		    const char *s1, const char *s2)
{
	char dir[4096];
	size_t dl;
	const char *sl;

	memset(it, 0, sizeof(*it));
	it->path = strdup(path);
	it->s1 = strdup(s1 ? s1 : "");
	it->s2 = strdup(s2 ? s2 : "");
	if (!it->path || !it->s1 || !it->s2) {
		item_free(it);
		return -1;
	}
	ms_dir(l, dir, sizeof(dir));
	dl = strlen(dir);
	it->rel = !strncmp(it->path, dir, dl) && it->path[dl] == '/' ? it->path + dl + 1 : it->path;
	sl = strrchr(it->path, '/');
	it->name = sl ? sl + 1 : it->path;
	it->mtime = mtime;
	it->size = size;
	return 0;
}

static int (*sort_cmp)(const ms_item *, const ms_item *);

static int sort_thunk(const void *a, const void *b)
{
	return sort_cmp(a, b);
}

static void list_sort(const ms_lib *l, ms_item *it, int n)
{
	if (!l->cmp || n < 2)
		return;
	sort_cmp = l->cmp;
	qsort(it, (size_t)n, sizeof(*it), sort_thunk);
}

static int list_add(ms_item **arr, int *n, int *cap, const ms_item *it)
{
	if (*n >= MS_MAX_ITEMS)
		return -1;
	if (*n == *cap) {
		int nc = *cap ? *cap * 2 : 64;
		ms_item *na = realloc(*arr, sizeof(ms_item) * (size_t)nc);

		if (!na)
			return -1;
		*arr = na;
		*cap = nc;
	}
	(*arr)[(*n)++] = *it;
	return 0;
}

static void parse_line(ms_lib *l, char *s)
{
	char *f[5];
	ms_item it;
	int i;

	for (i = 0; i < 5; i++) {
		f[i] = s;
		if (i < 4) {
			s = strchr(s, '\t');
			if (!s)
				return;
			*s++ = 0;
		}
	}
	if (item_set(l, &it, f[2], (time_t)atoll(f[0]), atoll(f[1]), f[3], f[4]) < 0)
		return;
	if ((l->loaded ? list_add(&l->st, &l->sn, &l->scap, &it) : list_add(&l->it, &l->n, &l->cap, &it)) < 0)
		item_free(&it);
}

static void scan_end(ms_lib *l)
{
	bui_fd_del(l->fd);
	close(l->fd);
	free(l->line);
	l->line = NULL;
	l->fd = -1;
	l->len = 0;
	l->scanning = 0;
}

static void scan_readable(int fd, void *user)
{
	ms_lib *l = user;
	int got = 0, eof = 0;

	for (;;) {
		ssize_t n = read(fd, l->line + l->len, LINE_MAX_LEN - 1 - (size_t)l->len);
		char *p, *nl;

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		if (n <= 0) {
			eof = 1;
			break;
		}
		l->len += (int)n;
		l->line[l->len] = 0;
		for (p = l->line; (nl = memchr(p, '\n', (size_t)(l->line + l->len - p))); p = nl + 1) {
			*nl = 0;
			parse_line(l, p);
			got = 1;
		}
		l->len -= (int)(p - l->line);
		memmove(l->line, p, (size_t)l->len);
		if (l->len >= LINE_MAX_LEN - 1)
			l->len = 0;	/* riga impossibile: scartata */
	}
	if (eof) {
		scan_end(l);
		if (l->loaded) {
			list_free(l->it, l->n);
			l->it = l->st;
			l->n = l->sn;
			l->cap = l->scap;
			l->st = NULL;
			l->sn = l->scap = 0;
		}
		l->loaded = 1;
		got = 1;
	}
	if (got && (eof || !l->loaded)) {
		list_sort(l, l->it, l->n);
		if (l->changed)
			l->changed(l);
	}
}

void ms_cancel(ms_lib *l)
{
	if (!l->scanning)
		return;
	/* il figlio muore da solo (SIGPIPE) alla prossima riga che scrive */
	scan_end(l);
	list_free(l->st, l->sn);
	l->st = NULL;
	l->sn = l->scap = 0;
}

void ms_scan(ms_lib *l)
{
	sigset_t all, old;
	int pfd[2];
	pid_t pid;

	ms_cancel(l);
	if (!l->loaded) {
		/* la prima lettura era rimasta a meta': si riparte da capo */
		list_free(l->it, l->n);
		l->it = NULL;
		l->n = l->cap = 0;
	}
	l->line = malloc(LINE_MAX_LEN);
	if (!l->line)
		return;
	if (pipe(pfd) < 0) {
		free(l->line);
		l->line = NULL;
		return;
	}
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	/* nessun segnale fra fork() e il ripristino dei gestori nel figlio: i
	 * gestori della shell scrivono su una pipe che il figlio condivide */
	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, &old);
	pid = fork();
	if (pid == 0) {
		ms_child_reset();
		(void)nice(15);	/* la scansione e' lavoro di fondo */
		close(pfd[0]);
		child_main(l, pfd[1]);
	}
	sigprocmask(SIG_SETMASK, &old, NULL);
	close(pfd[1]);
	if (pid < 0 || bui_fd_add(pfd[0], scan_readable, l) < 0) {
		close(pfd[0]);
		free(l->line);
		l->line = NULL;
		return;
	}
	l->fd = pfd[0];
	l->len = 0;
	l->scanning = 1;
}

void ms_free(ms_lib *l)
{
	ms_cancel(l);
	list_free(l->it, l->n);
	l->it = NULL;
	l->n = l->cap = 0;
	l->loaded = 0;
}

int ms_find(const ms_lib *l, const char *path)
{
	int i;

	if (!path || !*path)
		return -1;
	for (i = 0; i < l->n; i++)
		if (!strcmp(l->it[i].path, path))
			return i;
	return -1;
}

int ms_upsert(ms_lib *l, const char *path, time_t mtime, long long size, const char *s1,
	      const char *s2)
{
	int i = ms_find(l, path);
	ms_item it;

	if (item_set(l, &it, path, mtime, size, s1, s2) < 0)
		return -1;
	if (i >= 0) {
		item_free(&l->it[i]);
		l->it[i] = it;
	} else if (list_add(&l->it, &l->n, &l->cap, &it) < 0) {
		item_free(&it);
		return -1;
	}
	list_sort(l, l->it, l->n);
	return ms_find(l, path);
}

void ms_remove(ms_lib *l, const char *path)
{
	int i = ms_find(l, path);

	if (i < 0)
		return;
	item_free(&l->it[i]);
	memmove(&l->it[i], &l->it[i + 1], sizeof(ms_item) * (size_t)(l->n - i - 1));
	l->n--;
}

int ms_cmp_path(const ms_item *a, const ms_item *b)
{
	int da = (int)(a->name - a->rel), db = (int)(b->name - b->rel), d;
	char fa[512], fb[512];

	/* prima i file della radice, poi le cartelle in ordine */
	snprintf(fa, sizeof(fa), "%.*s", da, a->rel);
	snprintf(fb, sizeof(fb), "%.*s", db, b->rel);
	d = ms_natcmp(fa, fb);
	if (!d)
		d = ms_natcmp(a->name, b->name);
	return d ? d : strcmp(a->path, b->path);
}

void ms_title(const ms_item *it, char *buf, int cap)
{
	const char *dot = strrchr(it->name, '.');
	int n = dot && dot > it->name ? (int)(dot - it->name) : (int)strlen(it->name);

	ms_clean(buf, cap, it->name, n);
}

void ms_folder(const ms_item *it, char *buf, int cap)
{
	int n = (int)(it->name - it->rel);

	ms_clean(buf, cap, it->rel, n > 0 ? n - 1 : 0);
}
