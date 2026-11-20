// SPDX-License-Identifier: MIT
/*
 * Vorbis through Tremor (libvorbisidec), the integer-only decoder.
 *
 * FFmpeg's Vorbis decoder is floating point only, and on a CPU without an
 * FPU that is 20-40 times slower than integer code. Mono and stereo Vorbis
 * streams (Ogg, WebM, Matroska) are therefore decoded here; the interface
 * mimics avcodec_send_packet() / avcodec_receive_frame() and gives
 * interleaved S16 frames.
 */
#include "brainplay.h"

#ifdef HAVE_TREMOR

#include <limits.h>
#include <tremor/ivorbiscodec.h>

#include <libavutil/intreadwrite.h>

struct tremor {
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;
	bool dsp_ok;
	bool eof;
	int64_t pts;
	int64_t packetno;
};

/* the three Vorbis headers out of the codec extradata (Xiph lacing) */
static int split_headers(const uint8_t *ex, int size, const uint8_t *hdr[3], int len[3])
{
	int i;

	if (size >= 6 && AV_RB16(ex) == 30) {
		/* each header after a 16 bit length */
		int left = size;

		for (i = 0; i < 3; i++) {
			if (left < 2)
				return -1;
			len[i] = AV_RB16(ex);
			ex += 2;
			left -= 2;
			if (len[i] > left)
				return -1;
			hdr[i] = ex;
			ex += len[i];
			left -= len[i];
		}
		return 0;
	}
	if (size >= 3 && size < INT_MAX - 0x1ff && ex[0] == 2) {
		int overall = 3;

		ex++;
		for (i = 0; i < 2; i++, ex++) {
			len[i] = 0;
			for (; overall < size && *ex == 0xff; ex++) {
				len[i] += 0xff;
				overall += 0xff + 1;
			}
			len[i] += *ex;
			overall += *ex;
			if (overall > size)
				return -1;
		}
		len[2] = size - overall;
		hdr[0] = ex;
		hdr[1] = hdr[0] + len[0];
		hdr[2] = hdr[1] + len[1];
		return 0;
	}
	return -1;
}

struct tremor *tremor_open(const AVCodecParameters *par)
{
	const uint8_t *hdr[3];
	int len[3], i;
	struct tremor *t;

	if (par->codec_id != AV_CODEC_ID_VORBIS || !par->extradata ||
	    split_headers(par->extradata, par->extradata_size, hdr, len) < 0)
		return NULL;
	t = av_mallocz(sizeof(*t));
	if (!t)
		return NULL;
	vorbis_info_init(&t->vi);
	vorbis_comment_init(&t->vc);
	for (i = 0; i < 3; i++) {
		ogg_packet op = {
			.packet = (unsigned char *)hdr[i],
			.bytes = len[i],
			.b_o_s = i == 0,
			.packetno = i,
		};

		if (vorbis_synthesis_headerin(&t->vi, &t->vc, &op) < 0)
			goto fail;
	}
	/* more than two channels would need a proper downmix: FFmpeg does that */
	if (t->vi.channels < 1 || t->vi.channels > 2 || t->vi.rate <= 0)
		goto fail;
	if (vorbis_synthesis_init(&t->vd, &t->vi))
		goto fail;
	t->dsp_ok = true;
	vorbis_block_init(&t->vd, &t->vb);
	t->pts = AV_NOPTS_VALUE;
	t->packetno = 3;
	return t;
fail:
	tremor_close(t);
	return NULL;
}

void tremor_close(struct tremor *t)
{
	if (!t)
		return;
	if (t->dsp_ok) {
		vorbis_block_clear(&t->vb);
		vorbis_dsp_clear(&t->vd);
	}
	vorbis_comment_clear(&t->vc);
	vorbis_info_clear(&t->vi);
	av_free(t);
}

void tremor_reset(struct tremor *t)
{
	vorbis_synthesis_restart(&t->vd);
	t->eof = false;
	t->pts = AV_NOPTS_VALUE;
}

int tremor_send(struct tremor *t, const AVPacket *pkt)
{
	ogg_packet op = { 0 };

	if (!pkt) {
		t->eof = true;
		return 0;
	}
	op.packet = pkt->data;
	op.bytes = pkt->size;
	op.granulepos = -1;
	op.packetno = t->packetno++;
	if (vorbis_synthesis(&t->vb, &op) == 0)
		vorbis_synthesis_blockin(&t->vd, &t->vb);
	t->pts = pkt->pts;
	return 0;
}

int tremor_receive(struct tremor *t, AVFrame *f)
{
	ogg_int32_t **pcm;
	int n = vorbis_synthesis_pcmout(&t->vd, &pcm);
	int ch = t->vi.channels, c, i;
	int16_t *out;

	if (n <= 0)
		return t->eof ? AVERROR_EOF : AVERROR(EAGAIN);
	f->format = AV_SAMPLE_FMT_S16;
	f->nb_samples = n;
	f->sample_rate = (int)t->vi.rate;
	av_channel_layout_default(&f->ch_layout, ch);
	if (av_frame_get_buffer(f, 0) < 0) {
		av_frame_unref(f);
		vorbis_synthesis_read(&t->vd, n);
		return AVERROR(ENOMEM);
	}
	out = (int16_t *)f->data[0];
	for (c = 0; c < ch; c++) {
		const ogg_int32_t *src = pcm[c];

		/* Tremor's samples are 1.9.22 fixed point */
		for (i = 0; i < n; i++) {
			int v = src[i] >> 9;

			out[i * ch + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
	vorbis_synthesis_read(&t->vd, n);
	f->pts = t->pts;
	t->pts = AV_NOPTS_VALUE;	/* what follows continues from here */
	return 0;
}

int tremor_rate(const struct tremor *t)
{
	return (int)t->vi.rate;
}

int tremor_channels(const struct tremor *t)
{
	return t->vi.channels;
}

#else /* !HAVE_TREMOR */

struct tremor *tremor_open(const AVCodecParameters *par)
{
	return NULL;
}

void tremor_close(struct tremor *t)
{
}

void tremor_reset(struct tremor *t)
{
}

int tremor_send(struct tremor *t, const AVPacket *pkt)
{
	return AVERROR(ENOSYS);
}

int tremor_receive(struct tremor *t, AVFrame *f)
{
	return AVERROR(ENOSYS);
}

int tremor_rate(const struct tremor *t)
{
	return 0;
}

int tremor_channels(const struct tremor *t)
{
	return 0;
}

#endif
