/* SPDX-License-Identifier: MIT */
/*
 * brainplay - native media player for BrainDesk (SHARP Brain PW-SH5).
 *
 * Threads:
 *   reader  av_read_frame() -> packet queues (also performs the seeks)
 *   video   packets -> frames -> frame queue (decides what to skip when late)
 *   audio   packets -> S16 stereo -> audio output; owns the master clock
 *   main    X events, control bar, slave protocol, shows the frames on time
 */
#ifndef BRAINPLAY_H
#define BRAINPLAY_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#define BRAINPLAY_VERSION "1.0"

/* ------------------------------------------------------------- util.c --- */

enum { LOG_ERR, LOG_INFO, LOG_VERB, LOG_DBG };

extern int g_loglevel;
void logmsg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define err(...)  logmsg(LOG_ERR, __VA_ARGS__)
#define info(...) logmsg(LOG_INFO, __VA_ARGS__)
#define verb(...) logmsg(LOG_VERB, __VA_ARGS__)
#define dbg(...)  do { if (g_loglevel >= LOG_DBG) logmsg(LOG_DBG, __VA_ARGS__); } while (0)

double now_sec(void);			/* CLOCK_MONOTONIC, seconds */
void sleep_sec(double s);
/* absolute CLOCK_MONOTONIC deadline for pthread_cond_timedwait */
void deadline_after(struct timespec *ts, double s);
void cond_init_monotonic(pthread_cond_t *c);
/* "1:02:03" or "2:03"; buf at least 16 bytes */
const char *fmt_time(char *buf, double sec, bool hours);

/* wake-up of the main loop from other threads / signal handlers */
extern int g_wake_fd;
void wake_main(void);

/* ----------------------------------------------------------- options --- */

struct options {
	const char *file;
	bool slave, no_window, keep_open, start_paused;
	bool no_audio, no_video;
	int disp_w, disp_h;	/* size of the video area, to choose lowres */
	bool stats, benchmark;
	bool framedrop;		/* default on */
	bool float_vorbis;	/* debug: FFmpeg's Vorbis decoder instead of Tremor */
	bool debug_sync;
	bool first_frame_exit;	/* debug: quit when the first frame is on screen */
	const char *vo;		/* auto | pxp | pxp-shm | x11 | null */
	const char *ao;		/* alsa | null */
	const char *audio_device;
	const char *pxp_sim;	/* NULL, "new" or "legacy" (test builds only) */
	int volume;		/* 0..100 */
	int lowres;		/* -1 = automatic */
	int min_level;		/* lowest speed-up level (0..3) */
	int win_w, win_h;	/* 0 = full screen */
	double start;		/* seconds */
	double bar_timeout;	/* seconds, 0 = control bar always visible */
	double decode_delay;	/* debug: sleep per decoded video frame (seconds) */
	double max_time;	/* debug: quit after this many seconds (0 = off) */
};

/* ------------------------------------------------------------ queues --- */

struct pktnode;

struct pktq {
	pthread_mutex_t mu;
	pthread_cond_t cond;
	struct pktnode *first, *last;
	int nb;
	int bytes;
	int64_t duration;	/* in stream time base */
	unsigned kick;		/* wake-up counter */
};

#define FRAMEQ_SIZE 3

struct vframe {
	AVFrame *f;
	double pts;		/* seconds, NAN if unknown */
	double dur;		/* seconds */
	int serial;
};

struct frameq {
	pthread_mutex_t mu;
	pthread_cond_t cond;
	struct vframe q[FRAMEQ_SIZE];
	int r, n;
};

/* ------------------------------------------------------------- audio --- */

struct ao;

struct ao *ao_open(const char *driver, const char *device, int rate, const atomic_bool *abort_flag);
int ao_rate(const struct ao *ao);
const char *ao_name(const struct ao *ao);
/* blocking write of interleaved S16 stereo frames; <0 on fatal error */
int ao_write(struct ao *ao, const int16_t *data, int frames);
/* seconds of audio written but not heard yet */
double ao_delay(struct ao *ao);
void ao_pause(struct ao *ao);	/* silence now, keep what was not played */
void ao_resume(struct ao *ao);	/* ... and queue it again */
void ao_flush(struct ao *ao);	/* forget everything (seek) */
void ao_drain(struct ao *ao);	/* wait until everything was played */
int ao_underruns(const struct ao *ao);
void ao_close(struct ao *ao);

/* ------------------------------------------------------------ player --- */

struct stats {
	atomic_int v_decoded;		/* frames out of the decoder */
	atomic_int v_shown;
	atomic_int v_drop_late;		/* decoded but too late to be shown */
	atomic_int v_skip_pkts;		/* packets skipped up to a key frame */
	atomic_int v_catchups;		/* number of skips to the next key frame */
	atomic_int v_level_max;
	atomic_int a_frames;
	/* main thread only */
	double sync_sum, sync_abs_max;
	int sync_n;
	double first_frame_at;		/* seconds since process start */
};

enum { ST_PLAYING, ST_PAUSED, ST_ENDED };

struct player {
	struct options opt;

	AVFormatContext *ic;
	AVStream *vst, *ast;
	AVCodecContext *vdec, *adec;
	struct tremor *tremor;	/* decodes the audio instead of adec (Vorbis) */
	double duration;	/* seconds, 0 = unknown */
	double start_time;	/* pts of position 0 */
	char title[256], artist[128], album[128];
	char vinfo[64], ainfo[64];

	struct pktq vq, aq;
	struct frameq fq;
	pthread_t reader_th, video_th, audio_th;
	bool reader_run, video_run, audio_run;

	/* control state, protected by mu; cond is broadcast on every change */
	pthread_mutex_t mu;
	pthread_cond_t cond;
	pthread_cond_t reader_cond;
	bool reader_waiting;
	atomic_bool abort;	/* set once, under mu; read anywhere */
	bool paused;
	bool ended;		/* played to the end (keep-open) */
	bool seek_req;
	bool seeking;		/* the reader is carrying out a seek */
	double seek_target;	/* position, seconds */
	int seek_dir;		/* -1 backwards, +1 forwards, 0 absolute */
	double seek_from;	/* position when a relative seek was asked */
	bool start_known;	/* start_time is valid */
	bool demux_eof;
	bool audio_done;	/* no audio, or everything was played */
	bool video_dec_done;	/* no video, or decoder drained */

	/* master clock (mu): position pts that advances with real time */
	bool clk_started;
	double clk_pts, clk_at;
	bool clk_audio;		/* the audio output is driving it */
	/* start of playback after open / seek */
	int shown_serial;	/* serial whose first frame is on screen */
	double start_pts;	/* pts of that frame */
	double seek_pos;	/* position to report until the clock restarts */

	atomic_int serial;	/* bumped by every seek */
	atomic_int volume;	/* 0..100 */

	/* frame dropping (video thread; level read by others for the stats) */
	atomic_int level;
	atomic_int lowres;
	double frame_dur;	/* nominal, seconds */
	int vidx, aidx;

	struct stats st;
};

int player_open(struct player *p, const struct options *opt);
int player_start(struct player *p);
void player_stop(struct player *p);	/* joins the threads */
void player_close(struct player *p);
int player_benchmark(struct player *p);

void player_set_paused(struct player *p, bool paused);
void player_toggle(struct player *p);
void player_seek(struct player *p, double pos, int dir);
void player_seek_rel(struct player *p, double delta);
void player_set_volume(struct player *p, int vol);
int player_state(struct player *p);
double player_pos(struct player *p);	/* seconds from the beginning */
/* master clock as a pts; NAN while it is not running yet */
double player_clock(struct player *p, double now);
bool player_finished(struct player *p);
void player_print_stats(struct player *p);

/* queues (player.c), used by audio.c too */
int pktq_get(struct player *p, struct pktq *q, AVPacket *pkt, int *serial, unsigned *kick);
void pktq_wake(struct pktq *q);
struct vframe *frameq_peek(struct frameq *fq, int idx);
void frameq_pop(struct player *p, struct frameq *fq);

/* clock helpers (player.c); call with p->mu held */
void clock_set_locked(struct player *p, double pts, double at, bool audio);
double clock_get_locked(struct player *p, double now);

/* audio.c */
void *audio_thread(void *arg);

/* tremor.c: integer Vorbis decoder, used like avcodec send/receive */
struct tremor *tremor_open(const AVCodecParameters *par);
void tremor_close(struct tremor *t);
void tremor_reset(struct tremor *t);
int tremor_send(struct tremor *t, const AVPacket *pkt);
int tremor_receive(struct tremor *t, AVFrame *f);
int tremor_rate(const struct tremor *t);
int tremor_channels(const struct tremor *t);

/* ------------------------------------------------------------- slave --- */

void slave_init(struct player *p);
/* reads pending commands from stdin; false when the player has to quit */
bool slave_read(struct player *p);
/* prints what changed; returns the time of the next status line */
double slave_tick(struct player *p, double now, bool force);
int slave_fd(void);

#endif
