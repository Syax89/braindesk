// SPDX-License-Identifier: MIT
/*
 * Demuxing, queues, the video decoder thread and its frame dropping,
 * seeking and the master clock.
 */
#include "brainplay.h"

#include <math.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/pixdesc.h>

#define MAX_QUEUE_BYTES	(4 << 20)
#define MIN_QUEUE_PKTS	25

/* ---------------------------------------------------------------- clock --- */

void clock_set_locked(struct player *p, double pts, double at, bool audio)
{
	p->clk_pts = pts;
	p->clk_at = at;
	p->clk_started = true;
	p->clk_audio = audio;
}

double clock_get_locked(struct player *p, double now)
{
	if (!p->clk_started)
		return NAN;
	return p->paused ? p->clk_pts : p->clk_pts + (now - p->clk_at);
}

double player_clock(struct player *p, double now)
{
	double v;

	pthread_mutex_lock(&p->mu);
	v = clock_get_locked(p, now);
	pthread_mutex_unlock(&p->mu);
	return v;
}

/* -------------------------------------------------------- packet queue --- */

struct pktnode {
	AVPacket *pkt;
	int serial;
	struct pktnode *next;
};

static void pktq_init(struct pktq *q)
{
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->mu, NULL);
	pthread_cond_init(&q->cond, NULL);
}

/* takes the content of pkt; pkt == NULL queues the end-of-stream marker */
static int pktq_put(struct pktq *q, AVPacket *pkt, int serial)
{
	struct pktnode *n = av_mallocz(sizeof(*n));

	if (n)
		n->pkt = av_packet_alloc();
	if (!n || !n->pkt) {
		av_free(n);
		if (pkt)
			av_packet_unref(pkt);
		return -1;
	}
	if (pkt)
		av_packet_move_ref(n->pkt, pkt);
	n->serial = serial;

	pthread_mutex_lock(&q->mu);
	if (q->last)
		q->last->next = n;
	else
		q->first = n;
	q->last = n;
	q->nb++;
	q->bytes += n->pkt->size + (int)sizeof(*n);
	q->duration += n->pkt->duration;
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mu);
	return 0;
}

/*
 * 1 = got a packet (data == NULL: end of stream), 0 = woken up without a
 * packet (look at the control state), -1 = quit.
 */
int pktq_get(struct player *p, struct pktq *q, AVPacket *pkt, int *serial, unsigned *kick)
{
	struct pktnode *n = NULL;
	int ret;

	pthread_mutex_lock(&q->mu);
	for (;;) {
		if (p->abort) {
			ret = -1;
			break;
		}
		if (q->first) {
			n = q->first;
			q->first = n->next;
			if (!q->first)
				q->last = NULL;
			q->nb--;
			q->bytes -= n->pkt->size + (int)sizeof(*n);
			q->duration -= n->pkt->duration;
			ret = 1;
			break;
		}
		if (q->kick != *kick) {
			*kick = q->kick;
			ret = 0;
			break;
		}
		pthread_cond_wait(&q->cond, &q->mu);
	}
	pthread_mutex_unlock(&q->mu);

	if (n) {
		av_packet_move_ref(pkt, n->pkt);
		*serial = n->serial;
		av_packet_free(&n->pkt);
		av_free(n);
		/* there is room again: the reader may be waiting for it */
		pthread_mutex_lock(&p->mu);
		if (p->reader_waiting)
			pthread_cond_signal(&p->reader_cond);
		pthread_mutex_unlock(&p->mu);
	}
	return ret;
}

void pktq_wake(struct pktq *q)
{
	pthread_mutex_lock(&q->mu);
	q->kick++;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->mu);
}

static void pktq_flush(struct pktq *q)
{
	struct pktnode *n, *next;

	pthread_mutex_lock(&q->mu);
	for (n = q->first; n; n = next) {
		next = n->next;
		av_packet_free(&n->pkt);
		av_free(n);
	}
	q->first = q->last = NULL;
	q->nb = q->bytes = 0;
	q->duration = 0;
	pthread_mutex_unlock(&q->mu);
}

static void pktq_stat(struct pktq *q, int *nb, int *bytes, int64_t *duration)
{
	pthread_mutex_lock(&q->mu);
	*nb = q->nb;
	*bytes = q->bytes;
	*duration = q->duration;
	pthread_mutex_unlock(&q->mu);
}

/* --------------------------------------------------------- frame queue --- */

static int frameq_init(struct frameq *fq)
{
	int i;

	memset(fq, 0, sizeof(*fq));
	pthread_mutex_init(&fq->mu, NULL);
	pthread_cond_init(&fq->cond, NULL);
	for (i = 0; i < FRAMEQ_SIZE; i++)
		if (!(fq->q[i].f = av_frame_alloc()))
			return -1;
	return 0;
}

/* blocks while the queue is full; -1 if the frame became stale meanwhile */
static int frameq_push(struct player *p, AVFrame *f, double pts, double dur, int serial,
		       double *blocked)
{
	struct frameq *fq = &p->fq;
	struct vframe *vf;
	double t0 = 0;

	pthread_mutex_lock(&fq->mu);
	while (fq->n == FRAMEQ_SIZE && !p->abort && atomic_load(&p->serial) == serial) {
		if (t0 == 0)
			t0 = now_sec();
		pthread_cond_wait(&fq->cond, &fq->mu);
	}
	if (t0 != 0)
		*blocked = now_sec() - t0;
	if (p->abort || atomic_load(&p->serial) != serial) {
		pthread_mutex_unlock(&fq->mu);
		return -1;
	}
	vf = &fq->q[(fq->r + fq->n) % FRAMEQ_SIZE];
	av_frame_unref(vf->f);
	av_frame_move_ref(vf->f, f);
	vf->pts = pts;
	vf->dur = dur;
	vf->serial = serial;
	fq->n++;
	pthread_mutex_unlock(&fq->mu);
	wake_main();
	return 0;
}

/* main thread: the idx-th queued frame, or NULL */
struct vframe *frameq_peek(struct frameq *fq, int idx)
{
	struct vframe *vf = NULL;

	pthread_mutex_lock(&fq->mu);
	if (idx < fq->n)
		vf = &fq->q[(fq->r + idx) % FRAMEQ_SIZE];
	pthread_mutex_unlock(&fq->mu);
	return vf;
}

void frameq_pop(struct player *p, struct frameq *fq)
{
	pthread_mutex_lock(&fq->mu);
	if (fq->n) {
		av_frame_unref(fq->q[fq->r].f);
		fq->r = (fq->r + 1) % FRAMEQ_SIZE;
		fq->n--;
		pthread_cond_signal(&fq->cond);
	}
	pthread_mutex_unlock(&fq->mu);
}

static void frameq_wake(struct frameq *fq)
{
	pthread_mutex_lock(&fq->mu);
	pthread_cond_broadcast(&fq->cond);
	pthread_mutex_unlock(&fq->mu);
}

/* -------------------------------------------------------------- decoders --- */

static const AVCodec *find_decoder(enum AVCodecID id)
{
	const AVCodec *c = NULL;

	/* fixed-point library decoder first, if the build has it */
	if (id == AV_CODEC_ID_OPUS)
		c = avcodec_find_decoder_by_name("libopus");
	return c ? c : avcodec_find_decoder(id);
}

static AVCodecContext *open_decoder(struct player *p, AVStream *st, int lowres)
{
	const AVCodec *codec = find_decoder(st->codecpar->codec_id);
	AVDictionary *opts = NULL;
	AVCodecContext *ctx;
	int e;

	if (!codec)
		return NULL;
	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return NULL;
	if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
		goto fail;
	ctx->pkt_timebase = st->time_base;
	ctx->thread_count = 1;			/* one core: no frame threads, no latency */
	ctx->flags2 |= AV_CODEC_FLAG2_FAST;	/* speed before exactness */
	if (codec->type == AVMEDIA_TYPE_VIDEO) {
		ctx->lowres = lowres > codec->max_lowres ? codec->max_lowres : lowres;
	} else {
		/* let AC-3 mix down inside the decoder: it is cheaper there */
		av_dict_set(&opts, "downmix", "stereo", 0);
	}
	e = avcodec_open2(ctx, codec, &opts);
	av_dict_free(&opts);
	if (e < 0) {
		err("cannot open the %s decoder: %s", codec->name, av_err2str(e));
		goto fail;
	}
	return ctx;
fail:
	avcodec_free_context(&ctx);
	return NULL;
}

/* highest lowres that still gives a picture about as big as the screen */
static int auto_lowres(const struct player *p, const AVCodec *codec, int w, int h)
{
	int dw = p->opt.disp_w > 0 ? p->opt.disp_w : 854;
	int dh = p->opt.disp_h > 0 ? p->opt.disp_h : 480;
	int target_w, l, best = 0;

	if (!codec || w <= 0 || h <= 0)
		return 0;
	/* width of the picture once fitted to the screen */
	target_w = (int64_t)w * dh <= (int64_t)h * dw ? (int)((int64_t)w * dh / h) : dw;
	for (l = 1; l <= codec->max_lowres; l++)
		if ((w >> l) * 10 >= target_w * 7)
			best = l;
	return best;
}

/* --------------------------------------------------------- frame dropping --- */

/*
 * When the decoder cannot keep up, speed it up step by step:
 *   1  no in-loop (deblocking) filter on non-reference frames   (H.264, VP8)
 *   2  no in-loop filter at all
 *   3  non-reference frames (B frames) are not decoded
 * and, if that is still not enough and the picture falls behind by more
 * than half a second, skip the packets up to the next key frame: the
 * picture freezes and jumps, but it stays with the sound.
 * Formats that still fall behind after that and have a lower-resolution
 * decoding mode (MPEG-1/2/4, H.263, MJPEG) are reopened one lowres step
 * higher at that key frame.
 */
#define LEVEL_MAX 3

enum { KEEP, CATCHUP };

struct dropctl {
	bool has_lf;		/* the codec has a loop filter that can be skipped */
	bool intra_only;
	int late_run;
	double last_raise, last_lower, hold;
	double win_start, win_blocked;
	bool win_late;
	int catchups;		/* recent skips to a key frame */
	double catchup_t0;
	double avg_dec;		/* typical time to decode a frame, seconds */
};

static void apply_level(AVCodecContext *dec, int level)
{
	dec->skip_loop_filter = level >= 2 ? AVDISCARD_ALL :
				level == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
	dec->skip_frame = level >= 3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

static void set_level(struct player *p, int level)
{
	atomic_store(&p->level, level);
	if (level > atomic_load(&p->st.v_level_max))
		atomic_store(&p->st.v_level_max, level);
	verb("speed-up level %d", level);
}

/* called for every decoded frame: is the decoder falling behind? */
static int drop_decide(struct player *p, struct dropctl *dc, double pts, double now)
{
	int level = atomic_load(&p->level);
	double fd = p->frame_dur, clk, late, emergency;

	if (!p->opt.framedrop || isnan(pts))
		return KEEP;
	pthread_mutex_lock(&p->mu);
	clk = p->paused ? NAN : clock_get_locked(p, now);
	pthread_mutex_unlock(&p->mu);
	if (isnan(clk)) {
		dc->late_run = 0;
		return KEEP;
	}
	late = clk - pts;
	if (late > -0.5 * fd)
		dc->win_late = true;
	dc->late_run = late > 0 ? dc->late_run + 1 : 0;

	emergency = dc->intra_only ? fmax(2 * fd, 0.08) : 0.5;
	if (late > emergency && (level >= LEVEL_MAX || dc->intra_only || late > 1.0)) {
		if (level < LEVEL_MAX && !dc->intra_only)
			set_level(p, LEVEL_MAX);
		dc->late_run = 0;
		return CATCHUP;
	}
	if (level < LEVEL_MAX && !dc->intra_only &&
	    (dc->late_run >= 3 || late > 0.15) && now - dc->last_raise > 0.25) {
		set_level(p, dc->has_lf ? level + 1 : LEVEL_MAX);
		/* a recent step down was a mistake: wait longer before the next one */
		if (now - dc->last_lower < 5)
			dc->hold = fmin(dc->hold * 2, 120);
		dc->last_raise = now;
		dc->late_run = 0;
	}
	return KEEP;
}

/* called after every queued frame: is there CPU to spare for a step back? */
static void drop_relax(struct player *p, struct dropctl *dc, double blocked, double now)
{
	int level = atomic_load(&p->level);
	bool paused;
	double idle;

	pthread_mutex_lock(&p->mu);
	paused = p->paused || !p->clk_started;
	pthread_mutex_unlock(&p->mu);
	if (paused) {
		dc->win_start = now;
		dc->win_blocked = 0;
		dc->win_late = false;
		return;
	}
	dc->win_blocked += blocked;
	if (now - dc->win_start < 2)
		return;
	idle = dc->win_blocked / (now - dc->win_start);
	if (level > p->opt.min_level && !dc->win_late && idle > 0.4 &&
	    now - dc->last_raise > dc->hold) {
		set_level(p, dc->has_lf ? level - 1 : p->opt.min_level);
		dc->last_lower = now;
	}
	dc->win_start = now;
	dc->win_blocked = 0;
	dc->win_late = false;
}

/* ----------------------------------------------------------- video thread --- */

static void *video_thread(void *arg)
{
	struct player *p = arg;
	const AVCodecDescriptor *desc = avcodec_descriptor_get(p->vdec->codec_id);
	struct dropctl dc = { .hold = 4 };
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	double tb = av_q2d(p->vst->time_base);
	bool skip_to_key = false;
	int want_lowres = -1;
	int serial = -1;
	unsigned kick = 0;

	/* the sound and the user interface come first on the only core */
	if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 4) < 0) {
	}

	dc.has_lf = p->vdec->codec_id == AV_CODEC_ID_H264 || p->vdec->codec_id == AV_CODEC_ID_VP8;
	dc.intra_only = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
	dc.win_start = now_sec();

	while (pkt && frame) {
		int pkt_serial, r, e;
		double t_dec;
		bool eof;

		r = pktq_get(p, &p->vq, pkt, &pkt_serial, &kick);
		if (r < 0)
			break;
		if (r == 0)
			continue;
		if (pkt_serial != atomic_load(&p->serial)) {
			av_packet_unref(pkt);
			continue;
		}
		if (pkt_serial != serial) {
			avcodec_flush_buffers(p->vdec);
			serial = pkt_serial;
			skip_to_key = false;
			dc.late_run = 0;
		}
		eof = !pkt->data;
		if (!eof && skip_to_key) {
			int64_t t = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
			double clk = player_clock(p, now_sec());
			/*
			 * A key frame that is late itself would only be thrown
			 * away. With intra-only codecs every frame is a key
			 * frame: take the first one that will still be on time
			 * once it is decoded.
			 */
			bool late_key = t != AV_NOPTS_VALUE && !isnan(clk) &&
					clk - t * tb > (dc.intra_only ? -dc.avg_dec : 0.25);

			if (!(pkt->flags & AV_PKT_FLAG_KEY) || late_key) {
				av_packet_unref(pkt);
				atomic_fetch_add(&p->st.v_skip_pkts, 1);
				continue;
			}
			skip_to_key = false;
			avcodec_flush_buffers(p->vdec);
			if (want_lowres >= 0) {
				AVCodecContext *nd = open_decoder(p, p->vst, want_lowres);

				if (nd) {
					avcodec_free_context(&p->vdec);
					p->vdec = nd;
					atomic_store(&p->lowres, nd->lowres);
					info("decoding at 1/%d of the size to keep up", 1 << nd->lowres);
				}
				want_lowres = -1;
			}
		}
		apply_level(p->vdec, atomic_load(&p->level));
		t_dec = now_sec();
		e = avcodec_send_packet(p->vdec, eof ? NULL : pkt);
		av_packet_unref(pkt);
		if (e < 0 && e != AVERROR(EAGAIN) && e != AVERROR_EOF)
			dbg("video: decoder rejected a packet: %s", av_err2str(e));

		for (;;) {
			double pts = NAN, dur = p->frame_dur, blocked = 0, now;

			e = avcodec_receive_frame(p->vdec, frame);
			if (e == AVERROR_EOF) {
				pthread_mutex_lock(&p->mu);
				if (atomic_load(&p->serial) == serial)
					p->video_dec_done = true;
				pthread_cond_broadcast(&p->cond);
				pthread_mutex_unlock(&p->mu);
				wake_main();
				break;
			}
			if (e < 0)
				break;
			atomic_fetch_add(&p->st.v_decoded, 1);
			if (p->opt.decode_delay > 0)
				sleep_sec(p->opt.decode_delay);	/* debug: a slow CPU */

			if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
				pts = frame->best_effort_timestamp * tb;
			if (frame->duration > 0)
				dur = frame->duration * tb;

			now = now_sec();
			dc.avg_dec = dc.avg_dec > 0 ? dc.avg_dec * 0.75 + (now - t_dec) * 0.25 : now - t_dec;
			t_dec = now;
			if (drop_decide(p, &dc, pts, now) == CATCHUP) {
				/* this frame and all the others up to the next key frame */
				av_frame_unref(frame);
				atomic_fetch_add(&p->st.v_drop_late, 1);
				atomic_fetch_add(&p->st.v_catchups, 1);
				skip_to_key = true;
				if (now - dc.catchup_t0 > 10) {
					dc.catchup_t0 = now;
					dc.catchups = 0;
				}
				if (++dc.catchups >= 2 && p->opt.lowres < 0 &&
				    p->vdec->lowres < p->vdec->codec->max_lowres &&
				    (p->vdec->coded_width >> (p->vdec->lowres + 1)) >= 160) {
					want_lowres = p->vdec->lowres + 1;
					dc.catchups = 0;
				}
				verb("video is %.0f ms late: skipping to the next key frame",
				     (player_clock(p, now) - pts) * 1e3);
				break;
			}
			if (frameq_push(p, frame, pts, dur, serial, &blocked) < 0) {
				av_frame_unref(frame);
				break;
			}
			drop_relax(p, &dc, blocked, now_sec());
		}
		if (skip_to_key) {
			/* what the decoder still holds belongs to the skipped part */
			avcodec_flush_buffers(p->vdec);
		}
	}
	av_frame_free(&frame);
	av_packet_free(&pkt);
	return NULL;
}

/* ---------------------------------------------------------- reader thread --- */

static bool queues_full(struct player *p)
{
	int vn, vb, an, ab;
	int64_t vd, ad;
	bool v_ok, a_ok;

	pktq_stat(&p->vq, &vn, &vb, &vd);
	pktq_stat(&p->aq, &an, &ab, &ad);
	if (vb + ab > MAX_QUEUE_BYTES)
		return true;
	v_ok = !p->vst || (vn > MIN_QUEUE_PKTS && (!vd || vd * av_q2d(p->vst->time_base) > 1.0));
	a_ok = !p->ast || (an > MIN_QUEUE_PKTS && (!ad || ad * av_q2d(p->ast->time_base) > 1.0));
	return v_ok && a_ok;
}

static void reader_seek(struct player *p, double pos, int dir, double from)
{
	int64_t ts = (int64_t)((pos + p->start_time) * AV_TIME_BASE);
	int64_t cur = (int64_t)((from + p->start_time) * AV_TIME_BASE);
	int flags = AVSEEK_FLAG_BACKWARD;	/* the key frame at or before the target */
	int e;

	if (dir > 0) {
		/*
		 * A step forwards must not end behind the current position:
		 * if the key frame before the target is not ahead of it (or
		 * the container has no index to tell), take the next one.
		 */
		AVStream *st = p->vst ? p->vst : p->ast;
		int64_t t = av_rescale_q(ts, AV_TIME_BASE_Q, st->time_base);
		int64_t c = av_rescale_q(cur + AV_TIME_BASE / 2, AV_TIME_BASE_Q, st->time_base);
		int idx = av_index_search_timestamp(st, t, AVSEEK_FLAG_BACKWARD);
		const AVIndexEntry *ie = idx >= 0 ? avformat_index_get_entry(st, idx) : NULL;

		if (!ie || ie->timestamp <= c)
			flags = 0;
	}
	e = av_seek_frame(p->ic, -1, ts, flags);
	if (e < 0)
		e = av_seek_frame(p->ic, -1, ts, flags ^ AVSEEK_FLAG_BACKWARD);
	if (e < 0) {
		err("cannot seek to %.1f s: %s", pos, av_err2str(e));
		pthread_mutex_lock(&p->mu);
		p->seeking = false;
		p->seek_pos = from;
		pthread_mutex_unlock(&p->mu);
		wake_main();
		return;
	}

	pthread_mutex_lock(&p->mu);
	atomic_fetch_add(&p->serial, 1);
	p->clk_started = false;
	p->clk_audio = false;
	p->seek_pos = pos;
	p->seeking = false;
	p->demux_eof = false;
	p->audio_done = !p->ast;
	p->video_dec_done = !p->vst;
	p->ended = false;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mu);

	pktq_flush(&p->vq);
	pktq_flush(&p->aq);
	pktq_wake(&p->vq);
	pktq_wake(&p->aq);
	frameq_wake(&p->fq);
	wake_main();
}

static void *reader_thread(void *arg)
{
	struct player *p = arg;
	AVPacket *pkt = av_packet_alloc();
	int errors = 0;

	while (pkt) {
		bool do_seek = false;
		double target = 0, from = 0;
		int dir = 0, serial, e;

		pthread_mutex_lock(&p->mu);
		for (;;) {
			if (p->abort)
				break;
			if (p->seek_req) {
				do_seek = true;
				target = p->seek_target;
				dir = p->seek_dir;
				from = p->seek_from;
				p->seek_req = false;
				p->seeking = true;
				break;
			}
			if (!p->demux_eof && !queues_full(p))
				break;
			p->reader_waiting = true;
			pthread_cond_wait(&p->reader_cond, &p->mu);
			p->reader_waiting = false;
		}
		pthread_mutex_unlock(&p->mu);
		if (p->abort)
			break;
		if (do_seek) {
			reader_seek(p, target, dir, from);
			continue;
		}

		e = av_read_frame(p->ic, pkt);
		serial = atomic_load(&p->serial);
		if (e < 0 && e != AVERROR_EOF && !p->abort && !avio_feof(p->ic->pb) &&
		    !(p->ic->pb && p->ic->pb->error) && ++errors < 200) {
			/* a damaged stretch of the file: the demuxer finds its way again */
			if (errors == 1)
				err("read error: %s (going on)", av_err2str(e));
			continue;
		}
		if (e >= 0)
			errors = 0;
		if (e < 0) {
			if (e != AVERROR_EOF && !p->abort)
				err("read error: %s", av_err2str(e));
			if (p->vst)
				pktq_put(&p->vq, NULL, serial);
			if (p->ast)
				pktq_put(&p->aq, NULL, serial);
			pthread_mutex_lock(&p->mu);
			p->demux_eof = true;
			pthread_mutex_unlock(&p->mu);
			wake_main();
			continue;
		}
		if (!p->start_known && (pkt->stream_index == p->vidx || pkt->stream_index == p->aidx)) {
			int64_t t = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;

			if (t != AV_NOPTS_VALUE) {
				pthread_mutex_lock(&p->mu);
				p->start_time = t * av_q2d(p->ic->streams[pkt->stream_index]->time_base) - p->opt.start;
				p->start_known = true;
				pthread_mutex_unlock(&p->mu);
			}
		}
		if (pkt->stream_index == p->vidx)
			pktq_put(&p->vq, pkt, serial);
		else if (pkt->stream_index == p->aidx)
			pktq_put(&p->aq, pkt, serial);
		else
			av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	return NULL;
}

/* ------------------------------------------------------------------ open --- */

static int interrupt_cb(void *arg)
{
	struct player *p = arg;

	return p->abort;
}

/* streams fully described by the container need no (slow) probing */
static bool need_stream_info(AVFormatContext *ic)
{
	bool dur = ic->duration != AV_NOPTS_VALUE && ic->duration > 0;
	unsigned i;

	if (!ic->nb_streams || (ic->ctx_flags & AVFMTCTX_NOHEADER))
		return true;
	for (i = 0; i < ic->nb_streams; i++) {
		AVStream *st = ic->streams[i];
		AVCodecParameters *par = st->codecpar;

		if (par->codec_id == AV_CODEC_ID_NONE || par->codec_id == AV_CODEC_ID_PROBE)
			return true;
		if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
		    !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
		    (par->width <= 0 || par->height <= 0))
			return true;
		if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
			dur = true;
	}
	return !dur;
}

static void copy_tag(char *dst, size_t size, AVFormatContext *ic, AVStream *st, const char *key)
{
	const AVDictionaryEntry *e = av_dict_get(ic->metadata, key, NULL, 0);

	if (!e && st)
		e = av_dict_get(st->metadata, key, NULL, 0);
	snprintf(dst, size, "%s", e ? e->value : "");
}

static void read_info(struct player *p)
{
	AVFormatContext *ic = p->ic;
	unsigned i;

	p->duration = 0;
	if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0)
		p->duration = ic->duration / (double)AV_TIME_BASE;
	for (i = 0; i < ic->nb_streams && p->duration <= 0; i++) {
		AVStream *st = ic->streams[i];

		if ((st == p->vst || st == p->ast) && st->duration != AV_NOPTS_VALUE && st->duration > 0)
			p->duration = st->duration * av_q2d(st->time_base);
	}
	if (ic->start_time != AV_NOPTS_VALUE) {
		p->start_time = ic->start_time / (double)AV_TIME_BASE;
		p->start_known = true;
	} else {
		AVStream *st = p->vst ? p->vst : p->ast;

		if (st && st->start_time != AV_NOPTS_VALUE) {
			p->start_time = st->start_time * av_q2d(st->time_base);
			p->start_known = true;
		}
	}

	copy_tag(p->title, sizeof(p->title), ic, p->ast, "title");
	copy_tag(p->artist, sizeof(p->artist), ic, p->ast, "artist");
	copy_tag(p->album, sizeof(p->album), ic, p->ast, "album");
	if (!p->title[0]) {
		const char *base = strrchr(p->opt.file, '/');
		char *dot;

		snprintf(p->title, sizeof(p->title), "%s", base ? base + 1 : p->opt.file);
		dot = strrchr(p->title, '.');
		if (dot && dot != p->title)
			*dot = 0;
	}
	if (p->vst) {
		AVRational fr = p->vst->avg_frame_rate.num ? p->vst->avg_frame_rate : p->vst->r_frame_rate;

		p->frame_dur = fr.num > 0 && fr.den > 0 ? av_q2d(av_inv_q(fr)) : 0.04;
		if (p->frame_dur < 0.005 || p->frame_dur > 1)
			p->frame_dur = 0.04;
		snprintf(p->vinfo, sizeof(p->vinfo), "%s %dx%d %.2f fps", p->vdec->codec->name,
			 p->vst->codecpar->width, p->vst->codecpar->height, 1 / p->frame_dur);
	}
	if (p->ast && p->tremor)
		snprintf(p->ainfo, sizeof(p->ainfo), "vorbis(tremor) %d Hz %d ch",
			 tremor_rate(p->tremor), tremor_channels(p->tremor));
	else if (p->ast)
		snprintf(p->ainfo, sizeof(p->ainfo), "%s %d Hz %d ch", p->adec->codec->name,
			 p->ast->codecpar->sample_rate, p->ast->codecpar->ch_layout.nb_channels);
}

static void select_streams(struct player *p)
{
	const struct options *opt = &p->opt;
	unsigned i;

	for (i = 0; i < p->ic->nb_streams; i++) {
		AVStream *st = p->ic->streams[i];
		AVCodecParameters *par = st->codecpar;

		st->discard = AVDISCARD_ALL;
		if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
			continue;	/* cover art */
		if (par->codec_type == AVMEDIA_TYPE_VIDEO && !p->vst && !opt->no_video) {
			const AVCodec *codec = find_decoder(par->codec_id);
			int lowres = opt->lowres >= 0 ? opt->lowres :
				     auto_lowres(p, codec, par->width, par->height);

			p->vdec = open_decoder(p, st, lowres);
			if (!p->vdec) {
				err("video stream %u (%s) cannot be decoded", i,
				    avcodec_get_name(par->codec_id));
				continue;
			}
			atomic_store(&p->lowres, p->vdec->lowres);
			p->vst = st;
			p->vidx = i;
			st->discard = AVDISCARD_DEFAULT;
		} else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !p->ast && !opt->no_audio) {
			/* Vorbis: the integer decoder if it can handle the stream */
			if (par->codec_id == AV_CODEC_ID_VORBIS && !opt->float_vorbis)
				p->tremor = tremor_open(par);
			if (!p->tremor)
				p->adec = open_decoder(p, st, 0);
			if (!p->adec && !p->tremor) {
				err("audio stream %u (%s) cannot be decoded", i,
				    avcodec_get_name(par->codec_id));
				continue;
			}
			p->ast = st;
			p->aidx = i;
			st->discard = AVDISCARD_DEFAULT;
		}
	}
}

int player_open(struct player *p, const struct options *opt)
{
	AVDictionary *fopts = NULL;
	double t0 = now_sec(), t1, t2;
	bool probed = false;
	unsigned i;
	int e;

	memset(p, 0, sizeof(*p));
	p->opt = *opt;
	p->vidx = p->aidx = -1;
	p->serial = 0;
	p->shown_serial = -1;
	atomic_store(&p->volume, opt->volume);
	atomic_store(&p->level, opt->min_level);
	pthread_mutex_init(&p->mu, NULL);
	cond_init_monotonic(&p->cond);
	pthread_cond_init(&p->reader_cond, NULL);
	pktq_init(&p->vq);
	pktq_init(&p->aq);
	if (frameq_init(&p->fq) < 0)
		return -1;

	/* the libraries' own messages: only when asked (-v errors, -vv everything) */
	if (g_loglevel < LOG_DBG)
		av_log_set_level(g_loglevel >= LOG_VERB ? AV_LOG_ERROR : AV_LOG_FATAL);

	p->ic = avformat_alloc_context();
	if (!p->ic)
		return -1;
	p->ic->interrupt_callback.callback = interrupt_cb;
	p->ic->interrupt_callback.opaque = p;
	/* the files are local and well formed: do not read megabytes to be sure */
	av_dict_set(&fopts, "probesize", "1048576", 0);
	av_dict_set(&fopts, "analyzeduration", "1500000", 0);
	av_dict_set(&fopts, "fpsprobesize", "0", 0);
	e = avformat_open_input(&p->ic, opt->file, NULL, &fopts);
	av_dict_free(&fopts);
	if (e < 0) {
		err("%s: %s", opt->file, av_err2str(e));
		return -1;
	}
	t1 = now_sec();
	if (need_stream_info(p->ic)) {
		probed = true;
		e = avformat_find_stream_info(p->ic, NULL);
		if (e < 0)
			err("%s: cannot analyse the streams: %s", opt->file, av_err2str(e));
	}
	t2 = now_sec();

	select_streams(p);
	if (!p->vst && !p->ast && !probed) {
		/* maybe the container did not tell enough: look inside */
		for (i = 0; i < p->ic->nb_streams; i++)
			p->ic->streams[i]->discard = AVDISCARD_DEFAULT;
		probed = true;
		if (avformat_find_stream_info(p->ic, NULL) >= 0)
			select_streams(p);
	}
	if (!p->vst && !p->ast) {
		err("%s: nothing to play", opt->file);
		return -1;
	}
	read_info(p);
	p->audio_done = !p->ast;
	p->video_dec_done = !p->vst;
	p->paused = opt->start_paused;
	p->seek_pos = opt->start > 0 ? opt->start : 0;
	if (opt->start > 0) {
		p->seek_req = true;
		p->seek_target = opt->start;
		p->seek_from = 0;
	}
	verb("open %.0f ms, stream analysis %.0f ms%s, decoders %.0f ms",
	     (t1 - t0) * 1e3, (t2 - t1) * 1e3, probed ? "" : " (skipped)", (now_sec() - t2) * 1e3);
	if (p->vst)
		verb("video: %s%s", p->vinfo, p->vdec->lowres ? " (lowres)" : "");
	if (p->ast)
		verb("audio: %s", p->ainfo);
	return 0;
}

int player_start(struct player *p)
{
	if (pthread_create(&p->reader_th, NULL, reader_thread, p))
		return -1;
	p->reader_run = true;
	if (p->vst) {
		if (pthread_create(&p->video_th, NULL, video_thread, p))
			return -1;
		p->video_run = true;
	}
	if (p->ast) {
		if (pthread_create(&p->audio_th, NULL, audio_thread, p))
			return -1;
		p->audio_run = true;
	}
	return 0;
}

void player_stop(struct player *p)
{
	pthread_mutex_lock(&p->mu);
	atomic_store(&p->abort, true);
	pthread_cond_broadcast(&p->cond);
	pthread_cond_broadcast(&p->reader_cond);
	pthread_mutex_unlock(&p->mu);
	pktq_wake(&p->vq);
	pktq_wake(&p->aq);
	frameq_wake(&p->fq);
	if (p->reader_run)
		pthread_join(p->reader_th, NULL);
	if (p->video_run)
		pthread_join(p->video_th, NULL);
	if (p->audio_run)
		pthread_join(p->audio_th, NULL);
	p->reader_run = p->video_run = p->audio_run = false;
}

void player_close(struct player *p)
{
	int i;

	pktq_flush(&p->vq);
	pktq_flush(&p->aq);
	for (i = 0; i < FRAMEQ_SIZE; i++)
		av_frame_free(&p->fq.q[i].f);
	avcodec_free_context(&p->vdec);
	avcodec_free_context(&p->adec);
	tremor_close(p->tremor);
	p->tremor = NULL;
	avformat_close_input(&p->ic);
}

/* --------------------------------------------------------------- control --- */

void player_set_paused(struct player *p, bool paused)
{
	double now = now_sec();

	pthread_mutex_lock(&p->mu);
	if (p->paused != paused) {
		if (paused) {
			/* freeze the clock where it is */
			if (p->clk_started)
				p->clk_pts = clock_get_locked(p, now);
		}
		p->clk_at = now;
		p->paused = paused;
		pthread_cond_broadcast(&p->cond);
	}
	pthread_mutex_unlock(&p->mu);
	wake_main();
}

void player_seek(struct player *p, double pos, int dir)
{
	pthread_mutex_lock(&p->mu);
	if (p->duration > 0 && pos > p->duration - 0.5)
		pos = p->duration - 0.5;
	if (pos < 0)
		pos = 0;
	if (!p->seek_req && !p->seeking) {
		double c = clock_get_locked(p, now_sec());

		p->seek_from = isnan(c) ? p->seek_pos : c - p->start_time;
	}
	p->seek_req = true;
	p->seek_target = pos;
	p->seek_dir = dir;
	p->seek_pos = pos;
	pthread_cond_signal(&p->reader_cond);
	pthread_mutex_unlock(&p->mu);
}

void player_seek_rel(struct player *p, double delta)
{
	player_seek(p, player_pos(p) + delta, delta > 0 ? 1 : -1);
}

void player_toggle(struct player *p)
{
	bool ended, paused;

	pthread_mutex_lock(&p->mu);
	ended = p->ended;
	paused = p->paused;
	pthread_mutex_unlock(&p->mu);
	if (ended)
		player_seek(p, 0, 0);	/* play it again */
	player_set_paused(p, !paused);
}

void player_set_volume(struct player *p, int vol)
{
	atomic_store(&p->volume, vol < 0 ? 0 : vol > 100 ? 100 : vol);
}

int player_state(struct player *p)
{
	int s;

	pthread_mutex_lock(&p->mu);
	s = p->ended ? ST_ENDED : p->paused ? ST_PAUSED : ST_PLAYING;
	pthread_mutex_unlock(&p->mu);
	return s;
}

double player_pos(struct player *p)
{
	double pos;

	pthread_mutex_lock(&p->mu);
	if (p->ended && !p->seek_req && !p->seeking && p->duration > 0)
		pos = p->duration;
	else if (p->seek_req || p->seeking || !p->clk_started)
		pos = p->seek_pos;
	else
		pos = clock_get_locked(p, now_sec()) - p->start_time;
	if (p->duration > 0 && pos > p->duration)
		pos = p->duration;
	if (pos < 0)
		pos = 0;
	pthread_mutex_unlock(&p->mu);
	return pos;
}

/* everything was read, decoded and played */
bool player_finished(struct player *p)
{
	bool done;

	pthread_mutex_lock(&p->mu);
	done = p->demux_eof && p->audio_done && p->video_dec_done && !p->seek_req && !p->seeking;
	pthread_mutex_unlock(&p->mu);
	return done && !frameq_peek(&p->fq, 0);
}

void player_print_stats(struct player *p)
{
	struct stats *s = &p->st;

	info("stats: video shown=%d decoded=%d late-dropped=%d key-skips=%d skipped-packets=%d "
	     "max-level=%d lowres=%d | sync mean=%.1f ms max=%.1f ms | first-frame=%.0f ms",
	     atomic_load(&s->v_shown), atomic_load(&s->v_decoded), atomic_load(&s->v_drop_late),
	     atomic_load(&s->v_catchups), atomic_load(&s->v_skip_pkts),
	     atomic_load(&s->v_level_max), atomic_load(&p->lowres),
	     s->sync_n ? s->sync_sum / s->sync_n * 1e3 : 0.0, s->sync_abs_max * 1e3,
	     s->first_frame_at * 1e3);
}

/* -------------------------------------------------------------- benchmark --- */

static double thread_cpu(void)
{
	struct timespec t;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

/*
 * Decodes the file as fast as possible, without output: tells how far from
 * real time the CPU is (run it on the Brain).
 */
int player_benchmark(struct player *p)
{
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	double tv = 0, ta = 0, vmax = 0, t0, dt, limit = p->opt.max_time;
	double vfirst = NAN, vlast = NAN, wall0 = now_sec();
	int64_t asamples = 0;
	int vframes = 0, arate = 0;
	bool veof = !p->vst, aeof = !p->ast, eof = false;

	if (!pkt || !frame)
		return -1;
	if (p->vdec)
		apply_level(p->vdec, p->opt.min_level);
	while (!veof || !aeof) {
		AVCodecContext *dec = NULL;
		bool video;

		if (!eof && av_read_frame(p->ic, pkt) < 0)
			eof = true;
		if (!eof) {
			AVStream *st = p->ic->streams[pkt->stream_index];
			int64_t t = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;

			if (limit > 0 && t != AV_NOPTS_VALUE &&
			    t * av_q2d(st->time_base) - (p->start_known ? p->start_time : 0) > limit) {
				av_packet_unref(pkt);
				eof = true;
				continue;
			}
			if (pkt->stream_index == p->vidx)
				dec = p->vdec;
			else if (pkt->stream_index == p->aidx)
				dec = p->adec;
			if (!dec && !(p->tremor && pkt->stream_index == p->aidx)) {
				av_packet_unref(pkt);
				continue;
			}
		} else {
			dec = !veof ? p->vdec : p->adec;	/* drain */
		}
		video = dec && dec == p->vdec;
		t0 = thread_cpu();
		if (dec)
			avcodec_send_packet(dec, eof ? NULL : pkt);
		else
			tremor_send(p->tremor, eof ? NULL : pkt);
		av_packet_unref(pkt);
		for (;;) {
			int e = dec ? avcodec_receive_frame(dec, frame) : tremor_receive(p->tremor, frame);

			if (e == AVERROR_EOF) {
				if (video)
					veof = true;
				else
					aeof = true;
			}
			if (e < 0)
				break;
			if (video) {
				vframes++;
				if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
					double pts = frame->best_effort_timestamp * av_q2d(p->vst->time_base);

					if (isnan(vfirst) || pts < vfirst)
						vfirst = pts;
					if (isnan(vlast) || pts > vlast)
						vlast = pts;
				}
			} else {
				asamples += frame->nb_samples;
				arate = frame->sample_rate;
			}
			av_frame_unref(frame);
		}
		dt = thread_cpu() - t0;
		if (video) {
			tv += dt;
			if (dt > vmax)
				vmax = dt;
		} else {
			ta += dt;
		}
		if (eof && !video && !aeof)
			aeof = true;	/* decoder gave nothing more */
	}
	if (p->vst && vframes > 1 && tv > 0) {
		/* files without usable timestamps: trust the nominal frame rate */
		double span = isnan(vfirst) || vlast - vfirst < vframes * p->frame_dur / 2 ?
			      vframes * p->frame_dur : vlast - vfirst + p->frame_dur;
		double fps = vframes / span;

		printf("video: %s, %d frames (%.1f s at %.2f fps) decoded in %.2f s CPU: "
		       "%.1f fps, %.1f ms/frame (worst packet %.1f ms), %.2fx real time, level %d, lowres %d\n",
		       p->vinfo, vframes, span, fps, tv, vframes / tv, tv / vframes * 1e3,
		       vmax * 1e3, span / tv, p->opt.min_level, p->vdec->lowres);
	}
	if (p->ast && asamples > 0 && arate > 0 && ta > 0) {
		double span = (double)asamples / arate;

		printf("audio: %s, %.1f s decoded in %.2f s CPU: %.1fx real time (%.1f%% of the CPU)\n",
		       p->ainfo, span, ta, span / ta, ta / span * 100);
	}
	printf("total: %.2f s wall clock\n", now_sec() - wall0);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	return 0;
}
