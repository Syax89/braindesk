// SPDX-License-Identifier: MIT
/*
 * Audio thread: packets -> decoder -> S16 stereo (libswresample, integer
 * paths for the fixed-point decoders) -> software volume -> audio output.
 *
 * The audio output is the master clock: after every write the clock is set to
 * "pts of the last sample written - what is still queued in the device".
 *
 * Start of playback (first start and after every seek): the thread waits for
 * the first video frame to be on screen, then lines the audio up with it
 * (drops the samples that come before that frame, or plays silence up to the
 * first sample), so that sound and picture start together and nothing has to
 * be dropped to catch up.
 */
#include "brainplay.h"

#include <math.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

#define CHUNK_MS 20

struct audio {
	struct player *p;
	struct ao *ao;
	SwrContext *swr;
	int in_rate, in_fmt;
	AVChannelLayout in_layout;
	int16_t *buf;
	int buf_frames;
	int serial;
	bool ao_paused;
	bool started;		/* lined up with the video since the last (re)start */
	double next_pts;	/* pts of the next sample, NAN if unknown */
};

/*
 * Follows the control state. Blocks while paused (with the device stopped).
 * 0 = go on, 1 = a seek happened (drop what you are doing), -1 = quit.
 */
static int audio_sync_state(struct audio *a)
{
	struct player *p = a->p;
	int ret = 0;

	pthread_mutex_lock(&p->mu);
	for (;;) {
		if (p->abort) {
			ret = -1;
			break;
		}
		if (atomic_load(&p->serial) != a->serial) {
			ret = 1;
			break;
		}
		if (!p->paused)
			break;
		if (!a->ao_paused) {
			a->ao_paused = true;
			if (a->ao) {
				pthread_mutex_unlock(&p->mu);
				ao_pause(a->ao);
				pthread_mutex_lock(&p->mu);
				continue;
			}
		}
		pthread_cond_wait(&p->cond, &p->mu);
	}
	pthread_mutex_unlock(&p->mu);

	if (ret == 0 && a->ao_paused) {
		a->ao_paused = false;
		if (a->ao)
			ao_resume(a->ao);
	}
	return ret;
}

/* a seek happened: forget everything that belongs to the old position */
static void audio_restart(struct audio *a)
{
	struct player *p = a->p;

	if (p->tremor)
		tremor_reset(p->tremor);
	else
		avcodec_flush_buffers(p->adec);
	if (a->ao)
		ao_flush(a->ao);
	swr_free(&a->swr);	/* set up again by the next frame */
	a->ao_paused = false;
	a->started = false;
	a->next_pts = NAN;
	a->serial = atomic_load(&p->serial);
}

static void apply_volume(int16_t *d, int samples, int vol)
{
	int gain, i;

	if (vol >= 100)
		return;
	if (vol <= 0) {
		memset(d, 0, (size_t)samples * sizeof(*d));
		return;
	}
	/* quadratic curve: closer to how loudness is perceived than a linear one */
	gain = vol * vol * 32768 / 10000;
	for (i = 0; i < samples; i++)
		d[i] = (int16_t)((d[i] * gain) >> 15);
}

static int audio_config(struct audio *a, const AVFrame *f)
{
	static const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	struct player *p = a->p;
	AVChannelLayout in = { 0 };
	int e;

	if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&f->ch_layout))
		av_channel_layout_default(&in, f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 2);
	else if (av_channel_layout_copy(&in, &f->ch_layout) < 0)
		return -1;

	if (!a->ao || (ao_rate(a->ao) != f->sample_rate && a->in_rate != f->sample_rate)) {
		double t0 = now_sec();

		if (a->ao)
			ao_close(a->ao);
		a->ao = ao_open(p->opt.ao, p->opt.audio_device, f->sample_rate, &p->abort);
		if (!a->ao) {
			av_channel_layout_uninit(&in);
			return -1;
		}
		verb("audio output %s opened in %.0f ms", ao_name(a->ao), (now_sec() - t0) * 1e3);
	}

	swr_free(&a->swr);
	e = swr_alloc_set_opts2(&a->swr, &stereo, AV_SAMPLE_FMT_S16, ao_rate(a->ao),
				&in, f->format, f->sample_rate, 0, NULL);
	if (e >= 0)
		e = swr_init(a->swr);
	if (e < 0) {
		err("cannot convert the audio format (%s, %d Hz, %d ch)",
		    av_get_sample_fmt_name(f->format), f->sample_rate, in.nb_channels);
		swr_free(&a->swr);
		av_channel_layout_uninit(&in);
		return -1;
	}
	av_channel_layout_uninit(&a->in_layout);
	a->in_layout = in;
	a->in_rate = f->sample_rate;
	a->in_fmt = f->format;
	verb("audio: %s %d Hz %d ch -> s16 %d Hz stereo (%s)",
	     av_get_sample_fmt_name(f->format), f->sample_rate, in.nb_channels,
	     ao_rate(a->ao), ao_name(a->ao));
	return 0;
}

/* writes frames to the device in small chunks, keeping the clock up to date */
static int audio_write(struct audio *a, const int16_t *data, int frames, double pts)
{
	struct player *p = a->p;
	int rate = ao_rate(a->ao);
	int chunk = rate * CHUNK_MS / 1000;
	int done = 0;

	while (done < frames) {
		int n = frames - done < chunk ? frames - done : chunk;
		int r = audio_sync_state(a);
		double now;

		if (r)
			return r;
		if (data) {
			if (ao_write(a->ao, data + (size_t)done * 2, n) < 0)
				return -1;
		} else {
			static const int16_t silence[2 * 48000 * CHUNK_MS / 1000];
			int m = n < (int)(sizeof(silence) / 4) ? n : (int)(sizeof(silence) / 4);

			n = m;
			if (ao_write(a->ao, silence, n) < 0)
				return -1;
		}
		done += n;
		if (!isnan(pts)) {
			double delay = ao_delay(a->ao);

			bool starting = false;

			now = now_sec();
			pthread_mutex_lock(&p->mu);
			if (atomic_load(&p->serial) == a->serial && !p->paused) {
				double target = pts + (double)done / rate - delay;
				double cur = clock_get_locked(p, now);

				starting = !p->clk_started;
				/*
				 * The delay reported by the device is only as fine as
				 * its period: follow it slowly, jump only when the
				 * clock is really off (start, seek, underrun).
				 */
				if (p->clk_started && p->clk_audio && fabs(target - cur) < 0.1)
					target = cur + (target - cur) * 0.1;
				clock_set_locked(p, target, now, true);
			}
			pthread_mutex_unlock(&p->mu);
			/* the main thread is waiting for the clock to show the pictures */
			if (starting)
				wake_main();
		}
	}
	return 0;
}

/*
 * First samples after a start or a seek: wait for the picture, then make the
 * audio start at the position the clock (or the first frame) is at.
 */
static int audio_line_up(struct audio *a, double *pts, int16_t **data, int *frames)
{
	struct player *p = a->p;
	int rate = ao_rate(a->ao);
	double target = NAN, gap;

	pthread_mutex_lock(&p->mu);
	if (p->clk_started)
		target = clock_get_locked(p, now_sec());
	else if (p->shown_serial == a->serial)
		target = p->start_pts;
	pthread_mutex_unlock(&p->mu);

	if (isnan(target) || isnan(*pts)) {
		a->started = true;
		return 0;
	}
	gap = *pts - target;
	if (gap < -0.02) {
		/* audio that comes before the picture: drop it */
		int drop = (int)(-gap * rate);

		if (drop >= *frames) {
			*frames = 0;
			return 0;	/* still not lined up */
		}
		*data += (size_t)drop * 2;
		*frames -= drop;
		*pts += (double)drop / rate;
	} else if (gap > 0.02 && gap < 10) {
		/* the audio starts later than the picture: silence up to it */
		int r = audio_write(a, NULL, (int)(gap * rate), target);

		if (r)
			return r;
	}
	a->started = true;
	return 0;
}

/* waits (bounded) until the first video frame of this position is shown */
static int audio_wait_picture(struct audio *a)
{
	struct player *p = a->p;
	struct timespec ts;
	int ret = 0;

	deadline_after(&ts, 2.0);
	pthread_mutex_lock(&p->mu);
	while (p->vst && !p->video_dec_done && !p->clk_started &&
	       p->shown_serial != a->serial) {
		if (p->abort) {
			ret = -1;
			break;
		}
		if (atomic_load(&p->serial) != a->serial) {
			ret = 1;
			break;
		}
		if (pthread_cond_timedwait(&p->cond, &p->mu, &ts))
			break;
	}
	pthread_mutex_unlock(&p->mu);
	return ret;
}

static int audio_play_frame(struct audio *a, AVFrame *f)
{
	struct player *p = a->p;
	double pts = a->next_pts;
	int16_t *data;
	uint8_t *out[1];
	int frames, r, need;

	if (f->pts != AV_NOPTS_VALUE) {
		pts = f->pts * av_q2d(p->ast->time_base);
	} else if (isnan(pts)) {
		/* no timestamps at all: count from where playback was started */
		pthread_mutex_lock(&p->mu);
		pts = p->start_time + p->seek_pos;
		pthread_mutex_unlock(&p->mu);
	}

	if (!a->started && (r = audio_wait_picture(a)))
		return r;

	if (!a->swr || f->sample_rate != a->in_rate || f->format != a->in_fmt ||
	    f->ch_layout.nb_channels != a->in_layout.nb_channels) {
		if (audio_config(a, f) < 0)
			return -1;
	}

	need = swr_get_out_samples(a->swr, f->nb_samples);
	if (need > a->buf_frames) {
		av_free(a->buf);
		a->buf = av_malloc((size_t)(need + 256) * 2 * sizeof(int16_t));
		if (!a->buf) {
			a->buf_frames = 0;
			return -1;
		}
		a->buf_frames = need + 256;
	}
	out[0] = (uint8_t *)a->buf;
	frames = swr_convert(a->swr, out, a->buf_frames,
			     (const uint8_t **)f->extended_data, f->nb_samples);
	if (frames <= 0)
		return 0;
	apply_volume(a->buf, frames * 2, atomic_load(&p->volume));
	atomic_fetch_add(&p->st.a_frames, 1);

	data = a->buf;
	a->next_pts = isnan(pts) ? NAN : pts + (double)frames / ao_rate(a->ao);
	if (!a->started) {
		r = audio_line_up(a, &pts, &data, &frames);
		if (r || !frames)
			return r;
	}
	return audio_write(a, data, frames, pts);
}

/* the stream is over: let the device play what it still has */
static void audio_finish(struct audio *a)
{
	struct player *p = a->p;

	if (a->ao && audio_sync_state(a) == 0)
		ao_drain(a->ao);
	pthread_mutex_lock(&p->mu);
	if (atomic_load(&p->serial) == a->serial) {
		p->audio_done = true;
		/* from here on the clock runs on the system time */
		p->clk_audio = false;
		pthread_cond_broadcast(&p->cond);
	}
	pthread_mutex_unlock(&p->mu);
	wake_main();
}

void *audio_thread(void *arg)
{
	struct player *p = arg;
	struct audio a = { .p = p, .serial = -1, .next_pts = NAN };
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	unsigned kick = 0;
	bool failed = false;

	while (pkt && frame) {
		int pkt_serial, r, e;

		r = audio_sync_state(&a);
		if (r < 0)
			break;
		if (r > 0) {
			audio_restart(&a);
			continue;
		}
		r = pktq_get(p, &p->aq, pkt, &pkt_serial, &kick);
		if (r < 0)
			break;
		if (r == 0)
			continue;
		if (pkt_serial != a.serial || failed) {
			bool eof = !pkt->data;

			av_packet_unref(pkt);
			if (failed && eof)
				audio_finish(&a);
			continue;
		}
		if (p->tremor)
			e = tremor_send(p->tremor, pkt->data ? pkt : NULL);
		else
			e = avcodec_send_packet(p->adec, pkt->data ? pkt : NULL);
		av_packet_unref(pkt);
		if (e < 0 && e != AVERROR(EAGAIN) && e != AVERROR_EOF)
			dbg("audio: decoder rejected a packet: %s", av_err2str(e));
		for (;;) {
			e = p->tremor ? tremor_receive(p->tremor, frame) :
					avcodec_receive_frame(p->adec, frame);
			if (e == AVERROR_EOF) {
				audio_finish(&a);
				break;
			}
			if (e < 0)
				break;
			r = audio_play_frame(&a, frame);
			av_frame_unref(frame);
			if (r < 0 && !p->abort) {
				/* no way to play this stream: go on without sound */
				err("audio disabled");
				failed = true;
				pthread_mutex_lock(&p->mu);
				p->audio_done = true;
				p->clk_audio = false;
				pthread_cond_broadcast(&p->cond);
				pthread_mutex_unlock(&p->mu);
				wake_main();
			}
			if (r)
				break;
		}
	}

	if (a.ao) {
		if (p->opt.stats || g_loglevel >= LOG_VERB)
			info("audio: %s, %d underruns", ao_name(a.ao), ao_underruns(a.ao));
		ao_close(a.ao);
	}
	swr_free(&a.swr);
	av_channel_layout_uninit(&a.in_layout);
	av_free(a.buf);
	av_frame_free(&frame);
	av_packet_free(&pkt);
	return NULL;
}
