// SPDX-License-Identifier: MIT
/*
 * brainplay: options, start-up order and the main loop.
 *
 * The main thread owns the X connection: it handles the events, draws the
 * control bar, talks the slave protocol and shows every video frame when the
 * master clock reaches its timestamp. It sleeps in ppoll() until the next
 * frame or event: nothing runs while the player is paused.
 */
#include "brainplay.h"
#include "ui.h"
#include "vo.h"
#include "win.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct app {
	struct options opt;
	struct player p;
	struct win win;
	bool have_win;
	struct vo *vo;
	struct ui *ui;
	double t_main;		/* now_sec() at the start of main() */
	double age_main;	/* age of the process at that moment */
	double clock_deadline;	/* start the clock without the sound after this */
	double last_pts;
};

static volatile sig_atomic_t got_signal;

static void on_signal(int sig)
{
	got_signal = sig;
	wake_main();
}

/* seconds since the kernel started this process (covers the dynamic linking) */
static double process_age(void)
{
	struct timespec ts;
	char buf[512], *s;
	unsigned long long start = 0;
	long hz = sysconf(_SC_CLK_TCK);
	FILE *f = fopen("/proc/self/stat", "r");
	int i;

	if (!f)
		return 0;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	s = strrchr(buf, ')');	/* the command name may contain anything */
	if (!s)
		return 0;
	/* field 22 is the start time; s points at the end of field 2 */
	for (i = 0; i < 20 && s; i++)
		s = strchr(s + 1, ' ');
	if (!s || sscanf(s, " %llu", &start) != 1 || hz <= 0)
		return 0;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9 - (double)start / hz;
}

static void usage(FILE *f)
{
	fprintf(f,
		"brainplay " BRAINPLAY_VERSION " - media player of BrainDesk\n"
		"usage: brainplay [options] FILE\n"
		"\n"
		"  --slave            remote control: commands on stdin, status on stdout\n"
		"  --no-window        no window: sound only\n"
		"  --volume N         software volume 0-100 (default 100)\n"
		"  --start SEC        start at this position\n"
		"  --paused           start paused\n"
		"  --keep-open        stay open at the end of the file\n"
		"  --bar-timeout SEC  hide the control bar after SEC seconds of video\n"
		"                     (default 4, 0 = always visible)\n"
		"  --window WxH       a normal window instead of the full screen\n"
		"  --vo NAME          auto (default), pxp, pxp-shm, x11, null\n"
		"  --ao NAME          alsa (default), null (silent, real time)\n"
		"  --audio-device DEV ALSA device (default \"default\")\n"
		"  --no-audio, --no-video\n"
		"  --no-framedrop     never skip pictures to stay with the sound\n"
		"  --min-level N      always decode with speed-up level N (0-3):\n"
		"                     1 = no loop filter on non-reference frames,\n"
		"                     2 = no loop filter, 3 = skip non-reference frames\n"
		"  --lowres N         decode at 1/2^N of the size (default: automatic)\n"
		"  --benchmark        decode as fast as possible and report the speed\n"
		"  --stats            print playback statistics at the end\n"
		"  -v, --verbose      more messages (twice: debug)\n"
		"  -q, --quiet        errors only\n"
		"  -h, --help\n"
		"\n"
		"keys: Space play/pause, Left/Right 10 s, PgUp/PgDn 60 s, Up/Down volume,\n"
		"      q or Esc quit. Touch: tap the picture to play/pause.\n"
		"slave commands: play, pause, toggle, seek <s>, seek +<s>, seek -<s>,\n"
		"      volume <0-100>, status, quit\n");
}

enum {
	O_SLAVE = 256, O_NO_WINDOW, O_VOLUME, O_START, O_PAUSED, O_KEEP_OPEN, O_BAR_TIMEOUT,
	O_WINDOW, O_VO, O_AO, O_AUDIO_DEVICE, O_NO_AUDIO, O_NO_VIDEO, O_NO_FRAMEDROP,
	O_MIN_LEVEL, O_LOWRES, O_BENCHMARK, O_STATS, O_DEBUG_SYNC, O_DEBUG_DELAY, O_PXP_SIM,
	O_MAX_TIME, O_VERSION, O_FLOAT_VORBIS, O_FIRST_FRAME_EXIT,
};

static const struct option long_opts[] = {
	{ "slave", no_argument, NULL, O_SLAVE },
	{ "no-window", no_argument, NULL, O_NO_WINDOW },
	{ "volume", required_argument, NULL, O_VOLUME },
	{ "start", required_argument, NULL, O_START },
	{ "paused", no_argument, NULL, O_PAUSED },
	{ "keep-open", no_argument, NULL, O_KEEP_OPEN },
	{ "bar-timeout", required_argument, NULL, O_BAR_TIMEOUT },
	{ "window", required_argument, NULL, O_WINDOW },
	{ "vo", required_argument, NULL, O_VO },
	{ "ao", required_argument, NULL, O_AO },
	{ "audio-device", required_argument, NULL, O_AUDIO_DEVICE },
	{ "no-audio", no_argument, NULL, O_NO_AUDIO },
	{ "no-video", no_argument, NULL, O_NO_VIDEO },
	{ "no-framedrop", no_argument, NULL, O_NO_FRAMEDROP },
	{ "min-level", required_argument, NULL, O_MIN_LEVEL },
	{ "lowres", required_argument, NULL, O_LOWRES },
	{ "benchmark", no_argument, NULL, O_BENCHMARK },
	{ "stats", no_argument, NULL, O_STATS },
	{ "verbose", no_argument, NULL, 'v' },
	{ "quiet", no_argument, NULL, 'q' },
	{ "help", no_argument, NULL, 'h' },
	{ "version", no_argument, NULL, O_VERSION },
	/* for the tests */
	{ "debug-sync", no_argument, NULL, O_DEBUG_SYNC },
	{ "debug-decode-delay", required_argument, NULL, O_DEBUG_DELAY },
	{ "max-time", required_argument, NULL, O_MAX_TIME },
	{ "pxp-sim", optional_argument, NULL, O_PXP_SIM },
	{ "float-vorbis", no_argument, NULL, O_FLOAT_VORBIS },
	{ "debug-first-frame-exit", no_argument, NULL, O_FIRST_FRAME_EXIT },
	{ NULL, 0, NULL, 0 },
};

static int parse_options(struct options *o, int argc, char **argv)
{
	int c;

	memset(o, 0, sizeof(*o));
	o->vo = "auto";
	o->ao = "alsa";
	o->audio_device = "default";
	o->volume = 100;
	o->lowres = -1;
	o->framedrop = true;
	o->bar_timeout = 4;

	while ((c = getopt_long(argc, argv, "vqh", long_opts, NULL)) != -1) {
		switch (c) {
		case O_SLAVE:
			o->slave = true;
			break;
		case O_NO_WINDOW:
			o->no_window = true;
			break;
		case O_VOLUME:
			o->volume = atoi(optarg);
			o->volume = o->volume < 0 ? 0 : o->volume > 100 ? 100 : o->volume;
			break;
		case O_START:
			o->start = atof(optarg);
			break;
		case O_PAUSED:
			o->start_paused = true;
			break;
		case O_KEEP_OPEN:
			o->keep_open = true;
			break;
		case O_BAR_TIMEOUT:
			o->bar_timeout = atof(optarg);
			break;
		case O_WINDOW:
			if (sscanf(optarg, "%dx%d", &o->win_w, &o->win_h) != 2 ||
			    o->win_w < 64 || o->win_h < 64) {
				fprintf(stderr, "brainplay: bad window size: %s\n", optarg);
				return -1;
			}
			break;
		case O_VO:
			if (strcmp(optarg, "auto") && strcmp(optarg, "pxp") && strcmp(optarg, "pxp-shm") &&
			    strcmp(optarg, "x11") && strcmp(optarg, "null")) {
				fprintf(stderr, "brainplay: unknown video output: %s\n", optarg);
				return -1;
			}
			o->vo = optarg;
			break;
		case O_AO:
			if (strcmp(optarg, "alsa") && strcmp(optarg, "null")) {
				fprintf(stderr, "brainplay: unknown audio output: %s\n", optarg);
				return -1;
			}
			o->ao = optarg;
			break;
		case O_AUDIO_DEVICE:
			o->audio_device = optarg;
			break;
		case O_NO_AUDIO:
			o->no_audio = true;
			break;
		case O_NO_VIDEO:
			o->no_video = true;
			break;
		case O_NO_FRAMEDROP:
			o->framedrop = false;
			break;
		case O_MIN_LEVEL:
			o->min_level = atoi(optarg);
			o->min_level = o->min_level < 0 ? 0 : o->min_level > 3 ? 3 : o->min_level;
			break;
		case O_LOWRES:
			o->lowres = atoi(optarg);
			o->lowres = o->lowres < 0 ? -1 : o->lowres > 3 ? 3 : o->lowres;
			break;
		case O_BENCHMARK:
			o->benchmark = true;
			break;
		case O_STATS:
			o->stats = true;
			break;
		case O_DEBUG_SYNC:
			o->debug_sync = true;
			break;
		case O_DEBUG_DELAY:
			o->decode_delay = atof(optarg) / 1000;
			break;
		case O_MAX_TIME:
			o->max_time = atof(optarg);
			break;
		case O_FLOAT_VORBIS:
			o->float_vorbis = true;
			break;
		case O_FIRST_FRAME_EXIT:
			o->first_frame_exit = true;
			break;
		case O_PXP_SIM:
#ifdef PXP_SIM
			o->pxp_sim = optarg ? optarg : "new";
			break;
#else
			fprintf(stderr, "brainplay: built without the PXP simulator\n");
			return -1;
#endif
		case 'v':
			g_loglevel++;
			break;
		case 'q':
			g_loglevel = LOG_ERR;
			break;
		case 'h':
			usage(stdout);
			exit(0);
		case O_VERSION:
			printf("brainplay " BRAINPLAY_VERSION " (%s)\n", LIBAVCODEC_IDENT);
			exit(0);
		default:
			return -1;
		}
	}
	if (optind != argc - 1) {
		usage(stderr);
		return -1;
	}
	o->file = argv[optind];
	return 0;
}

static void show_frame(struct app *a, struct vframe *vf, double diff)
{
	struct player *p = &a->p;
	int r = VO_OK;

	if (a->vo) {
		AVRational sar = av_guess_sample_aspect_ratio(p->ic, p->vst, vf->f);

		r = vo_draw(a->vo, vf->f, sar);
	}
	if (r == VO_DROPPED) {
		atomic_fetch_add(&p->st.v_drop_late, 1);
		return;
	}
	if (r < 0)
		return;		/* the output said why */
	if (!atomic_fetch_add(&p->st.v_shown, 1)) {
		p->st.first_frame_at = a->age_main + (now_sec() - a->t_main);
		verb("first frame %.0f ms after the start of the process (%.0f ms after main)",
		     p->st.first_frame_at * 1e3, (now_sec() - a->t_main) * 1e3);
	}
	if (!isnan(diff)) {
		p->st.sync_sum += diff;
		p->st.sync_n++;
		if (fabs(diff) > p->st.sync_abs_max)
			p->st.sync_abs_max = fabs(diff);
	}
	if (p->opt.debug_sync)
		fprintf(stderr, "sync pts=%.3f late=%+.1fms level=%d\n", vf->pts,
			isnan(diff) ? 0.0 : diff * 1e3, atomic_load(&p->level));
}

/*
 * Shows the frames that are due. Returns the time at which the next one is.
 */
static double video_refresh(struct app *a, double now)
{
	struct player *p = &a->p;

	for (;;) {
		struct vframe *vf = frameq_peek(&p->fq, 0), *next;
		int serial = atomic_load(&p->serial);
		bool first, paused;
		double clk, diff;

		if (!vf)
			return now + 3600;
		if (vf->serial != serial) {
			frameq_pop(p, &p->fq);
			continue;
		}
		if (isnan(vf->pts))
			vf->pts = isnan(a->last_pts) ? p->start_time + p->seek_pos : a->last_pts + vf->dur;

		pthread_mutex_lock(&p->mu);
		first = p->shown_serial != serial;
		paused = p->paused;
		pthread_mutex_unlock(&p->mu);

		if (first) {
			/*
			 * First picture after the start or a seek: on screen at
			 * once, whatever the clock says. The sound starts from here.
			 */
			if (a->vo && !a->win.mapped && now - a->t_main < 1.0)
				return a->t_main + 1.0;		/* MapNotify wakes us up */
			show_frame(a, vf, NAN);
			now = now_sec();
			pthread_mutex_lock(&p->mu);
			if (atomic_load(&p->serial) == serial) {
				p->shown_serial = serial;
				p->start_pts = vf->pts;
				if (!p->clk_started && (!p->ast || p->audio_done))
					clock_set_locked(p, vf->pts, now, false);
				pthread_cond_broadcast(&p->cond);
			}
			pthread_mutex_unlock(&p->mu);
			a->clock_deadline = now + 1.5;
			a->last_pts = vf->pts;
			frameq_pop(p, &p->fq);
			continue;
		}
		if (paused)
			return now + 3600;

		clk = player_clock(p, now);
		if (isnan(clk)) {
			/* the sound has not started yet: do not wait for ever */
			if (now < a->clock_deadline)
				return a->clock_deadline;
			pthread_mutex_lock(&p->mu);
			if (!p->clk_started)
				clock_set_locked(p, p->start_pts, now, false);
			pthread_mutex_unlock(&p->mu);
			continue;
		}
		diff = clk - vf->pts;		/* > 0: the frame is late */
		if (diff < -10 && !p->ast) {
			/* a jump in the timestamps of a silent file: follow it */
			pthread_mutex_lock(&p->mu);
			clock_set_locked(p, vf->pts, now, false);
			pthread_mutex_unlock(&p->mu);
			diff = 0;
		}
		if (diff < -0.002)
			return now + (-diff > 1 ? 1 : -diff);

		/* late, and the next one is due as well: this one is not worth showing */
		next = frameq_peek(&p->fq, 1);
		if (p->opt.framedrop && next && next->serial == serial && !isnan(next->pts) &&
		    next->pts <= clk) {
			atomic_fetch_add(&p->st.v_drop_late, 1);
			a->last_pts = vf->pts;
			frameq_pop(p, &p->fq);
			continue;
		}
		show_frame(a, vf, diff);
		a->last_pts = vf->pts;
		frameq_pop(p, &p->fq);
		now = now_sec();
	}
}

int main(int argc, char **argv)
{
	static struct app app;
	struct app *a = &app;
	struct player *p = &a->p;
	struct sigaction sa = { .sa_handler = on_signal };
	const char *base;
	bool quit = false;
	int ret = 0;

	a->t_main = now_sec();
	a->age_main = process_age();
	a->last_pts = NAN;
	if (parse_options(&a->opt, argc, argv) < 0)
		return 2;

	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	/*
	 * The window first: the X server and the window manager set it up
	 * while this process opens the file and the decoders.
	 */
	base = strrchr(a->opt.file, '/');
	base = base ? base + 1 : a->opt.file;
	if (!a->opt.no_window && !a->opt.benchmark) {
		if (win_open(&a->win, &a->opt, base) == 0) {
			a->have_win = true;
			a->opt.disp_w = a->win.w;
			a->opt.disp_h = a->win.h;
		} else {
			info("no X display: sound only");
		}
	}
	if (!a->have_win && !a->opt.benchmark)
		a->opt.no_video = true;		/* nowhere to show it: do not decode it */

	if (player_open(p, &a->opt) < 0) {
		if (a->opt.slave)
			printf("error cannot play %s\n", a->opt.file);
		ret = 1;
		goto out;
	}
	if (a->opt.benchmark) {
		ret = player_benchmark(p) < 0;
		goto out;
	}
	if (a->have_win) {
		win_set_title(&a->win, p->title);
		if (p->vst) {
			a->vo = vo_open(a->opt.vo, &a->win, &a->opt);
			if (!a->vo && strcmp(a->opt.vo, "null"))
				err("no video output: the picture will not be shown");
		}
		a->ui = ui_new(&a->win, p, &a->opt);
		if (a->ui)
			ui_set_vo(a->ui, a->vo);
	}
	if (a->opt.slave)
		slave_init(p);
	if (player_start(p) < 0) {
		err("cannot start the threads");
		ret = 1;
		quit = true;
	}

	while (!quit && !got_signal) {
		struct pollfd fds[3];
		struct timespec ts;
		double now = now_sec(), next = now + 3600, d;
		int n = 0, xi = -1, si = -1;

		if (a->have_win) {
			while (XPending(a->win.dpy)) {
				XEvent ev;

				XNextEvent(a->win.dpy, &ev);
				if (a->ui && !ui_event(a->ui, &ev))
					quit = true;
			}
			if (quit)
				break;
		}

		if (player_finished(p)) {
			bool was_ended;

			pthread_mutex_lock(&p->mu);
			was_ended = p->ended;
			if (!was_ended) {
				/* stop the clock at the end */
				if (p->clk_started)
					p->clk_pts = clock_get_locked(p, now);
				p->ended = true;
				p->paused = true;
				pthread_cond_broadcast(&p->cond);
			}
			pthread_mutex_unlock(&p->mu);
			if (!a->opt.keep_open) {
				if (a->opt.slave)
					slave_tick(p, now, false);
				break;
			}
		}
		if (p->vst) {
			d = video_refresh(a, now);
			if (d < next)
				next = d;
		}
		if (a->ui) {
			d = ui_tick(a->ui, now_sec());
			if (d < next)
				next = d;
		}
		if (a->opt.slave) {
			d = slave_tick(p, now_sec(), false);
			if (d < next)
				next = d;
		}
		if (a->opt.first_frame_exit && atomic_load(&p->st.v_shown)) {
			if (a->have_win)
				XSync(a->win.dpy, False);	/* really on screen */
			break;
		}
		if (a->opt.max_time > 0) {
			if (now - a->t_main >= a->opt.max_time)
				break;
			if (a->t_main + a->opt.max_time < next)
				next = a->t_main + a->opt.max_time;
		}

		if (a->have_win) {
			XFlush(a->win.dpy);
			if (XEventsQueued(a->win.dpy, QueuedAlready))
				continue;
			xi = n;
			fds[n++] = (struct pollfd){ ConnectionNumber(a->win.dpy), POLLIN, 0 };
		}
		fds[n++] = (struct pollfd){ g_wake_fd, POLLIN, 0 };
		if (a->opt.slave && slave_fd() >= 0) {
			si = n;
			fds[n++] = (struct pollfd){ slave_fd(), POLLIN, 0 };
		}
		d = next - now_sec();
		if (d < 0)
			d = 0;
		ts.tv_sec = (time_t)d;
		ts.tv_nsec = (long)((d - ts.tv_sec) * 1e9);
		if (ppoll(fds, n, &ts, NULL) > 0) {
			uint64_t v;

			if (read(g_wake_fd, &v, sizeof(v)) < 0) {
			}
			if (si >= 0 && fds[si].revents && !slave_read(p))
				quit = true;
			if (xi >= 0 && (fds[xi].revents & (POLLERR | POLLHUP)))
				quit = true;
		}
	}

	player_stop(p);
	if (a->opt.stats || g_loglevel >= LOG_VERB)
		player_print_stats(p);
out:
	ui_free(a->ui);
	vo_close(a->vo);
	if (a->have_win)
		win_close(&a->win);
	player_close(p);
	if (g_wake_fd >= 0)
		close(g_wake_fd);
	return ret;
}
