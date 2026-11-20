// SPDX-License-Identifier: MIT
/*
 * Audio output: interleaved S16 stereo to ALSA ("default", so that the plug
 * layer converts the rate if the hardware needs it), or to a timed null
 * sink that consumes the samples in real time.
 *
 * Everything is called from the audio thread only.
 *
 * Pause does not depend on the driver: the device is stopped at once
 * (snd_pcm_drop) and the part that had been written but not played yet is
 * queued again on resume from a small ring that keeps the last samples.
 */
#include "brainplay.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct ao_ll {
	const char *name;
	int (*open)(struct ao *ao, const char *device, int rate);
	int (*write)(struct ao *ao, const int16_t *data, int frames);
	long (*delay)(struct ao *ao);	/* frames queued in the device */
	void (*stop)(struct ao *ao);	/* drop what is queued, ready for more */
	void (*start)(struct ao *ao);	/* start even if little was written */
	void (*close)(struct ao *ao);
};

struct ao {
	const struct ao_ll *ll;
	int rate;
	int buffer_frames;
	const atomic_bool *abort;
	int underruns;

	/* last samples written, for the resume after a pause */
	int16_t *ring;
	int ring_frames;
	int64_t ring_total;
	int replay;

	/*
	 * Virtual device clock: what a real-time device would have played of
	 * the frames written since the last stop. It paces the null sink and
	 * any ALSA device that takes the samples without ever blocking
	 * (the "null" PCM, a file plugin).
	 */
	bool selfpace;
	int nonpacing;
	int64_t v_written;
	double v_t0;

	snd_pcm_t *pcm;
};

/* ----------------------------------------------------------------- ALSA --- */

static int alsa_open(struct ao *ao, const char *device, int rate)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t bufsz = 0, persz = 0, start;
	unsigned r = rate, t;
	int e;

	e = snd_pcm_open(&ao->pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
	if (e < 0) {
		err("ALSA: cannot open \"%s\": %s", device, snd_strerror(e));
		ao->pcm = NULL;
		return -1;
	}
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((e = snd_pcm_hw_params_any(ao->pcm, hw)) < 0)
		goto fail;
	snd_pcm_hw_params_set_rate_resample(ao->pcm, hw, 1);
	if ((e = snd_pcm_hw_params_set_access(ao->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (e = snd_pcm_hw_params_set_format(ao->pcm, hw, SND_PCM_FORMAT_S16)) < 0 ||
	    (e = snd_pcm_hw_params_set_channels(ao->pcm, hw, 2)) < 0 ||
	    (e = snd_pcm_hw_params_set_rate_near(ao->pcm, hw, &r, NULL)) < 0)
		goto fail;
	/* big enough to survive a slow key frame, small enough for the seeks */
	t = 250000;
	snd_pcm_hw_params_set_buffer_time_near(ao->pcm, hw, &t, NULL);
	t = 25000;
	snd_pcm_hw_params_set_period_time_near(ao->pcm, hw, &t, NULL);
	if ((e = snd_pcm_hw_params(ao->pcm, hw)) < 0)
		goto fail;
	snd_pcm_hw_params_get_buffer_size(hw, &bufsz);
	snd_pcm_hw_params_get_period_size(hw, &persz, NULL);
	if (!bufsz)
		bufsz = r / 4;
	if (!persz)
		persz = bufsz / 8;

	start = persz * 2;
	if (start < r / 20)
		start = r / 20;
	if (start > bufsz)
		start = bufsz;
	if (snd_pcm_sw_params_current(ao->pcm, sw) >= 0) {
		snd_pcm_sw_params_set_start_threshold(ao->pcm, sw, start);
		snd_pcm_sw_params_set_avail_min(ao->pcm, sw, persz);
		snd_pcm_sw_params(ao->pcm, sw);
	}
	ao->rate = r;
	ao->buffer_frames = bufsz;
	verb("ALSA \"%s\": %u Hz, buffer %lu frames (%lu ms), period %lu",
	     device, r, (unsigned long)bufsz, (unsigned long)(bufsz * 1000 / r),
	     (unsigned long)persz);
	return 0;
fail:
	err("ALSA: cannot configure \"%s\" for %d Hz stereo: %s", device, rate, snd_strerror(e));
	snd_pcm_close(ao->pcm);
	ao->pcm = NULL;
	return -1;
}

static int alsa_write(struct ao *ao, const int16_t *data, int frames)
{
	while (frames > 0) {
		snd_pcm_sframes_t n = snd_pcm_writei(ao->pcm, data, frames);

		if (n == -EAGAIN) {
			snd_pcm_wait(ao->pcm, 100);
			continue;
		}
		if (n < 0) {
			if (n == -EPIPE)
				ao->underruns++;
			n = snd_pcm_recover(ao->pcm, n, 1);
			if (n < 0) {
				err("ALSA: write failed: %s", snd_strerror(n));
				return -1;
			}
			continue;
		}
		data += n * 2;
		frames -= n;
	}
	return 0;
}

static long alsa_delay(struct ao *ao)
{
	snd_pcm_sframes_t d = 0;

	if (snd_pcm_delay(ao->pcm, &d) < 0 || d < 0)
		return 0;
	return d;
}

static void alsa_stop(struct ao *ao)
{
	snd_pcm_drop(ao->pcm);
	snd_pcm_prepare(ao->pcm);
}

static void alsa_start(struct ao *ao)
{
	if (snd_pcm_state(ao->pcm) == SND_PCM_STATE_PREPARED)
		snd_pcm_start(ao->pcm);
}

static void alsa_close(struct ao *ao)
{
	if (ao->pcm) {
		snd_pcm_drop(ao->pcm);
		snd_pcm_close(ao->pcm);
	}
}

static const struct ao_ll ll_alsa = {
	"alsa", alsa_open, alsa_write, alsa_delay, alsa_stop, alsa_start, alsa_close,
};

/* ----------------------------------------------------------------- null --- */

static int null_open(struct ao *ao, const char *device, int rate)
{
	ao->rate = rate;
	ao->buffer_frames = rate / 5;
	ao->selfpace = true;
	return 0;
}

static int null_write(struct ao *ao, const int16_t *data, int frames)
{
	return 0;
}

static long null_delay(struct ao *ao)
{
	return 0;
}

static void null_nop(struct ao *ao)
{
}

static const struct ao_ll ll_null = {
	"null", null_open, null_write, null_delay, null_nop, null_nop, null_nop,
};

/* -------------------------------------------------------------- generic --- */

static long virt_delay(struct ao *ao, double now)
{
	int64_t played, d;

	if (!ao->v_written)
		return 0;
	played = (int64_t)((now - ao->v_t0) * ao->rate);
	d = ao->v_written - played;
	if (d < 0) {
		/* the virtual buffer ran dry: restart the count from now */
		ao->v_written = 0;
		d = 0;
	}
	return d;
}

/* writes to the device, without touching the ring */
static int ao_out(struct ao *ao, const int16_t *data, int frames)
{
	double now;

	if (ao->selfpace) {
		for (;;) {
			long over = virt_delay(ao, now_sec()) + frames - ao->buffer_frames;
			double wait;

			if (over <= 0 || atomic_load(ao->abort))
				break;
			wait = (double)over / ao->rate;
			sleep_sec(wait > 0.05 ? 0.05 : wait);
		}
	}
	if (ao->ll->write(ao, data, frames) < 0)
		return -1;
	now = now_sec();
	if (virt_delay(ao, now) == 0 && !ao->v_written)
		ao->v_t0 = now;
	ao->v_written += frames;

	if (!ao->selfpace) {
		/* a real device cannot be far ahead of real time with an empty buffer */
		if (ao->ll->delay(ao) <= 0 &&
		    virt_delay(ao, now) > 2L * ao->buffer_frames) {
			if (++ao->nonpacing >= 8) {
				info("audio device \"%s\" does not pace playback: timing it here",
				     ao->ll->name);
				ao->selfpace = true;
				ao->v_written = 0;
			}
		} else {
			ao->nonpacing = 0;
		}
	}
	return 0;
}

static long ao_delay_frames(struct ao *ao)
{
	return ao->selfpace ? virt_delay(ao, now_sec()) : ao->ll->delay(ao);
}

struct ao *ao_open(const char *driver, const char *device, int rate, const atomic_bool *abort_flag)
{
	struct ao *ao = calloc(1, sizeof(*ao));

	if (!ao)
		return NULL;
	ao->abort = abort_flag;
	ao->ll = !strcmp(driver, "null") ? &ll_null : &ll_alsa;
	if (ao->ll->open(ao, device, rate) < 0) {
		if (ao->ll == &ll_null) {
			free(ao);
			return NULL;
		}
		info("no audio device: playing without sound");
		ao->ll = &ll_null;
		ao->ll->open(ao, device, rate);
	}
	ao->ring_frames = ao->buffer_frames + ao->rate / 4;
	ao->ring = calloc(ao->ring_frames, 2 * sizeof(int16_t));
	if (!ao->ring) {
		ao->ll->close(ao);
		free(ao);
		return NULL;
	}
	return ao;
}

int ao_rate(const struct ao *ao)
{
	return ao->rate;
}

const char *ao_name(const struct ao *ao)
{
	return ao->ll->name;
}

int ao_underruns(const struct ao *ao)
{
	return ao->underruns;
}

int ao_write(struct ao *ao, const int16_t *data, int frames)
{
	const int16_t *s = data;
	int left = frames;

	/* remember the samples (only the last ring_frames matter) */
	if (left > ao->ring_frames) {
		ao->ring_total += left - ao->ring_frames;
		s += (size_t)(left - ao->ring_frames) * 2;
		left = ao->ring_frames;
	}
	while (left > 0) {
		int pos = (int)(ao->ring_total % ao->ring_frames);
		int n = ao->ring_frames - pos;

		if (n > left)
			n = left;
		memcpy(ao->ring + (size_t)pos * 2, s, (size_t)n * 2 * sizeof(int16_t));
		ao->ring_total += n;
		s += (size_t)n * 2;
		left -= n;
	}
	return ao_out(ao, data, frames);
}

double ao_delay(struct ao *ao)
{
	return (double)ao_delay_frames(ao) / ao->rate;
}

void ao_pause(struct ao *ao)
{
	long d = ao_delay_frames(ao);

	if (d > ao->ring_frames)
		d = ao->ring_frames;
	if (d > ao->ring_total)
		d = ao->ring_total;
	ao->replay = d;
	ao->ll->stop(ao);
	ao->v_written = 0;
}

void ao_resume(struct ao *ao)
{
	int64_t from = ao->ring_total - ao->replay;
	int left = ao->replay;

	ao->replay = 0;
	while (left > 0 && !atomic_load(ao->abort)) {
		int pos = (int)(from % ao->ring_frames);
		int n = ao->ring_frames - pos;

		if (n > left)
			n = left;
		if (ao_out(ao, ao->ring + (size_t)pos * 2, n) < 0)
			break;
		from += n;
		left -= n;
	}
}

void ao_flush(struct ao *ao)
{
	ao->ll->stop(ao);
	ao->replay = 0;
	ao->ring_total = 0;
	ao->v_written = 0;
}

void ao_drain(struct ao *ao)
{
	double limit = now_sec() + (double)(ao->buffer_frames + ao->ring_frames) / ao->rate + 0.5;

	ao->ll->start(ao);
	while (!atomic_load(ao->abort) && now_sec() < limit) {
		double d = ao_delay(ao);

		if (d <= 0.002)
			break;
		sleep_sec(d > 0.05 ? 0.05 : d);
	}
}

void ao_close(struct ao *ao)
{
	if (!ao)
		return;
	ao->ll->close(ao);
	free(ao->ring);
	free(ao);
}
