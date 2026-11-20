# brainplay

The media player of BrainDesk (SHARP Brain PW-SH5: i.MX283, ARM926EJ-S at
454 MHz, no FPU, one core, 112 MB of RAM).

One C program, statically linked against a minimal FFmpeg. It decodes in its
own threads and hands every frame straight to the hardware: the picture goes
from the decoder to the PXP source buffer (one copy) and the PXP writes it,
converted and scaled, directly into the frame buffer the panel is showing.
No `ffprobe`, no pipes, no shell.

```
brainplay [options] FILE
```

## What is in here

| Path | |
|---|---|
| `src/` | the player (about 6000 lines of C, gnu11, plus the generated `assets.c`) |
| `brain_pxp_uapi.h` | copy of `linux-brain/include/uapi/misc/brain_pxp.h` |
| `ffmpeg.mk` | versions and configuration of FFmpeg, libopus and Tremor |
| `Makefile`, `build.sh` | build inside the `brainplay-dev` container |
| `docker/Dockerfile` | Debian 13 image: armel cross compiler, native compiler, test tools |
| `tools/genassets.py` | bakes fonts and icons into `src/assets.c` |
| `doc/` | four screenshots from the test suite |
| `tests/` | test suite (`run.py`), clip generator, performance forecast |
| `third_party/` | downloaded sources and their build output (FFmpeg 9.0.2, libopus 1.6.1, Tremor, libogg) |
| `build/armel/brainplay` | the binary for the Brain (also copied to `os/brainplay`) |

## Build

Everything runs in a container (Docker / Colima); `player/` is mounted on
`/work` because FFmpeg's build system cannot live in a path with spaces.

```sh
./build.sh image          # once: build the brainplay-dev image
./build.sh ffmpeg-armel   # libopus + FFmpeg for the Brain  (about 4 minutes)
./build.sh armel          # build/armel/brainplay, stripped
./build.sh install        # copy it to ../os/brainplay

./build.sh ffmpeg-native  # the same FFmpeg configuration for the build machine
./build.sh native         # build/native/brainplay (with the PXP simulator)
./build.sh asan           # build/asan/brainplay  (-fsanitize=address,undefined)
./build.sh armel-sim      # ARM binary with the PXP simulator, for qemu
./build.sh test           # the test suite (see "Tests")
./build.sh all            # all of the above except test and install
```

On the Brain: `sudo install -m 755 brainplay /usr/local/bin/`. The user must
be in the groups `video` (for `/dev/pxp`) and `audio`.

### The binary

```
build/armel/brainplay   4.4 MB stripped, ELF EXEC (not PIE), ARM EABI5 soft-float
NEEDED                  libasound.so.2 libX11.so.6 libXext.so.6 libm.so.6 libc.so.6
Tag_CPU_arch            v5TE   (no Tag_FP_arch, no Tag_Advanced_SIMD_arch)
dynamic relocations     about 225, all lazy PLT slots but 5; no relative relocations
```

* `libm.so.6` is on the list because glibc's `libm.a` cannot be linked into a
  dynamic executable on ARM (it needs the static-only `_dl_hwcap`). It costs
  nothing: `libasound.so.2` loads it anyway.
* The executable is not position independent on purpose: no load-time
  relocations for FFmpeg's many tables and no GOT indirection on a CPU
  where that is not free.
* The disassembly contains ARMv5TE instructions only (checked: no `ldrex`,
  `uxtb`, `rev`, `movw`, no VFP/NEON mnemonic), and every decoder was run
  under `qemu-arm -cpu arm926`.

### FFmpeg configuration

FFmpeg 9.0.2 (`ffmpeg.mk` is the reference):

```
configure
  --disable-everything --disable-autodetect --disable-iconv
  --disable-programs --disable-doc
  --disable-avdevice --disable-avfilter --disable-swscale --disable-network
  --disable-shared --enable-static
  --enable-hardcoded-tables
  --enable-protocol=file
  --enable-demuxer=mov,matroska,avi,mpegts,mpegps,flv,mp3,ogg,flac,wav,aac
  --enable-demuxer=mpegvideo,h264,m4v,ac3
  --enable-decoder=h264,mpeg4,msmpeg4v2,msmpeg4v3,h263,flv,mpeg1video,mpeg2video,mjpeg,vp8
  --enable-decoder=aac_fixed,mp3,mp2,ac3_fixed,vorbis,flac
  --enable-libopus --enable-decoder=libopus
  --enable-decoder=pcm_s16le,pcm_s16be,pcm_u8,pcm_s8,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be
  --enable-decoder=pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,pcm_dvd,pcm_bluray
  --enable-decoder=adpcm_ms,adpcm_ima_wav,adpcm_ima_qt
  --enable-parser=h264,mpeg4video,mpegvideo,mpegaudio,aac,ac3,vp8,vorbis,opus,flac,mjpeg,h263
  --enable-cross-compile --cross-prefix=arm-linux-gnueabi- --host-cc=gcc
  --pkg-config=pkg-config --pkg-config-flags=--static
  --target-os=linux --arch=arm --cpu=armv5te
  --enable-armv5te --disable-armv6 --disable-armv6t2 --disable-vfp --disable-neon
  --disable-runtime-cpudetect --disable-pic --disable-debug
  --extra-cflags="-mtune=arm926ej-s -mfloat-abi=soft -fno-pie -ffunction-sections -fdata-sections"
  --extra-ldflags="-no-pie"
```

Notes:

* `--cpu=armv5te` plus `-mtune=arm926ej-s` gives the compiler flags
  `-march=armv5te -mtune=arm926ej-s -mfloat-abi=soft`. `--cpu=arm926ej-s`
  would generate the same code but tag the objects `v5TEJ`.
* Not `--enable-small`: speed first (FFmpeg's default `-O3`).
* `--enable-hardcoded-tables`: the tables of the MP3 and AAC decoders are in
  the binary instead of being computed, with emulated floating point, every
  time a file is opened.
* The raw `mpegvideo`, `h264`, `m4v`, `ac3` demuxers are needed because
  MPEG-PS/TS recognise their elementary streams with the probe functions of
  those demuxers (without them an MPEG-2 program stream is taken for MP3).
* LGPL configuration (nothing GPL-only is enabled).

**Fixed-point audio.** Floating point is emulated on this CPU. Measured
under `qemu-arm -cpu arm926`, FFmpeg's float Vorbis decoder is about 19
times slower than Tremor and its float Opus decoder about 12 times slower
than fixed-point libopus: neither could play in real time on the Brain.
So:

| Codec | Decoder | |
|---|---|---|
| AAC (LC, HE, HEv2) | `aac_fixed` | FFmpeg, fixed point |
| MP3, MP2 | `mp3`, `mp2` | FFmpeg; the plain names are the fixed-point decoders (`mp3float`/`mp2float` are not built) |
| AC-3 | `ac3_fixed` | FFmpeg, fixed point; 5.1 is mixed down to stereo inside the decoder |
| Opus | `libopus` | libopus 1.6.1 built with `--enable-fixed-point --disable-rtcd --disable-intrinsics`, inline ARMv5E assembly |
| Vorbis | Tremor | `libvorbisidec` (the snapshot Debian 13 ships) + libogg's bit reader, compiled here with `-D_ARM_ASSEM_`; driven by `src/tremor.c`, not through libavcodec |
| Vorbis with more than 2 channels | `vorbis` | FFmpeg's float decoder, as a fallback only (slow) |
| FLAC, PCM, ADPCM | `flac`, `pcm_*`, `adpcm_*` | integer |

Video decoders: `h264`, `mpeg4` (DivX/Xvid), `msmpeg4v2`, `msmpeg4v3`,
`h263`, `flv` (Sorenson H.263), `mpeg1video`, `mpeg2video`, `mjpeg`, `vp8`.
Containers: MP4/MOV/3GP/M4A, Matroska/WebM, AVI, MPEG-TS, MPEG-PS, FLV, MP3,
Ogg, FLAC, WAV, ADTS AAC.

## Architecture

```
            reader thread                 video thread (nice +4)
 file ──> av_read_frame ──> video packets ──> decoder ──> frame queue (3) ──┐
                       └──> audio packets ──┐                               │
                                            │      audio thread             │      main thread
                                            └──> decoder ──> S16 stereo     └──> shows each frame when the
                                                 (swresample, volume)            clock reaches its pts:
                                                      │                          copy to the PXP source buffer,
                                                      v                          PXP_IOC_RUN -> panel
                                                 ALSA "default"                  + X events, control bar,
                                                 = master clock                    slave protocol (ppoll)
```

| File | |
|---|---|
| `main.c` | options, start-up order, main loop: X events, UI, slave protocol, frame timing |
| `player.c` | open (with or without stream analysis), packet/frame queues, reader thread, video thread and its frame dropping, seeking, master clock, `--benchmark` |
| `audio.c` | audio thread: decode, convert, volume, clock, start alignment with the picture |
| `ao.c` | audio outputs: ALSA and a timed null sink; pause/resume by replaying the unplayed tail |
| `tremor.c` | integer Vorbis decoder glue |
| `vo.c`, `vo.h` | video output interface, aspect ratio fitting |
| `vo_pxp.c` | PXP outputs: screen overlay and XShm |
| `vo_x11.c`, `yuv2rgb.c` | software output: integer YUV to RGB565, nearest neighbour |
| `pxpdev.c` | `/dev/pxp` (both generations of the kernel interface) |
| `pxpsim.c` | software model of the kernel driver (test builds only) |
| `win.c` | the X11 window (Xlib + XShm only) |
| `ui.c`, `canvas.c`, `assets.c` | control bar, audio view, software drawing, baked fonts and icons |
| `slave.c` | remote control on stdin/stdout |

### Start-up

Aim: first picture well under a second after `exec`.

* small non-PIE binary, 5 shared libraries (9 with their dependencies), no
  load-time relocations of its own;
* the window is created and mapped before the file is opened, so the X
  server and the window manager work while the file is read;
* `avformat_find_stream_info()` (which decodes frames just to describe the
  streams) is skipped when the container already says everything (MP4, MKV,
  AVI, FLAC, WAV, Ogg, MP3 with a Xing header); it runs with small limits
  (`probesize` 1 MiB, `analyzeduration` 1.5 s) for MPEG-TS/PS, FLV and raw
  streams;
* the first picture is shown as soon as it is decoded, whatever the clock
  says; the audio device is opened after that (alsa-lib's configuration
  parsing alone is about 8 million instructions) and the sound is lined up
  with that picture;
* no fontconfig/Xft: text is drawn from baked 4-bit alpha glyphs.

`-v` prints where the time goes (`open`, `stream analysis`, `decoders`,
`audio output ... opened in`, `first frame N ms after the start of the
process`: measured from the kernel's process start time, so dynamic linking
is included). `--stats` prints the first-frame time too.

### Video outputs

Behind `struct vo_driver` (`vo.h`):

1. **`pxp`, screen overlay** (`pxp_screen` in the log). `PXP_IOC_SCREEN`
   tells the geometry of the panel's frame buffer; every frame is one
   `PXP_IOC_RUN` with `out_index = PXP_BUF_SCREEN`, `out_width` = screen
   width, `out_y`/`out_height` = the rows of the video area, the picture
   scaled into `dst_*` (multiples of 8, aspect ratio kept, as large as fits)
   and black `background` around it. After decoding there is exactly one
   copy made by the CPU: the planes into the PXP source buffer (which also
   handles odd line sizes, 4:2:2/4:4:4/4:1:1 chroma, grey, and pads the
   size to multiples of 8 by repeating the last column/row).
   X does not know about these pixels, so the overlay is used only while the
   window is mapped, **unobscured** (`VisibilityNotify`), at x = 0 and as
   wide as the screen. The window is black and never repainted in the video
   rows; when X repaints something (expose) the last frame is run through
   the PXP again.
2. **`pxp`, XShm** (`pxp_shm`). The PXP writes into a second DMA buffer,
   which is copied to an XShm image. Chosen automatically when the kernel
   has no `PXP_IOC_SCREEN` (the old, shorter `struct pxp_job` is used then:
   the new `PXP_IOC_RUN` number gets `ENOTTY` from that kernel), when the
   panel geometry does not match the X screen, and, per frame, whenever
   something covers part of the window or the window is not full width.
   `--vo pxp-shm` forces it.
3. **`x11`**. Software conversion and nearest-neighbour scaling to RGB565,
   XShm. Used when `/dev/pxp` does not exist: this is what runs under Xvfb.

XShm outputs wait for the server's completion event before sending the next
image (a frame that arrives while X is still busy is dropped rather than
queued). `--vo null` decodes without showing.

Sources more than 4 times larger than the destination (the PXP scaler stops
at 1/4) are cropped around their centre. For the codecs that can decode at
reduced size (MPEG-1/2/4, H.263 family, MJPEG) `lowres` is chosen
automatically when the source is much larger than the screen (decoded width
at least 70 % of the displayed width), e.g. 1280x720 MPEG-4 is decoded at
640x360; `--lowres N` forces a value.

### Sync and frame dropping

The master clock is the audio device: after every write (20 ms chunks) it is
set to "pts of the last sample written, minus what the device still has
queued" (low-pass filtered, because devices report their delay in whole
periods). Without audio, and after the audio ends, the same clock runs on
`CLOCK_MONOTONIC`. Each frame is shown when the clock reaches its pts.

When the decoder cannot keep up it is sped up step by step (the video thread
looks at how late each frame leaves the decoder):

| Level | |
|---|---|
| 1 | `skip_loop_filter = NONREF`: no deblocking on non-reference frames (H.264, VP8) |
| 2 | `skip_loop_filter = ALL` |
| 3 | `skip_frame = NONREF`: non-reference (B) frames are not decoded |
| catch-up | more than 0.5 s late at level 3: packets are skipped up to the next key frame (a key frame that is late itself is skipped too). The picture freezes and jumps, the sound never stops and the picture never drifts away from it. For intra-only codecs (MJPEG) this simply skips to the first frame that can still be on time. |
| lowres | two catch-ups within 10 s: the decoder is reopened at the next key frame with one more `lowres` step (codecs that have it) |

`AV_CODEC_FLAG2_FAST` is always set, `thread_count` is 1. Levels go back down
one at a time when the decoder has been waiting on a full frame queue for at
least 40 % of the last 2 seconds with no late frame; the wait before the next
attempt doubles (4 s to 120 s) when a step down turns out to be a mistake.
A frame that is already late when a newer one is waiting is not shown.
`--min-level N` starts (and stays) at a level, `--no-framedrop` turns all of
this off.

Seeking goes to the key frame at or before the target (for a step forwards:
only if that is ahead of the current position, otherwise the next one), all
queues and both decoders are flushed, the first picture is shown immediately
(also while paused) and the sound restarts lined up with it: audio before
that picture is dropped, a gap is filled with silence.

### Window and controls

A full-screen window (EWMH `_NET_WM_STATE_FULLSCREEN`, black, no cursor);
under JWM it covers the trays.

```
+----------------------------------------------------------------+
|                 picture (tap = play / pause)                   |
+----------------------------------------------------------------+
| play | 0:42 |======o----------------| 3:10 | vol- | 80 | vol+ | X |
+----------------------------------------------------------------+
```

* Control bar: 56 rows at the bottom (two rows of 56 on screens narrower
  than 640 pixels, i.e. the rotated panel); every target is at least 56
  pixels. Play/pause, elapsed time, seek bar (tap or drag; the seek happens
  on release), total time, volume down / value / up (hold to repeat), close.
* While a video plays the bar hides after 4 seconds and the picture takes
  the whole screen (848x480 for 16:9 instead of 752x424); any touch or key
  brings it back, and it stays while paused. With the overlay this needs no
  repainting tricks: the PXP job simply covers more or fewer rows, and the
  bar is redrawn by X in rows the overlay no longer touches.
  `--bar-timeout 0` keeps the bar always visible.
* A tap while the bar is hidden counts as a tap on the picture.
* Files without pictures: the same window with title, artist and album, a
  large elapsed time and a wide progress bar (tap or drag to seek).
* Keys: Space/Enter play/pause, Left/Right 10 s, PgDn/PgUp 60 s, Home start,
  Up/Down volume, q/Esc quit. Mouse wheel: volume.
* Volume is a software gain on the samples (0-100, quadratic curve): no
  mixer involved.

Fonts: DejaVu Sans baked at 16, 21, 30 and 76 pixels (Latin-1, Latin
Extended-A and some punctuation; other characters are shown as a box).

### Slave mode

`--slave` reads commands from stdin and writes status lines to stdout
(stderr keeps the log). `--no-window` plays the sound only.

```
brainplay --slave --no-window /path/to/song.mp3
```

Commands (one per line):

| | |
|---|---|
| `play` / `pause` / `toggle` | `play` on a file that ended (see `--keep-open`) starts it again |
| `seek <seconds>` | absolute position |
| `seek +<s>` / `seek -<s>` | relative |
| `volume <0-100>`, `volume +<n>`, `volume -<n>` | |
| `status` | print all the status lines again |
| `quit` (or `stop`) | |

Status lines, `key value`:

```
title Canzone di prova        (tag, or the file name without extension)
artist ...                    (if tagged)
album ...                     (if tagged)
video h264 640x360 25.00 fps  (if there is a picture and a window)
audio aac_fixed 44100 Hz 2 ch
dur 240.0                     (seconds; 0.0 = unknown)
volume 100
state playing                 (playing | paused | ended; on every change)
pos 12.3                      (at most twice per second while playing, and
                               right after a seek or a state change)
error <text>                  (unknown command, bad argument, file that cannot be played)
```

At the end of the file: `state ended`, then the player exits with status 0
(with `--keep-open` it stays, paused at the end). If stdin is a pipe or a
socket and the other end closes it, the player quits; SIGTERM/SIGINT/SIGHUP
end it cleanly too. Exit status: 0 normal end, 1 the file cannot be played,
2 bad command line.

### Options

```
--slave              remote control on stdin/stdout
--no-window          no window: sound only
--volume N           software volume 0-100 (default 100)
--start SEC          start at this position
--paused             start paused (the first picture is shown)
--keep-open          stay open at the end of the file
--bar-timeout SEC    hide the control bar after SEC seconds (default 4, 0 = never)
--window WxH         a normal window instead of the full screen
--vo NAME            auto (default), pxp, pxp-shm, x11, null
--ao NAME            alsa (default), null (silent, real time)
--audio-device DEV   ALSA device (default "default")
--no-audio, --no-video
--no-framedrop       never skip pictures
--min-level N        always decode at speed-up level N (0-3)
--lowres N           decode at 1/2^N of the size (default: automatic)
--benchmark          decode as fast as possible, print the speed, exit
--stats              statistics at the end (frames shown/dropped, sync, first frame)
-v / -vv / -q        more / debug / fewer messages
```

For tests: `--debug-sync` (one line per shown frame), `--debug-decode-delay
MS` (sleep after each decoded frame: a slow CPU), `--max-time SEC`,
`--debug-first-frame-exit`, `--float-vorbis`, and in test builds
`--pxp-sim[=legacy]`.

## Tests

`./build.sh test [--bin FILE] [--qemu] [group ...]` runs `tests/run.py` in the
container. Every test starts its own Xvfb (854x480x16, or 480x854) and
drives the player through xdotool and the slave protocol; logs and
screenshots go to `tests/out/`. Clips are generated by `tests/gen-clips.sh`
with the distribution's ffmpeg (a test pattern with a running time code and
one beep per second).

Because there is no Brain in the container, the PXP is replaced by
`src/pxpsim.c`, a model of `drivers/misc/brain_pxp.c` that accepts and
rejects exactly what the driver does (multiples of 8, 1/4 scaling limit,
buffer sizes, `reserved` must be 0, the rules of `PXP_BUF_SCREEN`, the old
and the new `struct pxp_job`). Its "panel" is painted on the X root window
over all windows (`IncludeInferiors`), so a screenshot shows what the panel
would show, and a wrong repaint or an overlay drawn over somebody else's
window is visible.

| Group | What it checks |
|---|---|
| `formats` | 18 clips (H.264+AAC mp4 at 424x240/640x360/854x480/1280x720, baseline, odd size, no audio; MPEG-4+MP3 avi; MS-MPEG4v3; VP8+Vorbis webm; H.264+Opus and H.264+AC-3 5.1 mkv; MPEG-2+MP2 PS; MPEG-1; H.264+AAC TS; MJPEG 4:2:2+PCM; FLV1+MP3; H.263+AAC 3gp) play to the end with every frame shown and in sync, software output |
| `pxp` | the PXP overlay and the XShm fallback (old kernel interface) on the simulator, all sizes |
| `audio` | mp3, flac, ogg, opus, wav, m4a, aac, 22 kHz mono mp3 headless: duration, tags, real time, position reports at most twice per second |
| `slave` | pause (position frozen), seek while paused (new picture shown), play, relative seeks, volume, unknown command, a burst of seeks, `state ended`, `quit`, closed pipe, SIGTERM |
| `keepopen` | `--start`, `--paused`, `--keep-open`, play again from the end |
| `ui` | buttons, tap on the picture, volume hold-repeat, tap and drag on the seek bar, keys, bar hiding and coming back, close |
| `audioview` | the audio view (tap on the big progress bar, tap to pause), the rotated 480x854 layout |
| `seek` | seeking in every container, seek near the end and play out |
| `slow` | a CPU that is too slow (`--debug-decode-delay`): see below |
| `alsa` | ALSA through a PulseAudio null sink (real-time device), the non-pacing ALSA `null` PCM, a device that cannot be opened |
| `wm` | under JWM with trays: full screen above them, overlay in use; without a window manager: another window on top stays intact, the overlay is switched off while it is there and comes back afterwards |
| `idle` | no CPU time at all while paused (video and music), 4 threads |
| `robust` | missing, empty, text, random, truncated and damaged files: exit status 1 or a clean end, never a crash |
| `bench` | `--benchmark` on every clip (meant for `--qemu`: every decoder runs on the emulated ARM926) |

Results on the build machine (Apple silicon, Colima VM):

* `build/native/brainplay`: all 13 groups, 436 checks, 0 failed.
* `build/asan/brainplay` (ASan + UBSan + LeakSanitizer): 436 checks, 0
  failed, no sanitizer report in any log.
* `build/armel/brainplay` under `qemu-arm -cpu arm926`: `bench` (every
  decoder), `audio`, `formats` (software output under Xvfb) pass; ALSA
  checked by hand against the `null` PCM.
* `build/armel-sim/brainplay` under qemu: `pxp`, `slave`, `ui`, `keepopen`,
  `audioview`, `seek`, `slow`, `wm` pass. (`idle` measures the qemu process
  and the video part of `seek` needs the simulator build: neither means
  anything for the release binary under qemu.)

A/V sync, measured as "clock minus pts" at the moment each frame is handed to
the output: mean 0-6 ms, maximum 3-10 ms on all clips (the upper values are
timer overshoot of the VM, not the player).

Slow CPU (`slow` group; 25 fps clips need 40 ms per frame, the sound must
keep real time and the pictures must stay with it):

| Clip | Decode time forced to | Result |
|---|---|---|
| H.264 640x360, 60 s | 25 ms | all 1500 frames, level 0 |
| H.264 with B frames, GOP 10 s | 50 ms | level 3, 400 of 750 frames shown, no key-frame skip, never more than 180 ms late |
| H.264 baseline (no B frames), GOP 2 s | 60 ms | 10 skips to a key frame, 300 of 500 frames, never more than 0.5 s late |
| MPEG-4 720x576 with B frames | 60 ms | level 3, never more than 90 ms late |
| MJPEG 15 fps | 100 ms | 65 of 120 frames, never more than 130 ms late |
| H.264 640x360 | 150 ms | 30 skips to a key frame, never more than 0.67 s late, plays to the end in 60 s |
| H.264 without audio | 60 ms | level 3, in step with the system clock |

## Expected performance on the Brain

Not measured: there is no device in the build environment. What can be said:

**Start-up.** Up to the first picture the process executes about 20 million
instructions natively (a quarter of them in the software colour conversion
that the Brain does not need), about 5 million of them in the dynamic linker;
opening ALSA `default` adds about 8 million (after the first picture). At a
few hundred MIPS that is 0.1-0.2 s of CPU, plus reading the binary and the
file from the SD card and the window manager mapping the window. The first
picture should be on screen in well under a second; `brainplay -v` shows the
real figure.

**Decoding speed.** `tests/perf.sh` runs the ARM binary under qemu and
scales the times with one calibration point (the CPU conversion loop of
`driver/pxpvideo.c`: about 120 ms per frame on the Brain, 3-4.5 ms under
qemu here, i.e. a factor of 25-40). The table uses the pessimistic end
(factor 40) and a Mandelbrot zoom as content; "speed" is seconds of video
decoded per second of CPU, so 1.0 is the break-even before the sound
(about 10-15 % of the CPU for MP3/AAC/Vorbis) and the output take their
share:

| Clip (25 fps) | level 0 | no loop filter (2) | no B frames (3) | level 3 + lowres 1 |
|---|---|---|---|---|
| MPEG-4 ASP 320x180 | 1.4 | | 3.4 | 5.4 |
| MPEG-4 ASP 424x240 | 0.9 | | 2.4 | 3.7 |
| MPEG-4 ASP 640x360 | 0.5 | | 1.3 | 2.0 |
| MPEG-4 ASP 854x480 | 0.3 | | 0.8 | 1.3 |
| MPEG-2 720x576 | 0.3 | | 0.8 | 1.5 |
| H.264 baseline 320x180 | 0.7 | 1.0 | 1.0 | |
| H.264 baseline 424x240 | 0.5 | 0.7 | 0.7 | |
| H.264 main 424x240 (B frames) | 0.35 | 0.45 | 0.6 | |
| H.264 high 640x360 | 0.17 | 0.23 | 0.4 | |
| VP8 424x240 | 0.3 | 0.6 | 0.6 | |
| MJPEG 424x240, 15 fps | 0.9 | | | 1.7 |

Read it as an order of magnitude (the existing demo, MPEG-4 SP 424x240 at
20 fps, plays today through the much heavier ffmpeg pipeline, which says the
real factor is nearer to 25 than to 40). The picture it gives is probably
right, though: MPEG-4 ASP at 424x240 and 20-25 fps is about what this CPU
decodes in real time; H.264 costs twice as much and wants 320x180 baseline
without the loop filter; anything bigger plays with frames dropped (in
sync, with the sound intact). For videos made for the Brain:

```
ffmpeg -i in.mp4 -vf scale=424:-2 -r 20 -c:v mpeg4 -vtag XVID -q:v 5 -bf 0 \
       -c:a libmp3lame -b:a 96k -ac 2 out.avi
```

Measure on the device with

```
brainplay --benchmark FILE                 # video and audio decoding speed
brainplay --benchmark --min-level 2 FILE   # the same without the loop filter
brainplay -v --stats FILE                  # start-up times, PXP time per frame, frames dropped, sync
```

**Output.** With the overlay a frame costs one copy of the planes into
write-combined memory (152 KB for 424x240) and one PXP job (about 6 ms,
during which the CPU is free for the decoder).

## First run on the Brain

```sh
scp os/brainplay brain:/tmp/ && ssh brain sudo install -m 755 /tmp/brainplay /usr/local/bin/

brainplay --version                      # does it start at all (libraries, CPU)
brainplay --benchmark ~/Video/x.mp4      # decoding speed, no output involved
brainplay -v --stats ~/Video/x.mp4       # from the desktop session (DISPLAY set)
```

What `-v` should say with the new kernel:

```
brainplay: video output: PXP, screen overlay
brainplay: PXP: screen overlay, rows 0-423          (0-479 once the bar hides)
brainplay: first frame NNN ms after the start of the process (NNN ms after main)
brainplay: ALSA "default": 44100 Hz, buffer ... frames (250 ms), period ...
...
brainplay: stats: video shown=... decoded=... late-dropped=... key-skips=... max-level=... | sync mean=... max=... | first-frame=...
brainplay: video output pxp_screen: N frames, X ms each (max Y ms), PXP included
```

`PXP: no screen overlay in this kernel (...)` followed by `video output: PXP,
through XShm` means the kernel still has the old interface; `video output:
software conversion to X11` means `/dev/pxp` could not be opened (missing, or
the user is not in the group `video`). If the overlay misbehaves, `--vo
pxp-shm` and `--vo x11` are there to compare, and `--bar-timeout 0` keeps
the geometry fixed.

To make it the desktop's player, point the `.desktop` entry (or the JWM
menu) at `brainplay %f` instead of `brain-video %f`; for the shell's music
view: `brainplay --slave --no-window FILE`.

Starting can be made a little faster for sound by giving alsa-lib less
configuration to parse (`ALSA_CONFIG_PATH=/path/to/minimal.conf`), if that
ever matters.

## Not done / cannot be verified off the device

* brainplay itself has not run on a real Brain (the `PXP_BUF_SCREEN`
  overlay of the kernel was verified on the device separately). Its use of
  `/dev/pxp`, the overlay next to Xorg's ShadowFB, ALSA on the BU26154 and
  all timing are untested on hardware; the PXP code was exercised only against the
  simulator, the ALSA code against PulseAudio's null sink and the ALSA
  `null` PCM.
* The catch-up thresholds (0.5 s, the level controller) were tuned with
  simulated decode delays, not with the real CPU.
* MPEG-TS/PS have no index: a seek resumes at the first key frame *after*
  the target, so a short step back inside one GOP goes nowhere.
* No HE-AAC test clip (Debian's ffmpeg has no HE-AAC encoder); `aac_fixed`
  includes SBR/PS but that path was not exercised.
* Text: Latin scripts only (no CJK glyphs: titles in Japanese show boxes).
* No subtitles, no playlists, no network, no cover art (attached pictures
  are ignored), no rotation by the PXP (the panel rotates).
* Only 16-bit RGB565 X screens (what the Brain and the tests use).
* `libX11` is linked even for `--no-window`; loading it lazily would save a
  few milliseconds per start in the shell's music view.
* The 4-second auto-hide of the bar is on by default; `--bar-timeout 0`
  if you prefer the bar always there.
