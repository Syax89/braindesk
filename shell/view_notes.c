// SPDX-License-Identifier: MIT
/*
 * view_notes.c - Note: elenco delle note e passaggio all'editor.
 *
 * Una nota e' un file di testo UTF-8 in ~/Note (*.txt); il titolo e' la prima
 * riga. L'elenco (piu' recenti in alto) arriva da media_scan.c; l'editor e'
 * in view_notes_edit.c. Il salvataggio e' automatico: tre secondi dopo
 * l'ultimo tasto, quando si torna all'elenco e quando la vista si chiude,
 * sempre su un file temporaneo poi rinominato sopra quello vero.
 *
 * La vista disegna da se' l'intestazione (BS_VIEW_NO_HEADER): la freccia
 * "indietro" dell'editor deve tornare all'elenco, non alla griglia.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "media.h"

#define AUTOSAVE_MS 3000
#define ROW_H 72

enum { M_LIST, M_EDIT };

static struct {
	bui_panel p;
	bui_list list;
	bui_win *win;
	bui_rect area;
	int mode;
	bui_widget *w_title, *w_state;
	char path[4096];	/* file della nota aperta; "" = nuova, mai salvata */
	char title[MS_TEXT];
	const char *state;
	int dirty;
	int press_head;		/* il tocco in corso e' dell'intestazione */
	int ask_timer;
} N;

static void rebuild(void);
static void lib_changed(ms_lib *l);

/* ------------------------------------------------------------------ */
/* titolo e anteprima                                                 */
/* ------------------------------------------------------------------ */

static void summary(const char *d, int len, char *title, char *preview, int cap)
{
	int i = 0, j;

	while (i < len && (d[i] == '\n' || d[i] == '\r' || d[i] == ' ' || d[i] == '\t'))
		i++;
	for (j = i; j < len && d[j] != '\n'; j++)
		;
	ms_clean(title, cap < 100 ? cap : 100, d + i, j - i);
	while (j < len && (d[j] == '\n' || d[j] == '\r' || d[j] == ' ' || d[j] == '\t'))
		j++;
	ms_clean(preview, cap, d + j, len - j);
}

/* nel processo figlio della scansione */
static void notes_probe(const char *path, char *s1, char *s2, int cap)
{
	char buf[640];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n > 0)
		summary(buf, (int)n, s1, s2, cap);
}

static int newest_first(const ms_item *a, const ms_item *b)
{
	if (a->mtime != b->mtime)
		return a->mtime > b->mtime ? -1 : 1;
	return strcmp(a->path, b->path);
}

static ms_lib lib = {
	.subdir = "Note",
	.exts = "txt",
	.probe = notes_probe,
	.cmp = newest_first,
	.changed = lib_changed,
};

/* ------------------------------------------------------------------ */
/* file                                                               */
/* ------------------------------------------------------------------ */

static void set_state(const char *s)
{
	if (N.state == s)
		return;
	N.state = s;
	if (N.mode == M_EDIT && N.w_state)
		bui_widget_set_text(N.w_state, s);
}

static void set_title(const char *t)
{
	char old[sizeof(N.title)];

	memcpy(old, N.title, sizeof(old));
	snprintf(N.title, sizeof(N.title), "%s", t && *t ? t : N.path[0] ? "Nota" : "Nuova nota");
	if (N.mode == M_EDIT && N.w_title && strcmp(old, N.title))
		bui_widget_damage(N.w_title);
}

static int write_all(int fd, const char *d, int len)
{
	while (len > 0) {
		ssize_t n = write(fd, d, (size_t)len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		d += n;
		len -= (int)n;
	}
	return 0;
}

/* salva la nota aperta se e' cambiata; 0 se sul disco c'e' il testo corrente */
static int save(void)
{
	char dir[3900], tmp[4200], title[MS_TEXT], preview[MS_TEXT];
	const char *text, *base;
	int len, fd, i;

	if (N.mode != M_EDIT || !N.dirty)
		return 0;
	text = ne_text(&len);
	if (!N.path[0] && !len) {
		/* nota nuova rimasta vuota: non lascia nessun file */
		N.dirty = 0;
		set_state("");
		return 0;
	}
	ms_dir(&lib, dir, sizeof(dir));
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		return -1;
	if (!N.path[0]) {
		time_t now = time(NULL);
		struct tm tm;
		char stamp[32];

		localtime_r(&now, &tm);
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
		snprintf(N.path, sizeof(N.path), "%s/nota-%s.txt", dir, stamp);
		for (i = 2; i < 100 && access(N.path, F_OK) == 0; i++)
			snprintf(N.path, sizeof(N.path), "%s/nota-%s-%d.txt", dir, stamp, i);
	}
	/* file temporaneo nella stessa cartella, poi rename(): sul disco c'e'
	 * sempre o la versione vecchia intera o quella nuova intera */
	base = strrchr(N.path, '/');
	base = base ? base + 1 : N.path;
	snprintf(tmp, sizeof(tmp), "%.*s.%s.tmp", (int)(base - N.path), N.path, base);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;
	if (write_all(fd, text, len) < 0 || fdatasync(fd) < 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	if (close(fd) < 0 || rename(tmp, N.path) < 0) {
		unlink(tmp);
		return -1;
	}
	N.dirty = 0;
	summary(text, len, title, preview, MS_TEXT);
	ms_upsert(&lib, N.path, time(NULL), len, title, preview);
	set_title(title);
	set_state("Salvata");
	return 0;
}

static void on_edit(void)
{
	N.dirty = 1;
	set_state("Modificata");
	bs_view_tick_in(AUTOSAVE_MS);
}

/* ------------------------------------------------------------------ */
/* passaggi fra elenco ed editor                                      */
/* ------------------------------------------------------------------ */

static void show_list(void)
{
	bs_view_tick_in(0);
	ne_close();
	N.mode = M_LIST;
	N.dirty = 0;
	rebuild();
	bs_view_damage(N.area);
	/* rilegge la cartella: l'elenco mostrato intanto e' gia' aggiornato */
	ms_scan(&lib);
}

static void leave_anyway(int button, void *user)
{
	(void)user;
	if (button == 1)
		show_list();
}

/*
 * Un dialogo cattura lo schermo che ha sotto: aperto dentro il click di un
 * pulsante lo fotograferebbe ancora "premuto". Un attimo dopo, invece, il
 * pulsante e' gia' stato ridisegnato a riposo.
 */
static void ask_now(void *user)
{
	N.ask_timer = 0;
	if (bs_view_is_active(&bs_view_notes) && N.mode == M_EDIT)
		bui_dialog_open(user);
}

static void ask(const bui_dialog *d)
{
	if (!N.ask_timer)
		N.ask_timer = bui_timer_add(40, 0, ask_now, (void *)d);
}

static void close_editor(void)
{
	static const bui_dialog d = {
		.title = "Nota non salvata",
		.message = "Non \xC3\xA8 stato possibile salvare la nota. Uscire comunque? "
			   "Le ultime modifiche andranno perse.",
		.buttons = { "Resta", "Esci" },
		.styles = { BUI_BS_TONAL, BUI_BS_DANGER },
		.cancel = 0,
		.done = leave_anyway,
	};

	if (save() < 0) {
		ask(&d);
		return;
	}
	show_list();
}

static void start_edit(const char *path, const char *text, int len, const char *title)
{
	if (ne_open(N.win, text, len, on_edit) < 0)
		return;
	ms_cancel(&lib);
	snprintf(N.path, sizeof(N.path), "%s", path);
	N.mode = M_EDIT;
	N.dirty = 0;
	N.state = "";
	set_title(title);
	rebuild();
	bs_view_damage(N.area);
}

static void open_note(bui_list *l, int i)
{
	const ms_item *it;
	struct stat st;
	char *buf;
	ssize_t n = 0, r;
	int fd;

	(void)l;
	if (i < 0 || i >= lib.n)
		return;
	it = &lib.it[i];
	fd = open(it->path, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fstat(fd, &st) < 0) {
		if (fd >= 0)
			close(fd);
		bui_toast("Impossibile aprire la nota", 2500);
		ms_scan(&lib);
		return;
	}
	if (st.st_size > NE_MAX_BYTES) {
		close(fd);
		bui_toast("Nota troppo grande per l'editor", 2500);
		return;
	}
	buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		close(fd);
		return;
	}
	while (n < st.st_size && (r = read(fd, buf + n, (size_t)(st.st_size - n))) != 0) {
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			break;
		n += r;
	}
	close(fd);
	start_edit(it->path, buf, (int)n, it->s1);
	free(buf);
}

static void on_new(bui_widget *w)
{
	(void)w;
	start_edit("", "", 0, "");
}

static void delete_done(int button, void *user)
{
	(void)user;
	if (button != 1)
		return;
	if (N.path[0]) {
		if (unlink(N.path) < 0 && errno != ENOENT) {
			bui_toast("Impossibile eliminare la nota", 2500);
			return;
		}
		ms_remove(&lib, N.path);
	}
	show_list();
	bui_toast("Nota eliminata", 1800);
}

static void on_delete(bui_widget *w)
{
	static const bui_dialog d = {
		.title = "Eliminare la nota?",
		.message = "La nota verr\xC3\xA0 eliminata definitivamente.",
		.buttons = { "Annulla", "Elimina" },
		.styles = { BUI_BS_TONAL, BUI_BS_DANGER },
		.cancel = 0,
		.done = delete_done,
	};

	(void)w;
	ask(&d);
}

static void on_back(bui_widget *w)
{
	(void)w;
	if (N.mode == M_EDIT)
		close_editor();
	else
		bs_view_close();
}

/* ------------------------------------------------------------------ */
/* elenco                                                             */
/* ------------------------------------------------------------------ */

static void draw_row(bui_list *l, bui_canvas *c, int i, bui_rect r, unsigned flags)
{
	const ms_item *it = &lib.it[i];
	char date[32];

	(void)l;
	ms_fmt_date(date, sizeof(date), it->mtime);
	bui_list_row(c, r, flags, BUI_IC(note, 24), it->s1[0] ? it->s1 : "Senza titolo",
		     it->s2[0] ? it->s2 : it->s1[0] ? "Nessun altro testo" : "Nota vuota", date);
	if (i + 1 < lib.n)
		bui_fill(c, bui_r(r.x + 56, r.y + r.h - 1, r.w - 72, 1), BUI_C_LINE);
}

static const char *empty_text(void)
{
	if (!lib.loaded)
		return "Lettura delle note\xE2\x80\xA6";
	return "Nessuna nota: tocca \xC2\xAB" "Nuova nota\xC2\xBB per iniziare";
}

static void lib_changed(ms_lib *l)
{
	if (!bs_view_is_active(&bs_view_notes) || N.mode != M_LIST)
		return;
	N.list.empty_text = empty_text();
	bui_list_set_count(&N.list, l->n);
}

static void rebuild(void)
{
	bui_panel *p = &N.p;
	bui_rect a = N.area;
	bui_widget *w;

	bui_panel_clear(p);
	N.w_title = N.w_state = NULL;
	w = bui_add_button(p, bui_r(0, 0, 64, BUI_HEADER_H), NULL, BUI_IC(back, 24), BUI_BS_ROUND, on_back);
	w->flags |= BUI_F_NOFOCUS;
	if (N.mode == M_LIST) {
		bui_panel_set_viewport(p, a, a.h);
		bui_add_label(p, bui_r(68, 0, a.w - 68 - 200, BUI_HEADER_H), "Note", BUI_LS_TITLE, BUI_AL_LEFT);
		w = bui_add_button(p, bui_r(a.w - 188, 0, 180, BUI_HEADER_H), "Nuova nota", BUI_IC(add, 24),
				   BUI_BS_GHOST, on_new);
		w->color = BUI_C_ACCENT;
		N.list.count = lib.n;
		N.list.row_h = ROW_H;
		N.list.draw_row = draw_row;
		N.list.on_select = open_note;
		N.list.empty_text = empty_text();
		N.list.sel = -1;
		w = bui_add_list(p, bui_r(0, BUI_HEADER_H, a.w, a.h - BUI_HEADER_H), &N.list);
		bui_panel_focus(p, w, 0);
		return;
	}
	bui_panel_set_viewport(p, bui_r(a.x, a.y, a.w, BUI_HEADER_H), BUI_HEADER_H);
	N.w_title = bui_add_label(p, bui_r(68, 0, a.w - 68 - 64 - 112, BUI_HEADER_H), N.title, BUI_LS_TITLE,
				  BUI_AL_LEFT);
	N.w_state = bui_add_label(p, bui_r(a.w - 64 - 108, 0, 100, BUI_HEADER_H), N.state, BUI_LS_SMALL,
				  BUI_AL_RIGHT);
	w = bui_add_button(p, bui_r(a.w - 64, 0, 64, BUI_HEADER_H), NULL, &bs_ic_delete_24, BUI_BS_ROUND,
			   on_delete);
	w->flags |= BUI_F_NOFOCUS;
	ne_layout(bui_r(a.x, a.y + BUI_HEADER_H, a.w, a.h - BUI_HEADER_H));
}

/* ------------------------------------------------------------------ */
/* vista                                                              */
/* ------------------------------------------------------------------ */

static void nt_enter(bs_view *v)
{
	N.win = v->win;
	N.mode = M_LIST;
	N.dirty = 0;
	bui_panel_init(&N.p, v->win);
	ms_scan(&lib);
}

static void nt_leave(bs_view *v)
{
	(void)v;
	if (N.mode == M_EDIT && save() < 0)
		bui_toast("Impossibile salvare la nota", 3000);
	ne_close();
	N.mode = M_LIST;
	bui_panel_clear(&N.p);
	ms_cancel(&lib);
}

static void nt_layout(bs_view *v, bui_rect a)
{
	(void)v;
	N.area = a;
	rebuild();
}

static void nt_draw(bs_view *v, bui_canvas *c, bui_rect clip)
{
	(void)v;
	bui_panel_draw(&N.p, c, clip);
	if (N.mode == M_EDIT)
		ne_draw(c, clip);
}

static int nt_pointer(bs_view *v, const bui_pointer *ev)
{
	(void)v;
	if (N.mode == M_LIST)
		return bui_panel_pointer(&N.p, ev);
	if (ev->type == BUI_PRESS || ev->type == BUI_WHEEL)
		N.press_head = bui_rect_has(N.p.vp, ev->x, ev->y);
	return N.press_head ? bui_panel_pointer(&N.p, ev) : ne_pointer(ev);
}

static int nt_key(bs_view *v, const bui_key *k)
{
	(void)v;
	if (N.mode == M_EDIT) {
		if (k->key == BUI_K_ESC) {
			close_editor();
			return 1;
		}
		ne_key(k);
		return 1;	/* nell'editor Backspace non deve mai chiudere la vista */
	}
	if (bui_panel_key(&N.p, k))
		return 1;
	if (k->key == 'n' || k->key == 'N' || k->key == '+') {
		on_new(NULL);
		return 1;
	}
	return 0;
}

static void nt_tick(bs_view *v, uint32_t now)
{
	(void)v;
	(void)now;
	if (save() < 0)
		bui_toast("Impossibile salvare la nota", 3000);
}

bs_view bs_view_notes = {
	.flags = BS_VIEW_NO_HEADER,
	.enter = nt_enter,
	.leave = nt_leave,
	.layout = nt_layout,
	.draw = nt_draw,
	.pointer = nt_pointer,
	.key = nt_key,
	.tick = nt_tick,
};
