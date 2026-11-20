# Third-party libraries of brainplay (FFmpeg, libopus, Tremor), included by
# Makefile. Changing this file rebuilds them.

FFVER     := 9.0.2
OPUSVER   := 1.6.1
OGGVER    := 1.3.5
# Tremor has no releases: this is the snapshot Debian 13 ships
TREMORVER := 1.2.1+git20180316

# Everything off, then only what a dictionary-sized player needs.
#
# Audio decoders are the fixed-point ones wherever there is one (the CPU has
# no FPU, floating point is emulated and 20-40 times slower):
#   aac_fixed, ac3_fixed    FFmpeg's fixed-point decoders
#   mp3, mp2                the plain names ARE the fixed-point decoders
#                           (the float ones are mp3float/mp2float: not built)
#   libopus                 libopus built with --enable-fixed-point (FFmpeg's
#                           own opus decoder is float only: not built)
#   vorbis                  FFmpeg only has a float decoder; brainplay itself
#                           decodes mono/stereo Vorbis with Tremor (integer)
#                           and keeps this one for multichannel files
#
# The raw mpegvideo/h264/m4v/ac3 demuxers are there because MPEG-PS/TS use
# their probe functions to recognise the elementary streams.
FF_COMPONENTS := \
	--enable-protocol=file \
	--enable-demuxer=mov,matroska,avi,mpegts,mpegps,flv,mp3,ogg,flac,wav,aac \
	--enable-demuxer=mpegvideo,h264,m4v,ac3 \
	--enable-decoder=h264,mpeg4,msmpeg4v2,msmpeg4v3,h263,flv,mpeg1video,mpeg2video,mjpeg,vp8 \
	--enable-decoder=aac_fixed,mp3,mp2,ac3_fixed,vorbis,flac \
	--enable-libopus --enable-decoder=libopus \
	--enable-decoder=pcm_s16le,pcm_s16be,pcm_u8,pcm_s8,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be \
	--enable-decoder=pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,pcm_dvd,pcm_bluray \
	--enable-decoder=adpcm_ms,adpcm_ima_wav,adpcm_ima_qt \
	--enable-parser=h264,mpeg4video,mpegvideo,mpegaudio,aac,ac3,vp8,vorbis,opus,flac,mjpeg,h263

FF_COMMON := \
	--disable-everything --disable-autodetect --disable-iconv \
	--disable-programs --disable-doc \
	--disable-avdevice --disable-avfilter --disable-swscale \
	--disable-network \
	--disable-shared --enable-static \
	--enable-hardcoded-tables \
	$(FF_COMPONENTS)

# ARMv5TE, soft-float, no runtime CPU detection, not position independent
# (non-PIE executable: no load-time relocations, no GOT indirection).
# --cpu=armv5te + -mtune=arm926ej-s is what the ARM926EJ-S wants; it gives the
# same code as --cpu=arm926ej-s but tags the objects v5TE instead of v5TEJ.
ARM_CPUFLAGS := -mtune=arm926ej-s -mfloat-abi=soft -fno-pie -ffunction-sections -fdata-sections

FF_ARMEL := \
	--enable-cross-compile --cross-prefix=arm-linux-gnueabi- --host-cc=gcc \
	--pkg-config=pkg-config --pkg-config-flags=--static \
	--target-os=linux --arch=arm --cpu=armv5te \
	--enable-armv5te --disable-armv6 --disable-armv6t2 --disable-vfp --disable-neon \
	--disable-runtime-cpudetect --disable-pic --disable-debug \
	--extra-cflags="$(ARM_CPUFLAGS)" \
	--extra-ldflags="-no-pie"

FF_NATIVE := \
	--extra-cflags="-ffunction-sections -fdata-sections"

# libopus: fixed point, no run-time CPU detection, no intrinsics. The inline
# ARMv5E assembly stays; the external assembly file is left out (it is tagged
# as NEON code even when none is assembled, and only speeds up the encoder):
# configure builds it only if it finds perl.
OPUS_ARMEL := \
	--host=arm-linux-gnueabi \
	--enable-fixed-point --disable-rtcd --disable-intrinsics \
	--disable-shared --enable-static --disable-doc --disable-extra-programs --disable-hardening \
	ac_cv_prog_HAVE_PERL=no \
	CFLAGS="-O2 -march=armv5te $(ARM_CPUFLAGS)"

# Tremor (integer Vorbis decoder): its decoder files plus the bit reader of
# libogg, with the hand-written ARM multiplies (_ARM_ASSEM_, plain ARMv4 smull)
TREMOR_FILES  := mdct block window synthesis info floor1 floor0 res012 mapping0 registry codebook sharedbook
TREMOR_CFLAGS := -O2 -march=armv5te $(ARM_CPUFLAGS) -D_ARM_ASSEM_ -w
