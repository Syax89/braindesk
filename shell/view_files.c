// SPDX-License-Identifier: MIT
/*
 * view_files.c - Esplora: il gestore di file integrato.
 *
 * Pagine: l'elenco della cartella (con le scorciatoie: colonna a sinistra in
 * orizzontale, striscia in alto in verticale), il foglio delle azioni di un
 * elemento, le proprieta', la rinomina, il lettore di testi e il
 * visualizzatore di immagini della Galleria.
 *
 * La cartella viene letta dal filo di lavoro (bs_work): readdir + stat di
 * ogni voce e ordinamento avvengono li', anche su una SD lenta l'interfaccia
 * resta viva. Finche' la risposta non arriva resta visibile l'elenco
 * precedente; dopo 200 ms compare "Caricamento...".
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bs_img.h"
#include "bs_work.h"
#include "view_files.h"
#include "view_gallery.h"

#define MAX_ENTRIES 4000
#define MAX_PLACES 10
#define ROW_H 64
#define SIDE_W 208
#define MORE_W 56
#define LONG_PRESS_MS 550
#define PATH_LEN 1024

enum { M_BROWSE, M_ACTIONS, M_PROPS, M_RENAME, M_TEXT, M_IMAGE };
enum { K_DIR, K_IMAGE, K_TEXT, K_VIDEO, K_AUDIO, K_NOEXT, K_OTHER };

struct entry {
	char *name;
	long long size, mtime;
	unsigned mode;
	uint8_t kind, is_link;
};

struct place {
	char label[24];
	char path[512];
	const bui_icon *icon;
	int make;		/* cartella standard: creata al primo uso */
};

struct scan {
	char path[PATH_LEN];
	int hidden, gen, keep;
	struct entry *e;
	int n, truncated, err;
};

struct rmjob {
	char path[PATH_LEN];
	int gen, rc;
};

static struct {
	bui_panel head, body, page;
	bui_list list, side;
	bui_widget *list_w, *side_w, *title_w, *sub_w;
	bui_rect area;
	int mode, wide, kbd;
	int cap_head;

	char cwd[PATH_LEN], home[512];
	struct entry *e;
	int n, err;
	int show_hidden;
	int loading, load_job, load_timer, gen;
	char select_name[256];

	struct place place[MAX_PLACES];
	int nplaces, cur_place;

	char title[256], sub[PATH_LEN];
	const char *empty;

	/* tocco sull'elenco */
	int press_more, from_pointer, lp_timer, lp_swallow;

	/* elemento su cui si sta agendo */
	int act;
	char act_name[256];
	char p_type[48], p_size[48], p_date[48], p_perm[32], p_where[PATH_LEN];
	char edit[256];
	bui_widget *edit_w;

	/* immagini della cartella, per il visualizzatore */
	int *img;
	int nimg;
	int text_pending;
} F;

static bs_view *self;

static void relayout(void);
static void navigate(const char *path, const char *select, int keep);
static void set_mode(int mode);

/* ------------------------------------------------------------------ */
/* nomi, tipi, testi                                                  */
/* ------------------------------------------------------------------ */

/* copia troncando (i nomi troppo lunghi si accorciano solo a schermo) */
static void cpy(char *buf, size_t len, const char *s)
{
	size_t n = strlen(s);

	if (n >= len)
		n = len - 1;
	memcpy(buf, s, n);
	buf[n] = 0;
}

/* dir + "/" + name; -1 (e stringa vuota) se non entra: mai percorsi troncati */
static int join(char *buf, size_t len, const char *dir, const char *name)
{
	size_t dl = strlen(dir), nl = strlen(name);
	int slash = !(dl == 1 && dir[0] == '/');

	if (dl + (size_t)slash + nl >= len) {
		buf[0] = 0;
		return -1;
	}
	memcpy(buf, dir, dl);
	if (slash)
		buf[dl++] = '/';
	memcpy(buf + dl, name, nl + 1);
	return 0;
}

static int kind_of(const char *name)
{
	static const char *const img[] = { "jpg", "jpeg", "png", "bmp", "gif", NULL };
	static const char *const txt[] = { "txt", "md", "log", "conf", "sh", "c", "h", "py", "json",
					    "xml", "csv", "ini", "cfg", NULL };
	static const char *const vid[] = { "mp4", "mkv", "avi", "webm", "mov", "mpg", "mpeg", "ts",
					    "flv", NULL };
	static const char *const aud[] = { "mp3", "flac", "ogg", "opus", "wav", "m4a", "aac", NULL };
	static const char *const *const tab[] = { img, txt, vid, aud };
	const char *e = strrchr(name, '.');
	int k, i;

	if (!e || e == name || !e[1])
		return K_NOEXT;
	for (k = 0; k < 4; k++)
		for (i = 0; tab[k][i]; i++)
			if (!strcasecmp(e + 1, tab[k][i]))
				return K_IMAGE + k;
	return K_OTHER;
}

static const bui_icon *kind_icon(int kind)
{
	switch (kind) {
	case K_DIR:
		return BUI_IC(folder, 24);
	case K_IMAGE:
		return BUI_IC(image, 24);
	case K_TEXT:
		return BUI_IC(doc, 24);
	case K_VIDEO:
		return BUI_IC(video, 24);
	case K_AUDIO:
		return BUI_IC(audio, 24);
	default:
		return BUI_IC(file, 24);
	}
}

static const char *kind_name(int kind)
{
	static const char *const name[] = { "Cartella", "Immagine", "Testo", "Video", "Audio", "File",
					     "File" };

	return name[kind];
}

static void fmt_date(char *buf, size_t len, long long t)
{
	static const char *const mon[] = { "gen", "feb", "mar", "apr", "mag", "giu",
					    "lug", "ago", "set", "ott", "nov", "dic" };
	time_t tt = (time_t)t;
	struct tm tm;

	localtime_r(&tt, &tm);
	snprintf(buf, len, "%d %s %d, %02d:%02d", tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900,
		 tm.tm_hour, tm.tm_min);
}

/* titolo (nome della cartella) e sottotitolo (percorso, con ~ per la home) */
static void set_titles(void)
{
	size_t hl = strlen(F.home);
	const char *b;

	switch (F.mode) {
	case M_BROWSE:
		b = strrchr(F.cwd, '/');
		if (!strcmp(F.cwd, F.home))
			snprintf(F.title, sizeof(F.title), "Home");
		else
			cpy(F.title, sizeof(F.title), b && b[1] ? b + 1 : F.cwd);
		if (hl > 1 && !strncmp(F.cwd, F.home, hl) && (!F.cwd[hl] || F.cwd[hl] == '/'))
			snprintf(F.sub, sizeof(F.sub), "~%s", F.cwd + hl);
		else
			cpy(F.sub, sizeof(F.sub), F.cwd);
		break;
	case M_ACTIONS:
		cpy(F.title, sizeof(F.title), F.act_name);
		if (F.e[F.act].kind == K_DIR)
			snprintf(F.sub, sizeof(F.sub), "Cartella");
		else
			snprintf(F.sub, sizeof(F.sub), "%s  \xC2\xB7  %s", T(kind_name(F.e[F.act].kind)), F.p_size);
		break;
	case M_PROPS:
		cpy(F.title, sizeof(F.title), F.act_name);
		snprintf(F.sub, sizeof(F.sub), "Propriet\xC3\xA0");
		break;
	case M_RENAME:
		snprintf(F.title, sizeof(F.title), "Rinomina");
		cpy(F.sub, sizeof(F.sub), F.act_name);
		break;
	case M_TEXT:
		cpy(F.title, sizeof(F.title), F.act_name);
		bs_text_info(F.sub, sizeof(F.sub));
		break;
	default:
		break;
	}
	if (F.title_w)
		bui_widget_damage(F.title_w);
	if (F.sub_w)
		bui_widget_damage(F.sub_w);
}

/* ------------------------------------------------------------------ */
/* scorciatoie                                                        */
/* ------------------------------------------------------------------ */

static void place_add(const char *label, const char *path, const bui_icon *icon, int make)
{
	struct place *p;

	if (F.nplaces >= MAX_PLACES)
		return;
	p = &F.place[F.nplaces++];
	cpy(p->label, sizeof(p->label), label);
	cpy(p->path, sizeof(p->path), path);
	p->icon = icon;
	p->make = make;
}

/* supporti montati: /proc/mounts e' un file del kernel, leggerlo non aspetta nessun disco */
static void places_mounts(void)
{
	const char *root = getenv("BRAIN_SYSROOT");
	char file[600], line[1024], dev[256], mnt[512], *p, *q;
	FILE *f;
	int found = 0;

	snprintf(file, sizeof(file), "%s/proc/mounts", root ? root : "");
	f = fopen(file, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f) && found < 4) {
		const char *b;

		if (sscanf(line, "%255s %511s", dev, mnt) != 2)
			continue;
		if (strncmp(mnt, "/media/", 7) && strncmp(mnt, "/mnt/", 5) && strncmp(mnt, "/run/media/", 11))
			continue;
		/* "\040" e' lo spazio nei nomi dei punti di montaggio */
		for (p = q = mnt; *p; p++) {
			if (p[0] == '\\' && p[1] == '0' && p[2] == '4' && p[3] == '0') {
				*q++ = ' ';
				p += 3;
			} else {
				*q++ = *p;
			}
		}
		*q = 0;
		b = strrchr(mnt, '/');
		place_add(b && b[1] ? b + 1 : mnt, mnt,
			  strstr(dev, "mmcblk") ? BUI_IC(sdcard, 24) : BUI_IC(usb, 24), 0);
		found++;
	}
	fclose(f);
}

static void places_build(void)
{
	static const struct {
		const char *name;
		const bui_icon *icon;
	} std[] = { { "Documenti", BUI_IC(doc, 24) }, { "Immagini", BUI_IC(image, 24) },
		    { "Musica", BUI_IC(music, 24) },  { "Video", BUI_IC(video, 24) },
		    { "Note", BUI_IC(note, 24) } };
	char path[512];
	unsigned i;

	F.nplaces = 0;
	place_add("Home", F.home, BUI_IC(home, 24), 0);
	for (i = 0; i < sizeof(std) / sizeof(std[0]); i++) {
		join(path, sizeof(path), F.home, std[i].name);
		place_add(std[i].name, path, std[i].icon, 1);
	}
	places_mounts();
}

/* la scorciatoia che contiene la cartella corrente (il percorso piu' lungo) */
static int place_current(void)
{
	int i, best = -1;
	size_t bl = 0;

	for (i = 0; i < F.nplaces; i++) {
		size_t l = strlen(F.place[i].path);

		if (!strncmp(F.cwd, F.place[i].path, l) && (!F.cwd[l] || F.cwd[l] == '/') && l > bl) {
			best = i;
			bl = l;
		}
	}
	return best;
}

/* ------------------------------------------------------------------ */
/* lettura della cartella (filo di lavoro)                            */
/* ------------------------------------------------------------------ */

static void entries_free(struct entry *e, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free(e[i].name);
	free(e);
}

static int entry_cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	int dx = x->kind == K_DIR, dy = y->kind == K_DIR, c;

	if (dx != dy)
		return dy - dx;		/* prima le cartelle */
	c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->name, y->name);
}

static void scan_run(void *arg, const volatile int *cancel)
{
	struct scan *s = arg;
	struct dirent *de;
	struct stat st;
	DIR *d = opendir(s->path);
	int cap = 0;

	if (!d) {
		s->err = errno ? errno : EIO;
		return;
	}
	while ((de = readdir(d)) != NULL && !*cancel) {
		struct entry *e;

		if (de->d_name[0] == '.' &&
		    (!s->hidden || !de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2])))
			continue;
		if (s->n >= MAX_ENTRIES) {
			s->truncated = 1;
			break;
		}
		if (s->n == cap) {
			int nc = cap ? cap * 2 : 128;
			struct entry *ne = realloc(s->e, sizeof(*ne) * (size_t)nc);

			if (!ne)
				break;
			s->e = ne;
			cap = nc;
		}
		e = &s->e[s->n];
		memset(e, 0, sizeof(*e));
		e->name = strdup(de->d_name);
		if (!e->name)
			break;
		e->is_link = de->d_type == DT_LNK;
		e->kind = K_OTHER;
		if (!fstatat(dirfd(d), de->d_name, &st, 0) ||
		    !fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
			e->size = (long long)st.st_size;
			e->mtime = (long long)st.st_mtime;
			e->mode = (unsigned)st.st_mode;
			if (S_ISDIR(st.st_mode))
				e->kind = K_DIR;
			else if (S_ISREG(st.st_mode))
				e->kind = (uint8_t)kind_of(e->name);
		}
		s->n++;
	}
	closedir(d);
	if (s->n > 1 && !*cancel)
		qsort(s->e, (size_t)s->n, sizeof(s->e[0]), entry_cmp);
}

static void load_timeout(void *user)
{
	(void)user;
	F.load_timer = 0;
	if (!F.loading)
		return;
	/* la SD e' lenta: meglio dirlo che lasciare l'elenco vecchio */
	entries_free(F.e, F.n);
	F.e = NULL;
	F.n = 0;
	F.list.empty_text = "Caricamento\xE2\x80\xA6";
	F.list.sc.pos = 0;
	bui_list_set_count(&F.list, 0);
}

static void scan_done(void *arg, int cancelled)
{
	struct scan *s = arg;
	int i, sel = -1;

	if (cancelled || s->gen != F.gen || !bs_view_is_active(self)) {
		entries_free(s->e, s->n);
		free(s);
		return;
	}
	F.load_job = 0;
	F.loading = 0;
	if (F.load_timer)
		bui_timer_del(F.load_timer);
	F.load_timer = 0;
	entries_free(F.e, F.n);
	F.e = s->e;
	F.n = s->n;
	F.err = s->err;
	F.list.empty_text = s->err == ENOENT ? "La cartella non esiste pi\xC3\xB9"
			  : s->err ? "Cartella non accessibile" : "Cartella vuota";
	for (i = 0; i < F.n && F.select_name[0]; i++)
		if (!strcmp(F.e[i].name, F.select_name))
			sel = i;
	F.select_name[0] = 0;
	F.list.sc.fling = 0;
	if (!s->keep)
		F.list.sc.pos = 0;
	F.list.sel = sel >= 0 ? sel : s->keep && F.list.sel < F.n ? F.list.sel : F.n ? 0 : -1;
	bui_list_set_count(&F.list, F.n);
	if (sel >= 0)
		bui_list_reveal(&F.list, sel);
	if (s->truncated)
		bui_toast("Cartella molto grande: mostrati i primi 4000 elementi", 3500);
	free(s);
}

static void navigate(const char *path, const char *select, int keep)
{
	struct scan *s = calloc(1, sizeof(*s));
	char tmp[PATH_LEN];
	int cp;

	if (!s)
		return;
	cpy(tmp, sizeof(tmp), path);		/* path puo' essere F.cwd */
	cpy(F.cwd, sizeof(F.cwd), tmp);
	cpy(F.select_name, sizeof(F.select_name), select ? select : "");
	bs_work_cancel(F.load_job);
	F.gen++;
	cpy(s->path, sizeof(s->path), F.cwd);
	s->hidden = F.show_hidden;
	s->gen = F.gen;
	s->keep = keep;
	F.load_job = bs_work_submit(scan_run, scan_done, s, 1);
	if (!F.load_job) {
		free(s);
		return;
	}
	F.loading = 1;
	if (!F.load_timer)
		F.load_timer = bui_timer_add(200, 0, load_timeout, NULL);
	set_titles();
	cp = place_current();
	if (cp != F.cur_place) {
		F.cur_place = cp;
		if (F.mode == M_BROWSE)
			relayout();	/* la scorciatoia evidenziata cambia */
	}
}

static void go_up(void)
{
	char parent[PATH_LEN], name[256];
	char *b;

	if (!strcmp(F.cwd, "/"))
		return;
	cpy(parent, sizeof(parent), F.cwd);
	b = strrchr(parent, '/');
	cpy(name, sizeof(name), b ? b + 1 : "");
	if (b == parent)
		parent[1] = 0;
	else if (b)
		*b = 0;
	navigate(parent, name, 0);	/* la cartella da cui si esce resta selezionata */
}

/* ------------------------------------------------------------------ */
/* apertura per tipo                                                  */
/* ------------------------------------------------------------------ */

static int src_count(void *user)
{
	(void)user;
	return F.nimg;
}

static const char *src_path(int i, void *user)
{
	static char path[PATH_LEN];

	(void)user;
	if (i < 0 || i >= F.nimg)
		return NULL;
	join(path, sizeof(path), F.cwd, F.e[F.img[i]].name);
	return path;
}

static void src_removed(int index, void *user)
{
	int ei, i;

	(void)user;
	if (index < 0 || index >= F.nimg)
		return;
	ei = F.img[index];
	free(F.e[ei].name);
	memmove(&F.e[ei], &F.e[ei + 1], sizeof(F.e[0]) * (size_t)(F.n - ei - 1));
	F.n--;
	memmove(&F.img[index], &F.img[index + 1], sizeof(int) * (size_t)(F.nimg - index - 1));
	F.nimg--;
	for (i = index; i < F.nimg; i++)
		F.img[i]--;
	F.list.count = F.n;
	if (F.list.sel >= F.n)
		F.list.sel = F.n - 1;
}

static void src_closed(int index, void *user)
{
	(void)user;
	if (index >= 0 && index < F.nimg)
		F.list.sel = F.img[index];
	free(F.img);
	F.img = NULL;
	F.nimg = 0;
	set_mode(M_BROWSE);
	if (F.list.sel >= 0)
		bui_list_reveal(&F.list, F.list.sel);
}

static void open_image(int idx)
{
	static const bs_viewer_src src = { src_count, src_path, src_removed, src_closed, NULL };
	int i, at = 0;

	free(F.img);
	F.img = malloc(sizeof(int) * (size_t)F.n);
	if (!F.img)
		return;
	F.nimg = 0;
	for (i = 0; i < F.n; i++)
		if (F.e[i].kind == K_IMAGE) {
			if (i == idx)
				at = F.nimg;
			F.img[F.nimg++] = i;
		}
	F.mode = M_IMAGE;
	bui_panel_clear(&F.head);
	bui_panel_clear(&F.body);
	bui_panel_clear(&F.page);
	F.title_w = F.sub_w = F.list_w = F.side_w = NULL;
	bs_viewer_open(self->win, F.area, &src, at);
}

static bui_rect content_rect(void)
{
	return bui_r(F.area.x, F.area.y + BUI_HEADER_H, F.area.w, F.area.h - BUI_HEADER_H);
}

static void text_done(int result, void *user)
{
	(void)user;
	F.text_pending = 0;
	if (!bs_view_is_active(self))
		return;
	if (result == BS_TEXT_OK) {
		if (F.mode != M_TEXT && F.mode != M_BROWSE)
			return;
		bs_text_show(self->win, content_rect());
		set_mode(M_TEXT);
		return;
	}
	if (F.mode == M_TEXT)
		set_mode(M_BROWSE);
	else
		set_titles();
	bui_toast(result == BS_TEXT_E_BINARY ? "Tipo di file non supportato"
		  : result == BS_TEXT_E_OPEN ? "Impossibile leggere il file" : "Memoria insufficiente",
		  2500);
}

static void open_text(int idx, int sniff)
{
	char path[PATH_LEN];

	join(path, sizeof(path), F.cwd, F.e[idx].name);
	cpy(F.act_name, sizeof(F.act_name), F.e[idx].name);
	F.text_pending = 1;
	snprintf(F.sub, sizeof(F.sub), T("Apertura di %s\xE2\x80\xA6"), F.act_name);
	if (F.sub_w)
		bui_widget_damage(F.sub_w);
	bs_text_load(path, bs_text_width_for(F.area.w), sniff, text_done, NULL);
}

static void open_media(int idx)
{
	const char *player = getenv("BRAIN_PLAYER");
	char path[PATH_LEN], msg[96];
	const char *argv[3];

	join(path, sizeof(path), F.cwd, F.e[idx].name);
	argv[0] = player && *player ? player : "brainplay";
	argv[1] = path;
	argv[2] = NULL;
	if (bs_launch(argv, "il lettore multimediale") == 0) {
		snprintf(msg, sizeof(msg), T("Apertura di %.60s\xE2\x80\xA6"), F.e[idx].name);
		bui_toast(msg, 1800);
	}
}

static void open_entry(int idx)
{
	char path[PATH_LEN];

	if (idx < 0 || idx >= F.n || F.loading)
		return;
	F.list.sel = idx;
	switch (F.e[idx].kind) {
	case K_DIR:
		if (join(path, sizeof(path), F.cwd, F.e[idx].name) < 0)
			bui_toast("Percorso troppo lungo", 2200);
		else
			navigate(path, NULL, 0);
		break;
	case K_IMAGE:
		open_image(idx);
		break;
	case K_TEXT:
		open_text(idx, 0);
		break;
	case K_NOEXT:
		open_text(idx, 1);	/* senza estensione: si apre solo se sembra testo */
		break;
	case K_VIDEO:
	case K_AUDIO:
		open_media(idx);
		break;
	default:
		bui_toast("Tipo di file non supportato", 2200);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* azioni: elimina, rinomina, proprieta'                              */
/* ------------------------------------------------------------------ */

static int rm_tree(int dfd, const char *name, int depth, const volatile int *cancel)
{
	struct dirent *de;
	DIR *d;
	int fd, rc = 0;

	if (!unlinkat(dfd, name, 0))
		return 0;
	if ((errno != EISDIR && errno != EPERM) || depth > 64 || *cancel)
		return -1;
	fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	d = fdopendir(fd);
	if (!d) {
		close(fd);
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (rm_tree(fd, de->d_name, depth + 1, cancel) < 0)
			rc = -1;
	}
	closedir(d);
	if (unlinkat(dfd, name, AT_REMOVEDIR) < 0)
		rc = -1;
	return rc;
}

static void rm_run(void *arg, const volatile int *cancel)
{
	struct rmjob *j = arg;

	j->rc = rm_tree(AT_FDCWD, j->path, 0, cancel);
}

static void rm_done(void *arg, int cancelled)
{
	struct rmjob *j = arg;

	(void)cancelled;
	bui_toast(j->rc ? "Impossibile eliminare tutto il contenuto" : "Elemento eliminato", 2200);
	if (bs_view_is_active(self) && j->gen == F.gen && F.mode == M_BROWSE)
		navigate(F.cwd, NULL, 1);
	free(j);
}

static void delete_confirmed(int button, void *user)
{
	struct rmjob *j;
	int gen = (int)(intptr_t)user;

	if (button != 1 || !bs_view_is_active(self) || gen != F.gen)
		return;
	j = calloc(1, sizeof(*j));
	if (!j)
		return;
	join(j->path, sizeof(j->path), F.cwd, F.act_name);
	if (F.mode != M_BROWSE)
		set_mode(M_BROWSE);
	j->gen = F.gen;
	if (!bs_work_submit(rm_run, rm_done, j, 1))
		free(j);
}

static void ask_delete(int idx)
{
	static char msg[320];
	bui_dialog d = {
		.message = msg,
		.buttons = { "Annulla", "Elimina" },
		.styles = { BUI_BS_TONAL, BUI_BS_DANGER },
		.cancel = 0,
		.done = delete_confirmed,
		.user = (void *)(intptr_t)F.gen,
	};

	if (idx < 0 || idx >= F.n)
		return;
	F.act = idx;
	cpy(F.act_name, sizeof(F.act_name), F.e[idx].name);
	if (F.e[idx].kind == K_DIR && !F.e[idx].is_link) {
		d.title = "Eliminare la cartella?";
		snprintf(msg, sizeof(msg),
			 T("\xC2\xAB%.120s\xC2\xBB e tutto il suo contenuto verranno eliminati definitivamente."),
			 F.act_name);
	} else {
		d.title = "Eliminare il file?";
		snprintf(msg, sizeof(msg), T("\xC2\xAB%.120s\xC2\xBB verr\xC3\xA0 eliminato definitivamente."),
			 F.act_name);
	}
	bui_dialog_open(&d);
}

static void props_fill(int idx)
{
	const struct entry *e = &F.e[idx];
	static const char rwx[] = "rwxrwxrwx";
	char perm[10];
	int i;

	snprintf(F.p_type, sizeof(F.p_type), "%s%s", e->is_link ? "Collegamento a " : "",
		 e->is_link ? (e->kind == K_DIR ? "cartella" : "file") : kind_name(e->kind));
	bs_fmt_size(F.p_size, sizeof(F.p_size), e->size);
	fmt_date(F.p_date, sizeof(F.p_date), e->mtime);
	for (i = 0; i < 9; i++)
		perm[i] = (e->mode & (0400u >> i)) ? rwx[i] : '-';
	perm[9] = 0;
	snprintf(F.p_perm, sizeof(F.p_perm), "%s (%03o)", perm, e->mode & 0777);
	cpy(F.p_where, sizeof(F.p_where), F.cwd);
}

static void open_actions(int idx)
{
	if (idx < 0 || idx >= F.n || F.loading)
		return;
	F.act = idx;
	F.list.sel = idx;
	cpy(F.act_name, sizeof(F.act_name), F.e[idx].name);
	props_fill(idx);
	set_mode(M_ACTIONS);
}

static void rename_apply(void)
{
	char from[PATH_LEN], to[PATH_LEN];
	struct stat st;

	if (!F.edit[0] || strchr(F.edit, '/') || !strcmp(F.edit, ".") || !strcmp(F.edit, "..")) {
		bui_toast("Nome non valido", 2000);
		return;
	}
	if (!strcmp(F.edit, F.act_name)) {
		set_mode(M_BROWSE);
		return;
	}
	join(from, sizeof(from), F.cwd, F.act_name);
	join(to, sizeof(to), F.cwd, F.edit);
	if (!lstat(to, &st)) {
		bui_toast("Esiste gi\xC3\xA0 un elemento con questo nome", 2500);
		return;
	}
	if (rename(from, to) < 0) {
		bui_toast("Impossibile rinominare", 2500);
		return;
	}
	set_mode(M_BROWSE);
	navigate(F.cwd, F.edit, 1);
}

static void edit_key(int key)
{
	size_t n = strlen(F.edit);

	if (key == BUI_K_BACKSPACE) {
		if (!n)
			return;
		/* toglie un carattere intero (UTF-8) */
		do
			n--;
		while (n > 0 && ((unsigned char)F.edit[n] & 0xC0) == 0x80);
		F.edit[n] = 0;
	} else if (key >= 0x20 && key < 0x100 && key != '/' && key != 0x7F) {
		if (n + 3 > 200)
			return;
		if (key < 0x80) {
			F.edit[n++] = (char)key;
		} else {	/* la tastiera arriva in Latin-1 */
			F.edit[n++] = (char)(0xC0 | (key >> 6));
			F.edit[n++] = (char)(0x80 | (key & 0x3F));
		}
		F.edit[n] = 0;
	} else {
		return;
	}
	bui_widget_damage(F.edit_w);
}

/* ------------------------------------------------------------------ */
/* callback dei widget                                                */
/* ------------------------------------------------------------------ */

static void on_back(bui_widget *w)
{
	(void)w;
	switch (F.mode) {
	case M_BROWSE:
		bs_view_close();
		break;
	case M_PROPS:
	case M_RENAME:
		set_mode(M_ACTIONS);
		break;
	case M_TEXT:
		bs_text_close();
		set_mode(M_BROWSE);
		break;
	default:
		set_mode(M_BROWSE);
		break;
	}
}

static void on_up(bui_widget *w)
{
	(void)w;
	go_up();
}

static void on_hidden(bui_widget *w)
{
	(void)w;
	F.show_hidden = !F.show_hidden;
	bui_toast(F.show_hidden ? "File nascosti visibili" : "File nascosti non mostrati", 1600);
	relayout();
	navigate(F.cwd, NULL, 1);
}

static void go_place(int i)
{
	if (i < 0 || i >= F.nplaces)
		return;
	if (F.place[i].make)
		mkdir(F.place[i].path, 0755);	/* cartella standard: nasce al primo uso */
	navigate(F.place[i].path, NULL, 0);
}

static void on_place(bui_widget *w)
{
	go_place(w->id);
}

static void on_side(bui_list *l, int i)
{
	(void)l;
	go_place(i);
}

static void on_select(bui_list *l, int i)
{
	int more = F.from_pointer && F.press_more;

	(void)l;
	if (more)
		open_actions(i);
	else
		open_entry(i);
}

static void on_act_open(bui_widget *w)
{
	int idx = F.act;

	(void)w;
	set_mode(M_BROWSE);
	open_entry(idx);
}

static void on_act_rename(bui_widget *w)
{
	(void)w;
	cpy(F.edit, sizeof(F.edit), F.act_name);
	set_mode(M_RENAME);
}

static void on_act_props(bui_widget *w)
{
	(void)w;
	set_mode(M_PROPS);
}

static void on_act_delete(bui_widget *w)
{
	(void)w;
	ask_delete(F.act);
}

static void on_rename_ok(bui_widget *w)
{
	(void)w;
	rename_apply();
}

static void on_rename_cancel(bui_widget *w)
{
	(void)w;
	set_mode(M_ACTIONS);
}

/* ------------------------------------------------------------------ */
/* disegno                                                            */
/* ------------------------------------------------------------------ */

static void draw_entry(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	const struct entry *e;
	bui_rect badge = bui_r(r.x + 12, r.y + (r.h - 40) / 2, 40, 40);
	bui_rect more = bui_r(r.x + r.w - MORE_W - 4, r.y, MORE_W, r.h);
	int x = r.x + 64, rx = more.x - 4, isdir;
	char sub[96], date[40], size[32];

	(void)l;
	if (i < 0 || i >= F.n)
		return;
	e = &F.e[i];
	isdir = e->kind == K_DIR;
	if ((flags & BUI_F_PRESSED) && F.press_more)
		bui_rrect(c, bui_r(more.x + (MORE_W - 44) / 2, r.y + (r.h - 44) / 2, 44, 44), 22,
			  BUI_C_SURFACE2);
	else if (flags & BUI_F_PRESSED)
		bui_rrect(c, bui_rect_inset(r, 4, 2), 12, BUI_C_SURFACE2);
	if (flags & BUI_F_FOCUS)
		bui_rrect_line(c, bui_rect_inset(r, 4, 2), 12, 2, BUI_C_ACCENT);
	bui_rrect(c, badge, 12, isdir ? BUI_C_ACCENT_BG : (flags & BUI_F_PRESSED) && !F.press_more
		  ? BUI_C_SURFACE3 : BUI_C_SURFACE2);
	bui_icon_in(c, kind_icon(e->kind), badge, isdir ? BUI_C_ACCENT : BUI_C_TEXT2);
	fmt_date(date, sizeof(date), e->mtime);
	if (isdir) {
		cpy(sub, sizeof(sub), date);
	} else {
		bs_fmt_size(size, sizeof(size), e->size);
		snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %s", size, date);
	}
	bui_text_box(c, BUI_FONT_BODY, bui_r(x, r.y + r.h / 2 - 23, rx - x, 22), e->name, BUI_C_TEXT,
		     BUI_AL_LEFT);
	bui_text_box(c, BUI_FONT_SMALL, bui_r(x, r.y + r.h / 2 + 3, rx - x, 20), sub, BUI_C_TEXT2,
		     BUI_AL_LEFT);
	bui_icon_in(c, BUI_IC(more, 24), more, BUI_C_TEXT2);
}

static void draw_side(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	(void)l;
	if (i < 0 || i >= F.nplaces)
		return;
	if (i == F.cur_place)
		flags |= BUI_F_SELECTED;
	bui_list_row(c, r, flags, F.place[i].icon, F.place[i].label, NULL, NULL);
	if (i == F.cur_place && !(flags & BUI_F_PRESSED))
		bui_icon_in(c, F.place[i].icon, bui_r(r.x + 16, r.y, 24, r.h), BUI_C_ACCENT);
}

static void draw_edit(bui_widget *w, bui_canvas *c, bui_rect r)
{
	int tw, maxw = r.w - 32, x;

	(void)w;
	bui_rrect(c, r, 12, BUI_C_SURFACE2);
	bui_rrect_line(c, r, 12, 2, BUI_C_ACCENT);
	tw = bui_text_width(BUI_FONT_BODY, F.edit, -1);
	if (tw <= maxw) {
		bui_text_box(c, BUI_FONT_BODY, bui_r(r.x + 16, r.y, maxw, r.h), F.edit, BUI_C_TEXT,
			     BUI_AL_LEFT);
		x = r.x + 16 + tw;
	} else {
		/* nome piu' lungo del campo: resta visibile la fine, dove si scrive */
		bui_rect saved = bui_clip_push(c, bui_r(r.x + 16, r.y, maxw, r.h));

		bui_text(c, BUI_FONT_BODY, r.x + 16 + maxw - tw, r.y + (r.h + BUI_FONT_BODY->cap_h) / 2,
			 F.edit, BUI_C_TEXT);
		bui_clip_pop(c, saved);
		x = r.x + 16 + maxw;
	}
	bui_fill(c, bui_r(x + 1, r.y + 14, 2, r.h - 28), BUI_C_ACCENT);
}

/* ------------------------------------------------------------------ */
/* costruzione delle pagine                                           */
/* ------------------------------------------------------------------ */

static void build_head(void)
{
	bui_rect a = F.area;
	bui_panel *p = &F.head;
	bui_widget *w;
	int right = a.w - 8;

	bui_panel_clear(p);
	bui_panel_set_viewport(p, bui_r(a.x, a.y, a.w, BUI_HEADER_H), BUI_HEADER_H);
	w = bui_add_button(p, bui_r(0, 0, 64, BUI_HEADER_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND, on_back);
	w->flags |= BUI_F_NOFOCUS;
	if (F.mode == M_BROWSE) {
		right -= 56;
		w = bui_add_button(p, bui_r(right, 0, 56, BUI_HEADER_H), NULL, BUI_IC(up, 24), BUI_BS_ROUND,
				   on_up);
		w->flags |= BUI_F_NOFOCUS;
		right -= 56;
		w = bui_add_button(p, bui_r(right, 0, 56, BUI_HEADER_H), NULL,
				   F.show_hidden ? BUI_IC(eye, 24) : BUI_IC(eye_off, 24), BUI_BS_ROUND,
				   on_hidden);
		w->flags |= BUI_F_NOFOCUS;
		if (F.show_hidden)
			w->color = BUI_C_ACCENT;
	}
	F.title_w = bui_add_label(p, bui_r(68, 6, right - 68 - 8, 26), F.title, BUI_LS_TITLE, BUI_AL_LEFT);
	F.sub_w = bui_add_label(p, bui_r(68, 32, right - 68 - 8, 20), F.sub, BUI_LS_SMALL, BUI_AL_LEFT);
}

static void build_browse(void)
{
	bui_rect a = content_rect();
	bui_panel *p = &F.body;
	int focus_list = F.kbd && F.list.sel >= 0;
	int y = 0, i;

	bui_panel_clear(p);
	bui_panel_set_viewport(p, a, a.h);
	F.side_w = NULL;
	F.list.row_h = ROW_H;
	F.list.draw_row = draw_entry;
	F.list.on_select = on_select;
	F.list.count = F.n;
	if (!F.list.empty_text)
		F.list.empty_text = "";
	if (F.wide) {
		/* orizzontale: scorciatoie in una colonna a sinistra */
		F.side.row_h = BUI_TOUCH;
		F.side.draw_row = draw_side;
		F.side.on_select = on_side;
		F.side.count = F.nplaces;
		if (F.side.sel < 0 || F.side.sel >= F.nplaces)
			F.side.sel = F.cur_place >= 0 ? F.cur_place : 0;
		F.side_w = bui_add_list(p, bui_r(0, 4, SIDE_W, a.h - 4), &F.side);
		F.list_w = bui_add_list(p, bui_r(SIDE_W + 1, 0, a.w - SIDE_W - 1, a.h), &F.list);
	} else {
		/* verticale: striscia di pulsanti in alto, su una o due righe */
		int per = F.nplaces <= 6 ? F.nplaces : (F.nplaces + 1) / 2;
		int bw = (a.w - 16) / (per > 0 ? per : 1);

		for (i = 0; i < F.nplaces; i++) {
			bui_widget *w = bui_add_button(p, bui_r(8 + (i % per) * bw, 4 + (i / per) * 68, bw, 64),
						       F.place[i].label, F.place[i].icon, BUI_BS_SEG, on_place);

			w->flags |= BUI_F_ICON_TOP | (i == F.cur_place ? BUI_F_SELECTED : 0);
			w->font = BUI_FONT_SMALL;
			w->id = i;
		}
		y = 4 + ((F.nplaces + per - 1) / per) * 68 + 4;
		F.list_w = bui_add_list(p, bui_r(0, y, a.w, a.h - y), &F.list);
	}
	if (focus_list)
		bui_panel_focus(p, F.list_w, 1);
}

static void build_page(void)
{
	bui_rect a = content_rect();
	bui_panel *p = &F.page;
	int pad = 16, w = a.w - 2 * pad, y = 8, bw;
	bui_widget *b, *first = NULL;

	bui_panel_clear(p);
	if (w > 640) {
		w = 640;
		pad = (a.w - w) / 2;
	}
	switch (F.mode) {
	case M_ACTIONS:
		bui_add_card(p, bui_r(pad, y, w, 4 * 60 + 8));
		y += 4;
		first = b = bui_add_row(p, bui_r(pad, y, w, 60), BUI_IC(open, 24), "Apri", NULL, on_act_open);
		b->flags |= BUI_F_DIVIDER;
		b = bui_add_row(p, bui_r(pad, y + 60, w, 60), BUI_IC(edit, 24), "Rinomina", NULL,
				on_act_rename);
		b->flags |= BUI_F_DIVIDER;
		b = bui_add_row(p, bui_r(pad, y + 120, w, 60), BUI_IC(info, 24), "Propriet\xC3\xA0", NULL,
				on_act_props);
		b->flags |= BUI_F_DIVIDER;
		bui_add_row(p, bui_r(pad, y + 180, w, 60), BUI_IC(trash, 24), "Elimina", NULL, on_act_delete);
		y += 4 * 60 + 4 + 16;
		break;
	case M_PROPS: {
		int dir = F.e[F.act].kind == K_DIR, n = dir ? 5 : 6;

		bui_add_card(p, bui_r(pad, y, w, n * 52 + 8));
		y += 4;
		bui_add_row(p, bui_r(pad, y, w, 52), NULL, "Nome", F.act_name, NULL)->flags |= BUI_F_DIVIDER;
		y += 52;
		bui_add_row(p, bui_r(pad, y, w, 52), NULL, "Tipo", F.p_type, NULL)->flags |= BUI_F_DIVIDER;
		y += 52;
		if (!dir) {
			bui_add_row(p, bui_r(pad, y, w, 52), NULL, "Dimensione", F.p_size, NULL)->flags |=
				BUI_F_DIVIDER;
			y += 52;
		}
		bui_add_row(p, bui_r(pad, y, w, 52), NULL, "Modificato", F.p_date, NULL)->flags |=
			BUI_F_DIVIDER;
		y += 52;
		bui_add_row(p, bui_r(pad, y, w, 52), NULL, "Permessi", F.p_perm, NULL)->flags |=
			BUI_F_DIVIDER;
		y += 52;
		bui_add_row(p, bui_r(pad, y, w, 52), NULL, "Posizione", F.p_where, NULL);
		y += 52 + 4 + 16;
		break;
	}
	case M_RENAME:
		bui_add_label(p, bui_r(pad + 4, y, w - 8, 28), "NUOVO NOME", BUI_LS_SECTION, BUI_AL_LEFT);
		y += 32;
		F.edit_w = bui_add_custom(p, bui_r(pad, y, w, BUI_TOUCH), draw_edit);
		y += BUI_TOUCH + 8;
		bui_add_label(p, bui_r(pad + 4, y, w - 8, 24), "Scrivi con la tastiera, poi premi Invio.",
			      BUI_LS_SMALL, BUI_AL_LEFT);
		y += 24 + 16;
		bw = (w - 12) / 2;
		bui_add_button(p, bui_r(pad, y, bw, BUI_TOUCH), "Annulla", NULL, BUI_BS_TONAL,
			       on_rename_cancel);
		bui_add_button(p, bui_r(pad + bw + 12, y, w - bw - 12, BUI_TOUCH), "Rinomina",
			       BUI_IC(check, 24), BUI_BS_FILLED, on_rename_ok);
		y += BUI_TOUCH + 16;
		break;
	default:
		break;
	}
	bui_panel_set_viewport(p, a, y);
	if (first && F.kbd)
		bui_panel_focus(p, first, 1);
}

static void relayout(void)
{
	if (F.mode == M_IMAGE) {
		bs_viewer_layout(F.area);
		return;
	}
	build_head();
	bui_panel_clear(&F.body);
	bui_panel_clear(&F.page);
	F.list_w = F.side_w = F.edit_w = NULL;
	switch (F.mode) {
	case M_BROWSE:
		build_browse();
		break;
	case M_TEXT:
		if (!bs_text_layout(content_rect())) {
			/* altra larghezza: gli a capo vanno rifatti */
			char path[PATH_LEN];

			join(path, sizeof(path), F.cwd, F.act_name);
			bs_text_load(path, bs_text_width_for(F.area.w), 0, text_done, NULL);
		}
		break;
	default:
		build_page();
		break;
	}
	bs_view_damage(F.area);
}

static void set_mode(int mode)
{
	F.mode = mode;
	set_titles();
	relayout();
}

static void fl_layout(bs_view *v, bui_rect a)
{
	(void)v;
	F.area = a;
	F.wide = a.w >= 640;
	relayout();
}

static void fl_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	bui_rect a = content_rect();

	(void)v;
	if (F.mode == M_IMAGE) {
		bs_viewer_draw(c, clip);
		return;
	}
	bui_panel_draw(&F.head, c, clip);
	switch (F.mode) {
	case M_BROWSE:
		bui_panel_draw(&F.body, c, clip);
		if (F.wide)
			bui_fill(c, bui_r(a.x + SIDE_W, a.y + 4, 1, a.h - 8), BUI_C_LINE);
		break;
	case M_TEXT:
		bs_text_draw(c, clip);
		break;
	default:
		bui_panel_draw(&F.page, c, clip);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* ingresso                                                           */
/* ------------------------------------------------------------------ */

static void lp_stop(void)
{
	if (F.lp_timer)
		bui_timer_del(F.lp_timer);
	F.lp_timer = 0;
}

/* pressione lunga su una riga: apre le azioni */
static void lp_fire(void *user)
{
	bui_pointer cancel = { .type = BUI_CANCEL };
	int row = F.list.pressed;

	(void)user;
	F.lp_timer = 0;
	if (F.mode != M_BROWSE || row < 0 || row >= F.n || F.loading)
		return;
	bui_panel_pointer(&F.body, &cancel);
	F.lp_swallow = 1;	/* il rilascio del dito non deve attivare nulla */
	open_actions(row);
}

static int browse_pointer(const bui_pointer *ev)
{
	int rc;

	if (ev->type == BUI_PRESS) {
		bui_rect lr = F.list_w ? bui_widget_rect(F.list_w) : bui_r(0, 0, 0, 0);

		lp_stop();
		F.press_more = bui_rect_has(lr, ev->x, ev->y) && ev->x >= lr.x + lr.w - MORE_W - 8;
		rc = bui_panel_pointer(&F.body, ev);
		if (F.list.pressed >= 0 && !F.press_more && bui_rect_has(lr, ev->x, ev->y))
			F.lp_timer = bui_timer_add(LONG_PRESS_MS, 0, lp_fire, NULL);
		return rc;
	}
	if (ev->type == BUI_MOVE && F.lp_timer && F.list.pressed < 0)
		lp_stop();	/* e' diventato uno scorrimento */
	if (ev->type == BUI_RELEASE || ev->type == BUI_CANCEL)
		lp_stop();
	F.from_pointer = 1;
	rc = bui_panel_pointer(&F.body, ev);	/* puo' cambiare pagina */
	F.from_pointer = 0;
	return rc;
}

static int fl_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	if (F.lp_swallow) {
		/* dopo una pressione lunga il resto del gesto non va a nessuno
		 * (la pagina sotto il dito e' gia' cambiata) */
		if (ev->type == BUI_PRESS)
			F.lp_swallow = 0;
		else {
			if (ev->type == BUI_RELEASE || ev->type == BUI_CANCEL)
				F.lp_swallow = 0;
			return 1;
		}
	}
	if (F.mode == M_IMAGE)
		return bs_viewer_pointer(ev);
	if (ev->type == BUI_PRESS || ev->type == BUI_WHEEL) {
		F.cap_head = bui_rect_has(F.head.vp, ev->x, ev->y);
		F.kbd = 0;
	}
	if (F.cap_head)
		return bui_panel_pointer(&F.head, ev);	/* puo' chiudere la vista */
	switch (F.mode) {
	case M_BROWSE:
		return browse_pointer(ev);
	case M_TEXT:
		return bs_text_pointer(ev);
	default:
		return bui_panel_pointer(&F.page, ev);
	}
}

/* una lettera porta alla prossima voce che inizia cosi' */
static int type_ahead(int key)
{
	int i, start = F.list.sel >= 0 ? F.list.sel : -1;

	for (i = 1; i <= F.n; i++) {
		int idx = (start + i + F.n) % F.n;
		unsigned char ch = (unsigned char)F.e[idx].name[0];

		if (ch == '.' && F.e[idx].name[1])
			ch = (unsigned char)F.e[idx].name[1];
		if (ch < 0x80 && (ch | 32) == (key | 32)) {
			F.list.sel = idx;
			bui_panel_focus(&F.body, F.list_w, 1);
			bui_list_reveal(&F.list, idx);
			bui_widget_damage(F.list_w);
			return 1;
		}
	}
	return 0;
}

static int fl_key(bs_view *v, const bui_key *k)
{
	int list_focus;

	(void)v;
	if (F.mode == M_IMAGE)
		return bs_viewer_key(k);
	F.kbd = 1;
	switch (F.mode) {
	case M_TEXT:
		if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE) {
			bs_text_close();
			set_mode(M_BROWSE);
			return 1;
		}
		return bs_text_key(k);
	case M_RENAME:
		if (k->key == BUI_K_ESC) {
			set_mode(M_ACTIONS);
			return 1;
		}
		if (k->key == BUI_K_ENTER && !(F.page.focus_vis && F.page.focus)) {
			rename_apply();
			return 1;
		}
		if (k->key == BUI_K_BACKSPACE || (k->key >= 0x20 && k->key < 0x100)) {
			edit_key(k->key);
			return 1;
		}
		bui_panel_key(&F.page, k);
		return 1;
	case M_ACTIONS:
	case M_PROPS:
		if (k->key == BUI_K_ESC || k->key == BUI_K_BACKSPACE || k->key == BUI_K_LEFT) {
			set_mode(F.mode == M_PROPS ? M_ACTIONS : M_BROWSE);
			return 1;
		}
		bui_panel_key(&F.page, k);
		return 1;
	default:
		break;
	}
	/* elenco */
	list_focus = F.body.focus == F.list_w && F.body.focus_vis && F.list.sel >= 0 && F.list.sel < F.n;
	switch (k->key) {
	case BUI_K_ESC:
		return 0;	/* chiude la vista */
	case BUI_K_BACKSPACE:
		if (!strcmp(F.cwd, "/"))
			return 0;
		go_up();
		return 1;
	case BUI_K_DELETE:
		if (list_focus)
			ask_delete(F.list.sel);
		return 1;
	case BUI_K_RIGHT:
		if (list_focus) {
			open_actions(F.list.sel);
			return 1;
		}
		break;
	case BUI_K_HOME:
	case BUI_K_END:
		if (F.n && F.list_w) {
			F.list.sel = k->key == BUI_K_HOME ? 0 : F.n - 1;
			bui_panel_focus(&F.body, F.list_w, 1);
			bui_list_reveal(&F.list, F.list.sel);
			bui_widget_damage(F.list_w);
		}
		return 1;
	case '.':
		on_hidden(NULL);
		return 1;
	default:
		break;
	}
	if (k->key < 0x80 && k->key != ' ' && F.n && F.list_w &&
	    (((k->key | 32) >= 'a' && (k->key | 32) <= 'z') || (k->key >= '0' && k->key <= '9')))
		return type_ahead(k->key);
	if (!F.body.focus && F.list_w && F.n &&
	    (k->key == BUI_K_UP || k->key == BUI_K_DOWN || k->key == BUI_K_LEFT || k->key == BUI_K_RIGHT)) {
		/* il primo tasto mostra il fuoco sull'elenco, non sulle scorciatoie */
		bui_panel_focus(&F.body, F.list_w, 1);
		return 1;
	}
	return bui_panel_key(&F.body, k);
}

/* ------------------------------------------------------------------ */
/* vita della vista                                                   */
/* ------------------------------------------------------------------ */

static void fl_enter(bs_view *v)
{
	const char *home = getenv("HOME");

	self = v;
	bui_panel_init(&F.head, v->win);
	bui_panel_init(&F.body, v->win);
	bui_panel_init(&F.page, v->win);
	memset(&F.list, 0, sizeof(F.list));
	memset(&F.side, 0, sizeof(F.side));
	F.list.sel = -1;
	F.side.sel = -1;
	F.list.empty_text = "";
	F.title_w = F.sub_w = F.list_w = F.side_w = F.edit_w = NULL;
	cpy(F.home, sizeof(F.home), home && home[0] == '/' ? home : "/");
	if (strlen(F.home) > 1 && F.home[strlen(F.home) - 1] == '/')
		F.home[strlen(F.home) - 1] = 0;
	F.mode = M_BROWSE;
	F.kbd = 0;
	F.lp_swallow = 0;
	F.area = v->area;
	F.wide = v->area.w >= 640;
	places_build();
	F.cur_place = -2;	/* forza il primo confronto */
	navigate(F.home, NULL, 0);
}

static void fl_leave(bs_view *v)
{
	(void)v;
	F.gen++;
	lp_stop();
	if (F.load_timer)
		bui_timer_del(F.load_timer);
	F.load_timer = 0;
	bs_work_cancel(F.load_job);
	F.load_job = 0;
	F.loading = 0;
	bs_viewer_close();
	bs_text_close();
	bui_panel_clear(&F.head);
	bui_panel_clear(&F.body);
	bui_panel_clear(&F.page);
	entries_free(F.e, F.n);
	F.e = NULL;
	F.n = 0;
	free(F.img);
	F.img = NULL;
	F.nimg = 0;
	F.title_w = F.sub_w = F.list_w = F.side_w = F.edit_w = NULL;
}

static int fl_text_input(bs_view *v)
{
	(void)v;
	return F.mode == M_RENAME;	/* nel campo di rinomina Canc e Tab sono tasti */
}

bs_view bs_view_files = {
	.flags = BS_VIEW_NO_HEADER,
	.text_input = fl_text_input,
	.enter = fl_enter,
	.leave = fl_leave,
	.layout = fl_layout,
	.draw = fl_draw,
	.pointer = fl_pointer,
	.key = fl_key,
};
