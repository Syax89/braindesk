# brainplay - native media player for BrainDesk (SHARP Brain PW-SH5)
#
# This Makefile is meant to run inside the brainplay-dev container
# (see build.sh), where the player/ directory is mounted on /work: FFmpeg's
# build system does not survive paths with spaces.
#
#   make ffmpeg-armel   minimal static FFmpeg for the Brain (ARMv5TE soft-float)
#   make armel          build/armel/brainplay (stripped)
#   make ffmpeg-native  the same FFmpeg configuration for the build machine
#   make native         build/native/brainplay (+ PXP simulator, for tests)
#   make asan           build/asan/brainplay (-fsanitize=address,undefined)
#   make armel-sim      build/armel-sim/brainplay (ARM + PXP simulator, for qemu)

TP      := third_party
JOBS    ?= $(shell nproc 2>/dev/null || echo 4)

# ---------------------------------------------------------------- FFmpeg ---
include ffmpeg.mk

FFSRC     := $(TP)/ffmpeg-$(FFVER)
OPUSSRC   := $(TP)/opus-$(OPUSVER)
OGGSRC    := $(TP)/libogg-$(OGGVER)
TREMORSRC := $(TP)/tremor-src

$(OPUSSRC)/configure:
	@test -f $(TP)/opus-$(OPUSVER).tar.gz || \
		curl -fL -o $(TP)/opus-$(OPUSVER).tar.gz https://downloads.xiph.org/releases/opus/opus-$(OPUSVER).tar.gz
	tar -C $(TP) -xzf $(TP)/opus-$(OPUSVER).tar.gz
	touch $@

$(TP)/armel/lib/libopus.a: $(OPUSSRC)/configure ffmpeg.mk
	rm -rf $(TP)/build-opus-armel && mkdir -p $(TP)/build-opus-armel
	cd $(TP)/build-opus-armel && ../opus-$(OPUSVER)/configure --prefix=$(CURDIR)/$(TP)/armel $(OPUS_ARMEL)
	$(MAKE) -C $(TP)/build-opus-armel -j$(JOBS)
	$(MAKE) -C $(TP)/build-opus-armel install

opus-armel: $(TP)/armel/lib/libopus.a

$(TREMORSRC)/mdct.c:
	@test -f "$(TP)/libvorbisidec_$(TREMORVER).orig.tar.gz" || \
		curl -fL -o "$(TP)/libvorbisidec_$(TREMORVER).orig.tar.gz" \
		"http://deb.debian.org/debian/pool/main/libv/libvorbisidec/libvorbisidec_$(TREMORVER).orig.tar.gz"
	mkdir -p $(TREMORSRC)
	tar -C $(TREMORSRC) --strip-components=1 -xzf "$(TP)/libvorbisidec_$(TREMORVER).orig.tar.gz"
	touch $@

$(OGGSRC)/src/bitwise.c:
	@test -f $(TP)/libogg-$(OGGVER).tar.gz || \
		curl -fL -o $(TP)/libogg-$(OGGVER).tar.gz https://downloads.xiph.org/releases/ogg/libogg-$(OGGVER).tar.gz
	tar -C $(TP) -xzf $(TP)/libogg-$(OGGVER).tar.gz
	touch $@

# libvorbisidec.a with libogg's bit reader inside; the ogg headers are the
# ones of libogg-dev:armel
$(TP)/armel/lib/libvorbisidec.a: $(TREMORSRC)/mdct.c $(OGGSRC)/src/bitwise.c ffmpeg.mk
	rm -rf $(TP)/build-tremor-armel
	mkdir -p $(TP)/build-tremor-armel $(TP)/armel/lib $(TP)/armel/include/tremor
	for f in $(TREMOR_FILES); do \
		arm-linux-gnueabi-gcc $(TREMOR_CFLAGS) -I$(TREMORSRC) -c -o $(TP)/build-tremor-armel/$$f.o $(TREMORSRC)/$$f.c || exit 1; \
	done
	arm-linux-gnueabi-gcc $(TREMOR_CFLAGS) -c -o $(TP)/build-tremor-armel/bitwise.o $(OGGSRC)/src/bitwise.c
	rm -f $@ && arm-linux-gnueabi-ar rcs $@ $(TP)/build-tremor-armel/*.o
	cp $(TREMORSRC)/ivorbiscodec.h $(TREMORSRC)/config_types.h $(TP)/armel/include/tremor/

tremor-armel: $(TP)/armel/lib/libvorbisidec.a

$(FFSRC)/configure:
	@test -f $(TP)/ffmpeg-$(FFVER).tar.xz || \
		curl -fL -o $(TP)/ffmpeg-$(FFVER).tar.xz https://ffmpeg.org/releases/ffmpeg-$(FFVER).tar.xz
	tar -C $(TP) -xf $(TP)/ffmpeg-$(FFVER).tar.xz
	touch $@

# prefix/lib/libavcodec.a is the stamp of a finished build
$(TP)/armel/lib/libavcodec.a: $(FFSRC)/configure ffmpeg.mk | $(TP)/armel/lib/libopus.a
	rm -rf $(TP)/build-armel && mkdir -p $(TP)/build-armel
	cd $(TP)/build-armel && PKG_CONFIG_LIBDIR=$(CURDIR)/$(TP)/armel/lib/pkgconfig \
		../ffmpeg-$(FFVER)/configure --prefix=$(CURDIR)/$(TP)/armel $(FF_COMMON) $(FF_ARMEL)
	$(MAKE) -C $(TP)/build-armel -j$(JOBS)
	$(MAKE) -C $(TP)/build-armel install

$(TP)/native/lib/libavcodec.a: $(FFSRC)/configure ffmpeg.mk
	rm -rf $(TP)/build-native && mkdir -p $(TP)/build-native
	cd $(TP)/build-native && ../ffmpeg-$(FFVER)/configure --prefix=$(CURDIR)/$(TP)/native $(FF_COMMON) $(FF_NATIVE)
	$(MAKE) -C $(TP)/build-native -j$(JOBS)
	$(MAKE) -C $(TP)/build-native install

ffmpeg-armel:  $(TP)/armel/lib/libavcodec.a
ffmpeg-native: $(TP)/native/lib/libavcodec.a

# ---------------------------------------------------------------- player ---
SRCS := main.c player.c audio.c ao.c vo.c vo_pxp.c vo_x11.c pxpdev.c yuv2rgb.c \
	win.c ui.c canvas.c assets.c slave.c util.c tremor.c
HDRS := $(wildcard src/*.h) brain_pxp_uapi.h

WARN   := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CSTD   := -std=gnu11 -D_GNU_SOURCE -DHAVE_TREMOR -I. -Isrc
# FFmpeg, libopus (fixed point) and Tremor (integer Vorbis)
FFLIBS := -lavformat -lavcodec -lswresample -lavutil -lopus -lvorbisidec
# Shared: only glibc (libc + libm), libasound and the X libraries.
# libm cannot be linked statically into a dynamic executable on ARM (libm.a
# needs the static-only _dl_hwcap), and libasound.so.2 loads libm.so.6 anyway.
DYNLIBS := -lasound -lX11 -lXext -lm

ARM_CC     := arm-linux-gnueabi-gcc
ARM_CFLAGS := -O2 -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -fno-pie \
	-ffunction-sections -fdata-sections $(CSTD) $(WARN) -I$(TP)/armel/include
ARM_LDFLAGS := -no-pie -Wl,--gc-sections -Wl,--as-needed -Wl,-O1 -Wl,--hash-style=gnu \
	-L$(TP)/armel/lib

ARM_DEPS := $(TP)/armel/lib/libavcodec.a $(TP)/armel/lib/libopus.a $(TP)/armel/lib/libvorbisidec.a

build/armel/%.o: src/%.c $(HDRS) $(ARM_DEPS)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -c -o $@ $<

build/armel/brainplay: $(SRCS:%.c=build/armel/%.o)
	$(ARM_CC) $(ARM_LDFLAGS) -o $@.unstripped $^ \
		-Wl,-Bstatic $(FFLIBS) -Wl,-Bdynamic $(DYNLIBS) -Wl,-Map,$@.map
	arm-linux-gnueabi-strip -s -o $@ $@.unstripped
	@ls -l $@ && arm-linux-gnueabi-readelf -d $@ | grep NEEDED

armel: build/armel/brainplay

build/armel-sim/%.o: src/%.c $(HDRS) $(ARM_DEPS)
	@mkdir -p $(@D)
	$(ARM_CC) $(ARM_CFLAGS) -DPXP_SIM -c -o $@ $<

build/armel-sim/brainplay: $(SRCS:%.c=build/armel-sim/%.o) build/armel-sim/pxpsim.o
	$(ARM_CC) $(ARM_LDFLAGS) -o $@ $^ \
		-Wl,-Bstatic $(FFLIBS) -Wl,-Bdynamic $(DYNLIBS)

armel-sim: build/armel-sim/brainplay

NAT_CFLAGS  := -O2 -g $(CSTD) $(WARN) -DPXP_SIM -I$(TP)/native/include
NAT_LDFLAGS := -L$(TP)/native/lib
NAT_LIBS    := $(FFLIBS) -logg $(DYNLIBS) -lpthread

build/native/%.o: src/%.c $(HDRS) $(TP)/native/lib/libavcodec.a
	@mkdir -p $(@D)
	$(CC) $(NAT_CFLAGS) -c -o $@ $<

build/native/brainplay: $(SRCS:%.c=build/native/%.o) build/native/pxpsim.o
	$(CC) $(NAT_LDFLAGS) -o $@ $^ $(NAT_LIBS)

native: build/native/brainplay

SAN := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

build/asan/%.o: src/%.c $(HDRS) $(TP)/native/lib/libavcodec.a
	@mkdir -p $(@D)
	$(CC) $(NAT_CFLAGS) -O1 $(SAN) -c -o $@ $<

build/asan/brainplay: $(SRCS:%.c=build/asan/%.o) build/asan/pxpsim.o
	$(CC) $(NAT_LDFLAGS) $(SAN) -o $@ $^ $(NAT_LIBS)

asan: build/asan/brainplay

# fonts and icons baked into src/assets.c (needs python3-pil + DejaVu)
assets:
	python3 tools/genassets.py src/assets.c

clean:
	rm -rf build/armel build/armel-sim build/native build/asan

distclean: clean
	rm -rf $(TP)/build-armel $(TP)/build-native $(TP)/build-opus-armel $(TP)/build-tremor-armel $(TP)/armel $(TP)/native

.PHONY: opus-armel tremor-armel ffmpeg-armel ffmpeg-native armel armel-sim native asan assets clean distclean
