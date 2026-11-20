// SPDX-License-Identifier: MIT
/*
 * bui_drm.c - backend DRM: la shell disegna direttamente sul pannello.
 *
 * Solo ioctl grezzi del kernel (<drm/drm.h>, <drm/drm_mode.h>), niente
 * libdrm, niente X. Il pannello del Brain e' di tipo MIPI-DBI: non viene
 * letto di continuo, il kernel gli invia soltanto le righe nominate da
 * DRM_IOCTL_MODE_DIRTYFB (arrotondate a fasce di 4). Quindi:
 *
 *   - le finestre si disegnano nelle loro tele in RAM normale (bui_comp.c);
 *   - i rettangoli cambiati vengono copiati nel dumb buffer (CMA,
 *     write-combined: ci si scrive soltanto, leggerlo e' lentissimo);
 *   - per ogni fascia di righe un DIRTYFB. Il driver unisce le clip di una
 *     chiamata in un solo rettangolo, quindi fasce lontane (barra in alto,
 *     toast in basso) partono con chiamate separate.
 *
 * Niente page flip, niente vblank, un solo buffer.
 *
 * Ingresso: i dispositivi evdev trovati per nome in /sys/class/input
 * (tocco "mxs-lradc-ts", tastiere "brain-kbd-gpio" e "brain-kbd-i2c"; in
 * mancanza, qualunque dispositivo che sembri un tocco o una tastiera),
 * catturati con EVIOCGRAB perche' i tasti non arrivino anche alla console.
 * Traduzione in bui_evdev.c.
 *
 * Per un'app X (xapp.c): bui_suspend(1) lascia il master DRM e libera i
 * dispositivi (restano aperti senza cattura: la shell ne vede ancora i
 * tempi per l'inattivita' e la combinazione Ctrl+Alt+Esc); bui_suspend(0)
 * riprende il master, rimette il proprio framebuffer sul CRTC e ricompone
 * tutto lo schermo dalle tele.
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "bui.h"
#include "bui_be.h"
#include "bui_comp.h"
#include "bui_evdev.h"

#define MAX_DEV 6
#define BAND_TRACE_MAX 16

enum { DEV_TOUCH = 1, DEV_KBD = 2 };

struct dev {
	int fd;
	int kind;
	char name[64];
	char path[32];
	int mono;		/* tempi sull'orologio di bui_now (CLOCK_BOOTTIME) */
	bui_touch touch;
};

static struct {
	int fd;
	uint32_t conn_id, crtc_id, fb_id, handle;
	struct drm_mode_modeinfo mode;
	int w, h;
	uint32_t pitch;
	uint64_t size;
	bui_px *map;
	int need_modeset;
	int master;
	int trace;
	struct dev dev[MAX_DEV];
	int ndev;
	bui_kbd kbd;
	int rep_timer;
} D = { .fd = -1 };

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* ------------------------------------------------------------------ */
/* uscita                                                             */
/* ------------------------------------------------------------------ */

static int modeset(void)
{
	struct drm_mode_crtc c;

	memset(&c, 0, sizeof(c));
	c.crtc_id = D.crtc_id;
	c.fb_id = D.fb_id;
	c.set_connectors_ptr = (uintptr_t)&D.conn_id;
	c.count_connectors = 1;
	c.mode = D.mode;
	c.mode_valid = 1;
	if (xioctl(D.fd, DRM_IOCTL_MODE_SETCRTC, &c) < 0) {
		fprintf(stderr, "bui: SETCRTC: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void present(const bui_rect *bands, int n)
{
	int i;

	if (!D.master)
		return;
	if (D.need_modeset) {
		/* il cambio di framebuffer invia gia' tutto lo schermo */
		if (modeset() == 0)
			D.need_modeset = 0;
		if (D.trace)
			fprintf(stderr, "bui: setcrtc %ux%u\n", D.w, D.h);
		return;
	}
	for (i = 0; i < n; i++) {
		struct drm_clip_rect clip;
		struct drm_mode_fb_dirty_cmd dc;

		clip.x1 = 0;
		clip.y1 = (uint16_t)bands[i].y;
		clip.x2 = (uint16_t)D.w;
		clip.y2 = (uint16_t)(bands[i].y + bands[i].h);
		memset(&dc, 0, sizeof(dc));
		dc.fb_id = D.fb_id;
		dc.num_clips = 1;
		dc.clips_ptr = (uintptr_t)&clip;
		if (xioctl(D.fd, DRM_IOCTL_MODE_DIRTYFB, &dc) < 0 && errno != ENOSYS)
			fprintf(stderr, "bui: DIRTYFB: %s\n", strerror(errno));
		if (D.trace)
			fprintf(stderr, "bui: dirtyfb %lu righe %d-%d (%d)\n",
				(unsigned long)(bui_now() % 100000), clip.y1, clip.y2, clip.y2 - clip.y1);
	}
}

static int drm_open(void)
{
	const char *path = getenv("BRAINSHELL_DRM_DEVICE");
	struct drm_mode_card_res res;
	uint32_t conns[8], crtcs[8], encs[8];
	struct drm_mode_modeinfo modes[16];
	struct drm_mode_get_connector conn;
	struct drm_mode_get_encoder enc;
	struct drm_mode_create_dumb cd;
	struct drm_mode_fb_cmd fc;
	struct drm_mode_map_dumb md;
	unsigned i, j, m;
	void *p;

	D.fd = open(path && *path ? path : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (D.fd < 0) {
		fprintf(stderr, "bui: /dev/dri/card0: %s\n", strerror(errno));
		return -1;
	}
	/* il primo che apre il dispositivo e' gia' master; SET_MASTER lo conferma
	 * (e fallisce se lo tiene qualcun altro, per esempio X) */
	if (xioctl(D.fd, DRM_IOCTL_SET_MASTER, NULL) < 0) {
		fprintf(stderr, "bui: DRM master non disponibile (%s): lo schermo e' di un altro\n",
			strerror(errno));
		return -1;
	}
	D.master = 1;

	memset(&res, 0, sizeof(res));
	if (xioctl(D.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	if (res.count_connectors > 8)
		res.count_connectors = 8;
	if (res.count_crtcs > 8)
		res.count_crtcs = 8;
	if (res.count_encoders > 8)
		res.count_encoders = 8;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_fbs = 0;
	if (xioctl(D.fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 || !res.count_crtcs)
		return -1;

	for (i = 0; i < res.count_connectors; i++) {
		memset(&conn, 0, sizeof(conn));
		conn.connector_id = conns[i];
		if (xioctl(D.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || !conn.count_modes)
			continue;
		if (conn.count_modes > 16)
			conn.count_modes = 16;
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_props = conn.count_encoders = 0;
		if (xioctl(D.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || !conn.count_modes)
			continue;
		if (conn.connection == 2)	/* disconnesso */
			continue;
		m = 0;
		for (j = 0; j < conn.count_modes; j++)
			if (modes[j].type & DRM_MODE_TYPE_PREFERRED) {
				m = j;
				break;
			}
		D.conn_id = conns[i];
		D.mode = modes[m];
		D.crtc_id = crtcs[0];
		if (conn.encoder_id) {
			memset(&enc, 0, sizeof(enc));
			enc.encoder_id = conn.encoder_id;
			if (xioctl(D.fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
				D.crtc_id = enc.crtc_id;
		}
		break;
	}
	if (!D.conn_id) {
		fprintf(stderr, "bui: nessun connettore con un modo\n");
		return -1;
	}
	/* il modo dice l'orientamento: 854x480, oppure 480x854 se il pannello
	 * e' stato ruotato dal device tree */
	D.w = D.mode.hdisplay;
	D.h = D.mode.vdisplay;

	memset(&cd, 0, sizeof(cd));
	cd.width = (uint32_t)D.w;
	cd.height = (uint32_t)D.h;
	cd.bpp = 16;
	if (xioctl(D.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
		fprintf(stderr, "bui: CREATE_DUMB: %s\n", strerror(errno));
		return -1;
	}
	D.handle = cd.handle;
	D.pitch = cd.pitch;
	D.size = cd.size;
	memset(&fc, 0, sizeof(fc));
	fc.width = cd.width;
	fc.height = cd.height;
	fc.pitch = cd.pitch;
	fc.bpp = 16;
	fc.depth = 16;		/* RGB565 */
	fc.handle = cd.handle;
	if (xioctl(D.fd, DRM_IOCTL_MODE_ADDFB, &fc) < 0) {
		fprintf(stderr, "bui: ADDFB: %s\n", strerror(errno));
		return -1;
	}
	D.fb_id = fc.fb_id;
	memset(&md, 0, sizeof(md));
	md.handle = cd.handle;
	if (xioctl(D.fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return -1;
	p = mmap(NULL, (size_t)cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, (off_t)md.offset);
	if (p == MAP_FAILED) {
		fprintf(stderr, "bui: mmap del dumb buffer: %s\n", strerror(errno));
		return -1;
	}
	D.map = p;
	D.need_modeset = 1;	/* al primo fotogramma, gia' composto: niente lampi */
	if (D.pitch != (uint32_t)D.w * 2)
		fprintf(stderr, "bui: pitch %u != %d: il kernel inviera' fotogrammi interi\n", D.pitch,
			D.w * 2);
	return 0;
}

static void drm_close(void)
{
	struct drm_mode_destroy_dumb dd;

	if (D.map)
		munmap(D.map, (size_t)D.size);
	D.map = NULL;
	if (D.fd >= 0) {
		if (D.fb_id)
			xioctl(D.fd, DRM_IOCTL_MODE_RMFB, &D.fb_id);
		if (D.handle) {
			dd.handle = D.handle;
			xioctl(D.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		}
		/* chiudendo l'ultimo descrittore il kernel rimette la console */
		close(D.fd);
	}
	D.fd = -1;
	D.fb_id = D.handle = 0;
}

/* ------------------------------------------------------------------ */
/* ingresso                                                           */
/* ------------------------------------------------------------------ */

#define BITS(n) (((n) + 8 * (int)sizeof(long) - 1) / (8 * (int)sizeof(long)))
static int test_bit(const unsigned long *b, int n)
{
	return (int)((b[n / (8 * (int)sizeof(long))] >> (n % (8 * (int)sizeof(long)))) & 1);
}

static void touch_emit(int type, int x, int y, uint32_t t, void *user)
{
	(void)user;
	if (bui__suspended())
		bui__input_filter(type, NULL, 1);
	else
		bui_comp_pointer(type, x, y, t);
}

static void key_emit(const bui_key *k)
{
	if (bui__suspended())
		bui__input_filter(BUI_IN_KEY, k, 1);
	else
		bui_comp_key(k);
}

static void repeat_fire(void *user);

static void repeat_arm(void)
{
	int d;

	if (D.rep_timer)
		bui_timer_del(D.rep_timer);
	D.rep_timer = 0;
	if (!D.kbd.rep_code || bui__suspended())
		return;
	d = (int32_t)(D.kbd.rep_due - bui_now());
	D.rep_timer = bui_timer_add(d > 0 ? d : 0, 0, repeat_fire, NULL);
}

static void repeat_fire(void *user)
{
	bui_key k;

	(void)user;
	D.rep_timer = 0;
	if (bui_kbd_repeat(&D.kbd, bui_now(), &k))
		key_emit(&k);
	repeat_arm();
}

static void dev_readable(int fd, void *user)
{
	struct dev *d = user;
	struct input_event ev[64];
	ssize_t n;
	int i;

	for (;;) {
		n = read(fd, ev, sizeof(ev));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		for (i = 0; i < (int)(n / (ssize_t)sizeof(ev[0])); i++) {
			uint32_t et;

			if (!d->mono) {
				uint32_t now = bui_now();

				ev[i].input_event_sec = now / 1000;
				ev[i].input_event_usec = (now % 1000) * 1000;
			}
			et = (uint32_t)ev[i].input_event_sec * 1000u + (uint32_t)(ev[i].input_event_usec / 1000);
			if (bui__input_stale(et)) {
				/* accodato prima del risveglio: il tocco riparte da zero */
				if (d->kind == DEV_TOUCH)
					bui_touch_reset(&d->touch);
				continue;
			}
			if (d->kind == DEV_TOUCH) {
				bui_touch_event(&d->touch, &ev[i]);
			} else if (ev[i].type == EV_KEY) {
				bui_key k;
				uint32_t t = (uint32_t)ev[i].input_event_sec * 1000u +
					     (uint32_t)(ev[i].input_event_usec / 1000);

				if (bui_kbd_event(&D.kbd, ev[i].code, ev[i].value, t, &k))
					key_emit(&k);
				repeat_arm();
			}
		}
	}
	if (d->kind == DEV_TOUCH)
		bui_touch_flush(&d->touch);
	if (n < 0 && errno == ENODEV) {
		/* dispositivo sparito (tastiera staccata) */
		bui_fd_del(fd);
		close(fd);
		d->fd = -1;
	}
}

static int read_file(const char *path, char *buf, int len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC), n;

	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, (size_t)len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return 0;
}

/* calibrazione del tocco come la usa X: Option "Calibration" "minx maxx miny maxy" */
static void touch_cal(struct dev *d, bui_touch_cal *c)
{
	struct input_absinfo ax, ay;
	char buf[4096], *s;
	const char *env;

	memset(c, 0, sizeof(*c));
	c->sw = D.w;
	c->sh = D.h;
	c->deadzone = 2;
	if (xioctl(d->fd, EVIOCGABS(ABS_X), &ax) == 0 && xioctl(d->fd, EVIOCGABS(ABS_Y), &ay) == 0) {
		c->minx = ax.minimum;
		c->maxx = ax.maximum;
		c->miny = ay.minimum;
		c->maxy = ay.maximum;
	} else {
		c->maxx = c->maxy = 4095;
	}
	if (strcmp(d->name, "mxs-lradc-ts") && !getenv("BRAINSHELL_TOUCH_CAL"))
		return;		/* un altro tocco (USB...): i suoi limiti, nessuna rotazione */
	env = getenv("BRAINSHELL_TOUCH_CAL");
	if (env) {
		bui_touch_parse_cal(env, c);
	} else if (read_file("/etc/X11/xorg.conf.d/99-calibrator.conf", buf, sizeof(buf)) == 0 &&
		   (s = strstr(buf, "\"Calibration\"")) != NULL) {
		s = strchr(s + 13, '"');
		if (s)
			bui_touch_parse_cal(s + 1, c);
	}
	env = getenv("BRAINSHELL_ROTATION");
	if (env)
		c->rot = atoi(env);
	else if (read_file("/etc/brain-rotation", buf, 16) == 0)
		c->rot = atoi(buf);
	if (c->rot < 0 || c->rot > 2)
		c->rot = 0;
	/* la rotazione scambia gli assi: lo schermo ruotato e' gia' nel modo */
}

static void dev_grab(struct dev *d, int on)
{
	if (d->fd >= 0 && xioctl(d->fd, EVIOCGRAB, (void *)(intptr_t)on) < 0 && on)
		fprintf(stderr, "bui: EVIOCGRAB %s: %s\n", d->name, strerror(errno));
}

static void dev_add(const char *path)
{
	unsigned long ev[BITS(EV_MAX + 1)], key[BITS(KEY_MAX + 1)], abs[BITS(ABS_MAX + 1)];
	struct dev *d;
	int fd, kind = 0, clk = BUI_CLOCK;

	if (D.ndev >= MAX_DEV)
		return;
	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "bui: %s: %s\n", path, strerror(errno));
		return;
	}
	d = &D.dev[D.ndev];
	memset(d, 0, sizeof(*d));
	memset(ev, 0, sizeof(ev));
	memset(key, 0, sizeof(key));
	memset(abs, 0, sizeof(abs));
	d->fd = fd;
	snprintf(d->path, sizeof(d->path), "%s", path);
	if (ioctl(fd, EVIOCGNAME(sizeof(d->name) - 1), d->name) < 0)
		d->name[0] = 0;
	ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
	if (test_bit(ev, EV_ABS) && test_bit(abs, ABS_X) && test_bit(abs, ABS_Y) &&
	    (test_bit(key, BTN_TOUCH) || test_bit(key, BTN_LEFT)))
		kind = DEV_TOUCH;
	else if (test_bit(ev, EV_KEY) && (test_bit(key, KEY_A) || test_bit(key, KEY_ENTER) ||
					  test_bit(key, KEY_ESC) || test_bit(key, KEY_POWER)))
		kind = DEV_KBD;
	if (!kind) {
		close(fd);
		return;
	}
	d->kind = kind;
	d->mono = ioctl(fd, EVIOCSCLOCKID, &clk) == 0;
	if (kind == DEV_TOUCH) {
		bui_touch_cal c;

		touch_cal(d, &c);
		bui_touch_init(&d->touch, &c, touch_emit, d);
	}
	dev_grab(d, 1);
	bui_fd_add(fd, dev_readable, d);
	D.ndev++;
	if (getenv("BUI_DEBUG"))
		fprintf(stderr, "bui: %s \"%s\": %s\n", path, d->name, kind == DEV_TOUCH ? "tocco" : "tastiera");
}

static int wanted_name(const char *n)
{
	return !strcmp(n, "mxs-lradc-ts") || !strcmp(n, "brain-kbd-gpio") || !strcmp(n, "brain-kbd-i2c") ||
	       !strcmp(n, "Brain Power Button");
}

static void input_open(void)
{
	const char *list = getenv("BRAINSHELL_EVDEV");	/* "/dev/input/event0,/dev/input/event2" */
	char named[MAX_DEV][32], other[16][32], buf[96], p[300];
	int nn = 0, no = 0, i;
	DIR *dir;
	struct dirent *e;

	if (list && *list) {
		char tmp[256], *s, *save = NULL;

		snprintf(tmp, sizeof(tmp), "%s", list);
		for (s = strtok_r(tmp, ",", &save); s; s = strtok_r(NULL, ",", &save))
			dev_add(s);
		return;
	}
	dir = opendir("/sys/class/input");
	if (!dir)
		return;
	while ((e = readdir(dir)) != NULL) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/sys/class/input/%s/device/name", e->d_name);
		if (read_file(p, buf, sizeof(buf)) < 0)
			continue;
		buf[strcspn(buf, "\n")] = 0;
		if (wanted_name(buf) && nn < MAX_DEV)
			snprintf(named[nn++], 32, "/dev/input/%.16s", e->d_name);
		else if (no < 16)
			snprintf(other[no++], 32, "/dev/input/%.16s", e->d_name);
	}
	closedir(dir);
	/* prima i dispositivi del Brain; senza, qualunque tocco o tastiera */
	for (i = 0; i < nn; i++)
		dev_add(named[i]);
	if (!D.ndev)
		for (i = 0; i < no; i++)
			dev_add(other[i]);
	if (!D.ndev)
		fprintf(stderr, "bui: nessun dispositivo di ingresso\n");
}

static void input_close(void)
{
	int i;

	for (i = 0; i < D.ndev; i++)
		if (D.dev[i].fd >= 0) {
			bui_fd_del(D.dev[i].fd);
			close(D.dev[i].fd);
			D.dev[i].fd = -1;
		}
	D.ndev = 0;
}

/* stato dei modificatori dopo un periodo in cui i tasti andavano altrove */
static void kbd_resync(void)
{
	unsigned long key[BITS(KEY_MAX + 1)];
	int i;

	bui_kbd_reset(&D.kbd);
	for (i = 0; i < D.ndev; i++) {
		static const int mods[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL,
					    KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT };
		bui_key dummy;
		unsigned j;

		if (D.dev[i].fd < 0 || D.dev[i].kind != DEV_KBD)
			continue;
		memset(key, 0, sizeof(key));
		if (ioctl(D.dev[i].fd, EVIOCGKEY(sizeof(key)), key) < 0)
			continue;
		for (j = 0; j < sizeof(mods) / sizeof(mods[0]); j++)
			if (test_bit(key, mods[j]))
				bui_kbd_event(&D.kbd, mods[j], 1, 0, &dummy);
	}
}

/* ------------------------------------------------------------------ */
/* backend                                                            */
/* ------------------------------------------------------------------ */

static int b_init(void)
{
	D.trace = getenv("BUI_TRACE") != NULL;
	if (drm_open() < 0) {
		drm_close();
		return -1;
	}
	bui_comp_init(D.w, D.h, D.map, (int)(D.pitch / 2), present);
	input_open();
	return 0;
}

static void b_shutdown(void)
{
	bui_timer_del(D.rep_timer);
	D.rep_timer = 0;
	bui_comp_shutdown();
	input_close();
	drm_close();
}

static int b_flush(uint32_t now)
{
	return bui_comp_flush(now);
}

static int b_suspend(int on)
{
	int i;

	if (on) {
		bui_comp_cancel_pointer();
		bui_timer_del(D.rep_timer);
		D.rep_timer = 0;
		D.kbd.rep_code = 0;
		for (i = 0; i < D.ndev; i++) {
			dev_grab(&D.dev[i], 0);
			if (D.dev[i].kind == DEV_TOUCH)
				bui_touch_reset(&D.dev[i].touch);
		}
		if (D.master && xioctl(D.fd, DRM_IOCTL_DROP_MASTER, NULL) < 0)
			fprintf(stderr, "bui: DROP_MASTER: %s\n", strerror(errno));
		D.master = 0;
		return 0;
	}
	/* stesso descrittore che era master: SET_MASTER e' permesso anche senza root */
	if (xioctl(D.fd, DRM_IOCTL_SET_MASTER, NULL) < 0) {
		fprintf(stderr, "bui: SET_MASTER: %s\n", strerror(errno));
		return -1;
	}
	D.master = 1;
	D.need_modeset = 1;
	for (i = 0; i < D.ndev; i++) {
		dev_grab(&D.dev[i], 1);
		if (D.dev[i].kind == DEV_TOUCH)
			bui_touch_reset(&D.dev[i].touch);
	}
	kbd_resync();
	bui_comp_damage_screen(bui_r(0, 0, D.w, D.h));
	return 0;
}

const bui_backend bui_be_drm = {
	.name = "drm",
	.init = b_init,
	.shutdown = b_shutdown,
	.screen_w = bui_comp_screen_w,
	.screen_h = bui_comp_screen_h,
	.win_create = bui_comp_win_create,
	.win_destroy = bui_comp_win_destroy,
	.win_show = bui_comp_win_show,
	.win_hide = bui_comp_win_hide,
	.win_move_resize = bui_comp_win_move_resize,
	.win_set_strut_top = bui_comp_win_set_strut_top,
	.popup_open = bui_comp_popup_open,
	.popup_open_round = bui_comp_popup_open_round,
	.flush = b_flush,
	.suspend = b_suspend,
	.redraw_all = bui_comp_redraw_all,
	.win_present = bui_comp_win_present,
};
