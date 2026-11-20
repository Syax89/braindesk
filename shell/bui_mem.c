// SPDX-License-Identifier: MIT
/*
 * bui_mem.c - backend "mem": lo stesso compositore del backend DRM, ma la
 * superficie d'uscita e' un buffer in memoria e l'ingresso viene da uno
 * script. Serve a provare la shell senza X e senza pannello (Docker, qemu,
 * ASan), con gli stessi percorsi di codice del Brain tranne gli ioctl.
 *
 *   BRAINSHELL_BACKEND=mem BRAINSHELL_SCRIPT=scena.txt [BRAINSHELL_MEM_SIZE=854x480]
 *   [BRAINSHELL_SHOTS=cartella] [BUI_MEM_STATE=file] brainshell
 *
 * Comandi dello script (uno per riga, # commento):
 *
 *   wait MS                      aspetta
 *   tap X Y [MS]                 tocco breve (70 ms), poi MS di pausa (300)
 *   press X Y [MS] / move X Y / release [MS]
 *   drag X0 Y0 X1 Y1 [PASSI [MS_PER_PASSO [MS]]]  trascina e rilascia in corsa
 *   key SPEC... [MS]             SPEC = [ctrl+][alt+][shift+]NOME (Return, Escape,
 *                                a, 1, comma...): passa dalla mappa evdev vera
 *   type TESTO                   ogni carattere come tasti (accentate con Alt)
 *   hold SPEC MS                 tiene premuto (ripetizione del tasto)
 *   stalekey SPEC                tasto con un tempo di 100 s fa (accodato prima di un risveglio)
 *   shot NOME                    PNG dello schermo in $BRAINSHELL_SHOTS/NOME.png
 *   expect CHIAVE=VALORE...      confronta con lo stato della shell: CHECK ok / FALLITO
 *   pixel X Y                    stampa il colore (RRGGBB)
 *   rss                          stampa VmRSS e RssAnon
 *   stats                        fotogrammi e righe inviate dall'ultimo "stats"
 *   log TESTO                    stampa
 *   sh COMANDO                   esegue un comando di shell (prepara le prove)
 *   quit                         esce (anche a fine script)
 *
 * BUI_MEM_STATE: file in cui il backend scrive "active" o "suspended" a ogni
 * bui_suspend() (lo legge il finto Xorg delle prove).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bui.h"
#include "bui_be.h"
#include "bui_comp.h"
#include "bui_evdev.h"

void bui__state_query(char *buf, int len);

#define MAX_LINES 2048

static struct {
	int w, h;
	bui_px *fb;
	char **line;
	int nlines, pc;
	int timer;
	/* comando in corso a piu' passi */
	int step, nsteps, x0, y0, x1, y1, ms, pause;
	bui_kbd kbd;
	int hold_code, hold_timer;
	unsigned frames, rows;
	int trace;
	const char *shots;
	int fails;
} M;

static int m_screen_w(void) { return M.w; }
static int m_screen_h(void) { return M.h; }

static void present(const bui_rect *bands, int n)
{
	int i;

	M.frames++;
	for (i = 0; i < n; i++) {
		M.rows += (unsigned)bands[i].h;
		if (M.trace)
			fprintf(stderr, "bui: dirtyfb %lu righe %d-%d (%d)\n", (unsigned long)(bui_now() % 100000),
				bands[i].y, bands[i].y + bands[i].h, bands[i].h);
	}
}

/* ------------------------------------------------------------------ */
/* PNG senza compressione (blocchi "stored"): niente zlib              */
/* ------------------------------------------------------------------ */

static uint32_t crc_tab[256];

static uint32_t crc(uint32_t c, const unsigned char *p, size_t n)
{
	size_t i;

	if (!crc_tab[1]) {
		uint32_t k, v;
		int j;

		for (k = 0; k < 256; k++) {
			v = k;
			for (j = 0; j < 8; j++)
				v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
			crc_tab[k] = v;
		}
	}
	c = ~c;
	for (i = 0; i < n; i++)
		c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return ~c;
}

static void be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *type, const unsigned char *data, uint32_t n)
{
	unsigned char h[8];
	uint32_t c;

	be32(h, n);
	memcpy(h + 4, type, 4);
	fwrite(h, 1, 8, f);
	if (n)
		fwrite(data, 1, n, f);
	c = crc(0, h + 4, 4);
	c = crc(c, data, n);
	be32(h, c);
	fwrite(h, 1, 4, f);
}

static int write_png(const char *path, const bui_px *px, int w, int h, int stride)
{
	size_t raw = (size_t)(w * 3 + 1) * h, nblk = (raw + 65534) / 65535, zn = 2 + raw + nblk * 5 + 4;
	unsigned char *rows = malloc(raw), *z = malloc(zn), ihdr[13];
	uint32_t a = 1, b = 0;
	size_t i, o = 0, left;
	FILE *f;
	int x, y;

	if (!rows || !z) {
		free(rows);
		free(z);
		return -1;
	}
	for (y = 0; y < h; y++) {
		unsigned char *r = rows + (size_t)y * (w * 3 + 1);

		r[0] = 0;
		for (x = 0; x < w; x++) {
			unsigned p = px[y * stride + x];
			unsigned rr = (p >> 11) & 31, gg = (p >> 5) & 63, bb = p & 31;

			r[1 + 3 * x] = (unsigned char)(rr << 3 | rr >> 2);
			r[2 + 3 * x] = (unsigned char)(gg << 2 | gg >> 4);
			r[3 + 3 * x] = (unsigned char)(bb << 3 | bb >> 2);
		}
	}
	z[o++] = 0x78;
	z[o++] = 0x01;
	for (i = 0, left = raw; left; ) {
		size_t n = left > 65535 ? 65535 : left;

		z[o++] = left == n;
		z[o++] = (unsigned char)n;
		z[o++] = (unsigned char)(n >> 8);
		z[o++] = (unsigned char)~n;
		z[o++] = (unsigned char)(~n >> 8);
		memcpy(z + o, rows + i, n);
		o += n;
		i += n;
		left -= n;
	}
	for (i = 0; i < raw; i++) {
		a = (a + rows[i]) % 65521;
		b = (b + a) % 65521;
	}
	be32(z + o, (b << 16) | a);
	o += 4;
	f = fopen(path, "wb");
	if (!f) {
		free(rows);
		free(z);
		return -1;
	}
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
	be32(ihdr, (uint32_t)w);
	be32(ihdr + 4, (uint32_t)h);
	ihdr[8] = 8;	/* bit */
	ihdr[9] = 2;	/* RGB */
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "IDAT", z, (uint32_t)o);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	free(rows);
	free(z);
	return 0;
}

/* ------------------------------------------------------------------ */
/* ingresso dallo script                                              */
/* ------------------------------------------------------------------ */

static void pointer(int type, int x, int y)
{
	if (bui__suspended())
		bui__input_filter(type, NULL, 1);
	else
		bui_comp_pointer(type, x, y, bui_now());
}

static uint32_t key_time;	/* tempo degli eventi (0: adesso) */

static void key_ev(int code, int value)
{
	uint32_t t = key_time ? key_time : bui_now();
	bui_key k;

	if (bui__input_stale(t))
		return;		/* come bui_drm.c: accodato prima del risveglio */
	if (!bui_kbd_event(&M.kbd, code, value, t, &k))
		return;
	if (bui__suspended())
		bui__input_filter(BUI_IN_KEY, &k, 1);
	else
		bui_comp_key(&k);
}

/* "ctrl+alt+Escape": premuti i modificatori, il tasto, poi rilasciati */
static int key_spec(const char *spec, int hold)
{
	char buf[64], *s = buf, *p;
	int mods[3], nm = 0, code, i;

	snprintf(buf, sizeof(buf), "%s", spec);
	while ((p = strchr(s, '+')) != NULL && p[1]) {
		*p = 0;
		if (!strcmp(s, "ctrl"))
			mods[nm++] = KEY_LEFTCTRL;
		else if (!strcmp(s, "alt"))
			mods[nm++] = KEY_LEFTALT;
		else if (!strcmp(s, "shift"))
			mods[nm++] = KEY_LEFTSHIFT;
		s = p + 1;
		if (nm == 3)
			break;
	}
	code = bui_kbd_code_by_name(s);
	if (!code) {
		/* nomi di xdotool per i simboli */
		static const char *const sym[][2] = {
			{ "asterisk", "*" }, { "plus", "+" }, { "percent", "%" }, { "parenleft", "(" },
			{ "parenright", ")" }, { "exclam", "!" }, { "question", "?" }, { "colon", ":" },
			{ "at", "@" }, { "numbersign", "#" }, { "dollar", "$" }, { "underscore", "_" },
			{ "quotedbl", "\"" }, { "less", "<" }, { "greater", ">" },
		};
		int sh, al;

		for (i = 0; i < (int)(sizeof(sym) / sizeof(sym[0])); i++)
			if (!strcmp(s, sym[i][0])) {
				code = bui_kbd_code_for((unsigned char)sym[i][1][0], &sh, &al);
				if (sh && nm < 3)
					mods[nm++] = KEY_LEFTSHIFT;
			}
	}
	if (!code) {
		fprintf(stderr, "bui-mem: tasto sconosciuto \"%s\"\n", spec);
		return -1;
	}
	if (s[0] >= 'A' && s[0] <= 'Z' && !s[1] && nm < 3)
		mods[nm++] = KEY_LEFTSHIFT;
	for (i = 0; i < nm; i++)
		key_ev(mods[i], 1);
	key_ev(code, 1);
	if (hold)
		return code;
	key_ev(code, 0);
	for (i = nm - 1; i >= 0; i--)
		key_ev(mods[i], 0);
	return 0;
}

static void type_text(const char *s)
{
	while (*s) {
		unsigned c = (unsigned char)*s++;
		int code, sh, al;

		/* UTF-8 a due o tre byte (accentate, euro) */
		if (c >= 0xC0 && c < 0xE0 && *s) {
			c = ((c & 0x1F) << 6) | ((unsigned char)*s++ & 0x3F);
		} else if (c >= 0xE0 && s[0] && s[1]) {
			c = ((c & 0x0F) << 12) | (((unsigned char)s[0] & 0x3F) << 6) |
			    ((unsigned char)s[1] & 0x3F);
			s += 2;
		}
		code = c == '\n' ? KEY_ENTER : bui_kbd_code_for((int)c, &sh, &al);
		if (!code) {
			fprintf(stderr, "bui-mem: carattere senza tasto U+%04X\n", c);
			continue;
		}
		if (al)
			key_ev(KEY_LEFTALT, 1);
		if (sh)
			key_ev(KEY_LEFTSHIFT, 1);
		key_ev(code, 1);
		key_ev(code, 0);
		if (sh)
			key_ev(KEY_LEFTSHIFT, 0);
		if (al)
			key_ev(KEY_LEFTALT, 0);
	}
}

static void hold_repeat(void *user)
{
	bui_key k;

	(void)user;
	M.hold_timer = 0;
	if (!M.hold_code)
		return;
	if (bui_kbd_repeat(&M.kbd, bui_now(), &k)) {
		if (bui__suspended())
			bui__input_filter(BUI_IN_KEY, &k, 1);
		else
			bui_comp_key(&k);
	}
	M.hold_timer = bui_timer_add(5, 0, hold_repeat, NULL);
}

static void print_rss(void)
{
	char buf[4096], *p;
	FILE *f = fopen("/proc/self/status", "r");
	size_t n;

	if (!f)
		return;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	for (p = strtok(buf, "\n"); p; p = strtok(NULL, "\n"))
		if (!strncmp(p, "VmRSS", 5) || !strncmp(p, "RssAnon", 7) || !strncmp(p, "RssFile", 7) ||
		    !strncmp(p, "RssShmem", 8))
			printf("RSS %s\n", p);
	fflush(stdout);
}

static void expect(char *args)
{
	char st[256], q[200], *tok, *save = NULL, pat[96];

	bui__state_query(q, sizeof(q));
	snprintf(st, sizeof(st), "popups=%d suspended=%d %s", bui_comp_popups(), bui__suspended(), q);
	for (tok = strtok_r(args, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		char padded[260];

		snprintf(padded, sizeof(padded), " %s ", st);
		snprintf(pat, sizeof(pat), " %s ", tok);
		if (strstr(padded, pat)) {
			printf("CHECK ok       %s\n", tok);
		} else {
			printf("CHECK FALLITO  %s (stato: %s)\n", tok, st);
			M.fails++;
		}
	}
	fflush(stdout);
}

static void next_cmd(void *user);

static void after(int ms)
{
	M.timer = bui_timer_add(ms, 0, next_cmd, NULL);
}

static void drag_step(void *user)
{
	(void)user;
	M.step++;
	if (M.step <= M.nsteps) {
		pointer(BUI_MOVE, M.x0 + (M.x1 - M.x0) * M.step / M.nsteps,
			M.y0 + (M.y1 - M.y0) * M.step / M.nsteps);
		M.timer = bui_timer_add(M.ms, 0, drag_step, NULL);
		return;
	}
	pointer(BUI_RELEASE, M.x1, M.y1);
	after(M.pause);
}

static void tap_release(void *user)
{
	(void)user;
	pointer(BUI_RELEASE, M.x0, M.y0);
	after(M.ms);
}

static void hold_end(void *user)
{
	(void)user;
	bui_timer_del(M.hold_timer);
	M.hold_timer = 0;
	key_ev(M.hold_code, 0);
	M.hold_code = 0;
	after(250);
}

static void next_cmd(void *user)
{
	char buf[512], cmd[32], *arg;
	int a[7], n;

	(void)user;
	M.timer = 0;
	for (;;) {
		if (M.pc >= M.nlines) {
			if (M.fails)
				printf("bui-mem: %d controlli falliti\n", M.fails);
			fflush(stdout);
			bui_quit();
			return;
		}
		snprintf(buf, sizeof(buf), "%s", M.line[M.pc++]);
		buf[strcspn(buf, "\r\n")] = 0;
		arg = buf + strspn(buf, " \t");
		if (!*arg || *arg == '#')
			continue;
		break;
	}
	if (sscanf(arg, "%31s", cmd) != 1)
		cmd[0] = 0;
	arg += strlen(cmd);
	arg += strspn(arg, " \t");
	memset(a, 0, sizeof(a));
	n = sscanf(arg, "%d %d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]);
	if (!strcmp(cmd, "wait")) {
		after(a[0]);
	} else if (!strcmp(cmd, "tap") && n >= 2) {
		M.x0 = a[0];
		M.y0 = a[1];
		M.ms = n >= 3 ? a[2] : 300;
		pointer(BUI_PRESS, a[0], a[1]);
		M.timer = bui_timer_add(70, 0, tap_release, NULL);
	} else if (!strcmp(cmd, "press") && n >= 2) {
		M.x0 = a[0];
		M.y0 = a[1];
		pointer(BUI_PRESS, a[0], a[1]);
		after(n >= 3 ? a[2] : 150);
	} else if (!strcmp(cmd, "move") && n >= 2) {
		pointer(BUI_MOVE, a[0], a[1]);
		after(n >= 3 ? a[2] : 16);
	} else if (!strcmp(cmd, "release")) {
		pointer(BUI_RELEASE, M.x0, M.y0);
		after(n >= 1 ? a[0] : 300);
	} else if (!strcmp(cmd, "drag") && n >= 4) {
		M.x0 = a[0];
		M.y0 = a[1];
		M.x1 = a[2];
		M.y1 = a[3];
		M.nsteps = n >= 5 && a[4] > 0 ? a[4] : 10;
		M.ms = n >= 6 ? a[5] : 16;
		M.pause = n >= 7 ? a[6] : 300;
		M.step = 0;
		pointer(BUI_PRESS, a[0], a[1]);
		M.timer = bui_timer_add(30, 0, drag_step, NULL);
	} else if (!strcmp(cmd, "key")) {
		char *tok, *save = NULL;
		int pause = 250;

		for (tok = strtok_r(arg, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
			if (tok[0] >= '0' && tok[0] <= '9' && tok[1]) {
				pause = atoi(tok);	/* numero di piu' cifre: la pausa */
				continue;
			}
			key_spec(tok, 0);
		}
		after(pause);
	} else if (!strcmp(cmd, "hold")) {
		char name[64];
		int ms = 1000;

		if (sscanf(arg, "%63s %d", name, &ms) >= 1) {
			M.hold_code = key_spec(name, 1);
			if (M.hold_code > 0) {
				M.hold_timer = bui_timer_add(5, 0, hold_repeat, NULL);
				M.timer = bui_timer_add(ms, 0, hold_end, NULL);
				return;
			}
		}
		after(0);
	} else if (!strcmp(cmd, "stalekey")) {
		key_time = bui_now() - 100000;
		key_spec(arg, 0);
		key_time = 0;
		after(250);
	} else if (!strcmp(cmd, "type")) {
		type_text(arg);
		after(250);
	} else if (!strcmp(cmd, "shot")) {
		char path[512];

		char *sl;

		snprintf(path, sizeof(path), "%s/%s.png", M.shots, arg);
		/* mkdir -p della cartella */
		for (sl = strchr(path + 1, '/'); sl; sl = strchr(sl + 1, '/')) {
			*sl = 0;
			mkdir(path, 0755);
			*sl = '/';
		}
		if (write_png(path, M.fb, M.w, M.h, M.w) < 0)
			fprintf(stderr, "bui-mem: %s: %s\n", path, strerror(errno));
		after(0);
	} else if (!strcmp(cmd, "expect")) {
		expect(arg);
		after(0);
	} else if (!strcmp(cmd, "pixel") && n >= 2) {
		unsigned p = a[0] >= 0 && a[0] < M.w && a[1] >= 0 && a[1] < M.h ? M.fb[a[1] * M.w + a[0]] : 0;

		printf("PIXEL %d %d %02X%02X%02X\n", a[0], a[1], ((p >> 11) & 31) << 3, ((p >> 5) & 63) << 2,
		       (p & 31) << 3);
		fflush(stdout);
		after(0);
	} else if (!strcmp(cmd, "rss")) {
		print_rss();
		after(0);
	} else if (!strcmp(cmd, "stats")) {
		printf("STATS %u fotogrammi, %u righe inviate\n", M.frames, M.rows);
		fflush(stdout);
		M.frames = M.rows = 0;
		after(0);
	} else if (!strcmp(cmd, "log")) {
		printf("%s\n", arg);
		fflush(stdout);
		after(0);
	} else if (!strcmp(cmd, "sh")) {
		fflush(stdout);
		if (system(arg) != 0)
			fprintf(stderr, "bui-mem: \"%s\" non riuscito\n", arg);
		after(0);
	} else if (!strcmp(cmd, "quit")) {
		M.pc = M.nlines;
		after(0);
	} else {
		fprintf(stderr, "bui-mem: comando sconosciuto alla riga %d: %s\n", M.pc, cmd);
		after(0);
	}
}

static int load_script(const char *path)
{
	FILE *f = fopen(path, "r");
	char buf[512];

	if (!f) {
		fprintf(stderr, "bui-mem: %s: %s\n", path, strerror(errno));
		return -1;
	}
	M.line = calloc(MAX_LINES, sizeof(char *));
	while (M.line && M.nlines < MAX_LINES && fgets(buf, sizeof(buf), f))
		M.line[M.nlines++] = strdup(buf);
	fclose(f);
	return 0;
}

static void write_state(int suspended)
{
	const char *p = getenv("BUI_MEM_STATE");
	FILE *f;

	if (!p || !*p)
		return;
	f = fopen(p, "w");
	if (f) {
		fputs(suspended ? "suspended\n" : "active\n", f);
		fclose(f);
	}
}

/* ------------------------------------------------------------------ */
/* backend                                                            */
/* ------------------------------------------------------------------ */

static int m_init(void)
{
	const char *size = getenv("BRAINSHELL_MEM_SIZE"), *script = getenv("BRAINSHELL_SCRIPT");

	M.w = 854;
	M.h = 480;
	if (size && sscanf(size, "%dx%d", &M.w, &M.h) != 2)
		M.w = 854, M.h = 480;
	if (M.w < 64 || M.h < 64 || M.w > 4096 || M.h > 4096)
		return -1;
	M.fb = calloc((size_t)M.w * M.h, sizeof(bui_px));
	if (!M.fb)
		return -1;
	M.trace = getenv("BUI_TRACE") != NULL;
	M.shots = getenv("BRAINSHELL_SHOTS");
	if (!M.shots || !*M.shots)
		M.shots = "shots";
	bui_comp_init(M.w, M.h, M.fb, M.w, present);
	write_state(0);
	if (script && *script) {
		if (load_script(script) < 0)
			return -1;
		/* lo script parte quando il primo fotogramma e' composto */
		M.timer = bui_timer_add(50, 0, next_cmd, NULL);
	}
	return 0;
}

static void m_shutdown(void)
{
	int i;

	bui_timer_del(M.timer);
	bui_timer_del(M.hold_timer);
	bui_comp_shutdown();
	free(M.fb);
	M.fb = NULL;
	for (i = 0; i < M.nlines; i++)
		free(M.line[i]);
	free(M.line);
	M.line = NULL;
	M.nlines = 0;
}

static int m_flush(uint32_t now)
{
	return bui_comp_flush(now);
}

static int m_suspend(int on)
{
	if (on) {
		bui_comp_cancel_pointer();
		M.kbd.rep_code = 0;
	} else {
		bui_comp_damage_screen(bui_r(0, 0, M.w, M.h));
	}
	write_state(on);
	return 0;
}

const bui_backend bui_be_mem = {
	.name = "mem",
	.init = m_init,
	.shutdown = m_shutdown,
	.screen_w = m_screen_w,
	.screen_h = m_screen_h,
	.win_create = bui_comp_win_create,
	.win_destroy = bui_comp_win_destroy,
	.win_show = bui_comp_win_show,
	.win_hide = bui_comp_win_hide,
	.win_move_resize = bui_comp_win_move_resize,
	.win_set_strut_top = bui_comp_win_set_strut_top,
	.popup_open = bui_comp_popup_open,
	.popup_open_round = bui_comp_popup_open_round,
	.flush = m_flush,
	.suspend = m_suspend,
	.redraw_all = bui_comp_redraw_all,
};
