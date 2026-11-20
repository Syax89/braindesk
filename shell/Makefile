# brainshell - interfaccia touch di BrainDesk (SHARP Brain PW-SH5)
#
#   make native   per la macchina di sviluppo (prove)           -> $(B)/native/
#   make debug    come native, con AddressSanitizer + UBSan     -> $(B)/debug/
#   make armel    ARMv5 soft-float per il Brain, senza simboli  -> $(B)/armel/
#   make bake     rigenera font e icone in gen/ (serve fonts-dejavu-core)
#   make tests    prove unitarie senza X (evdev, emulatore del terminale, batteria)
#                 -> $(B)/native/evdev_test vt_test batt_test (e in $(B)/debug/ con ASan)
#
# Ogni obiettivo produce due binari dagli stessi sorgenti:
#   brainshell      backend drm + mem: NESSUNA libreria X (solo libc)
#   brainshell-x11  anche il backend x11 (-lX11 -lXext): sviluppo, prove
#                   sotto Xvfb, sessione X di ripiego. brainshell vi passa
#                   da solo (exec) se si trova sotto X.
#
# native/debug/bake vanno eseguiti nell'immagine Docker "brain-shelltest"
# (test/Dockerfile), armel in "brain-userland" (docker/userland/Dockerfile).
# Vedi test/run.sh per i comandi completi.

SRC  = main.c apps.c bar.c home.c sys.c conf.c wmsel.c xapp.c idle_ev.c power.c batt_est.c \
       view_settings.c view_clock.c view_calc.c view_stub.c view_demo.c \
       view_notes.c view_notes_edit.c view_music.c view_music_player.c view_music_tags.c \
       view_video.c media_scan.c media_icons.c \
       view_files.c view_files_text.c view_gallery.c view_gallery_viewer.c bs_img.c bs_work.c \
       view_term.c vt.c keys.c view_keys.c \
       bui.c bui_core.c bui_comp.c bui_drm.c bui_evdev.c bui_mem.c bui_w.c bui_theme.c \
       gen/bui_fonts.c gen/bui_icons.c
SRC_X11 = bui_x.c wm.c idle.c
HDR  = bui.h bui_be.h bui_comp.h bui_evdev.h bui_x.h shell.h media.h batt_est.h gen/bui_assets.h vt.h \
       view_files.h view_gallery.h bs_img.h bs_work.h \
       third_party/stb_image.h third_party/tjpgd.c third_party/tjpgd.h third_party/tjpgdcnf.h

WARN   = -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS = -std=gnu11 -O2 $(WARN)
LIBS   = -pthread
LIBS_X11 = -lX11 -lXext -pthread
DEFS_X11 = -DBUI_HAVE_X11

CC_ARMEL = arm-linux-gnueabi-gcc
# ARM926EJ-S senza FPU: ARMv5TE, virgola mobile software
ARMEL_FLAGS = -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -marm \
              -ffunction-sections -fdata-sections
DEBUG_FLAGS = -std=gnu11 -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer

# cartella dei binari (le prove parallele ne usano un'altra)
B ?= build

DEJAVU ?= /usr/share/fonts/truetype/dejavu
ICONS  ?= tools/material-icons/MaterialIconsRound-Regular

.PHONY: native debug armel bake clean tests

native: $(B)/native/brainshell $(B)/native/brainshell-x11
debug: $(B)/debug/brainshell $(B)/debug/brainshell-x11
armel: $(B)/armel/brainshell $(B)/armel/brainshell-x11
tests: $(B)/native/evdev_test $(B)/debug/evdev_test $(B)/native/batt_test $(B)/debug/batt_test \
       $(B)/native/vt_test $(B)/debug/vt_test

$(B)/native/brainshell: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -g -o $@ $(SRC) $(LIBS)

$(B)/native/brainshell-x11: $(SRC) $(SRC_X11) $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEFS_X11) -g -o $@ $(SRC) $(SRC_X11) $(LIBS_X11)

$(B)/debug/brainshell: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(CC) $(DEBUG_FLAGS) -o $@ $(SRC) $(LIBS)

$(B)/debug/brainshell-x11: $(SRC) $(SRC_X11) $(HDR)
	@mkdir -p $(@D)
	$(CC) $(DEBUG_FLAGS) $(DEFS_X11) -o $@ $(SRC) $(SRC_X11) $(LIBS_X11)

$(B)/armel/brainshell: $(SRC) $(HDR)
	@mkdir -p $(@D)
	$(CC_ARMEL) $(CFLAGS) $(ARMEL_FLAGS) -Wl,--gc-sections -Wl,--as-needed -o $@.unstripped \
		$(SRC) $(LIBS)
	arm-linux-gnueabi-strip -o $@ $@.unstripped
	@ls -l $@

$(B)/armel/brainshell-x11: $(SRC) $(SRC_X11) $(HDR)
	@mkdir -p $(@D)
	$(CC_ARMEL) $(CFLAGS) $(DEFS_X11) $(ARMEL_FLAGS) -Wl,--gc-sections -Wl,--as-needed \
		-o $@.unstripped $(SRC) $(SRC_X11) $(LIBS_X11)
	arm-linux-gnueabi-strip -o $@ $@.unstripped
	@ls -l $@

$(B)/native/evdev_test: test/evdev_test.c bui_evdev.c bui_evdev.h bui.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I. -g -o $@ test/evdev_test.c bui_evdev.c

$(B)/debug/evdev_test: test/evdev_test.c bui_evdev.c bui_evdev.h bui.h
	@mkdir -p $(@D)
	$(CC) $(DEBUG_FLAGS) -I. -o $@ test/evdev_test.c bui_evdev.c

$(B)/native/vt_test: test/vt_test.c vt.c vt.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I. -g -o $@ test/vt_test.c vt.c

$(B)/debug/vt_test: test/vt_test.c vt.c vt.h
	@mkdir -p $(@D)
	$(CC) $(DEBUG_FLAGS) -I. -o $@ test/vt_test.c vt.c

$(B)/native/batt_test: test/batt_test.c batt_est.c batt_est.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I. -g -o $@ test/batt_test.c batt_est.c

$(B)/debug/batt_test: test/batt_test.c batt_est.c batt_est.h
	@mkdir -p $(@D)
	$(CC) $(DEBUG_FLAGS) -I. -o $@ test/batt_test.c batt_est.c

build/bake: tools/bake.c tools/stb_truetype.h
	@mkdir -p $(@D)
	$(CC) -O2 -o $@ tools/bake.c -lm

bake: build/bake
	build/bake $(DEJAVU) $(ICONS).otf $(ICONS).codepoints gen

clean:
	rm -rf build
