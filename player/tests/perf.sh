#!/bin/sh
# Rough forecast of the decoding speed on the Brain, from the build machine.
#
# The ARM binary runs every clip with --benchmark under qemu-arm. To turn
# qemu times into Brain times the script runs tests/calib.c, the CPU colour
# conversion loop of driver/pxpvideo.c, which takes about 120 ms per frame
# on the Brain (README of the project): factor = 120 ms / time under qemu.
# One data point, a different kind of code: take the result as an order of
# magnitude. The real numbers come from "brainplay --benchmark FILE" on the
# device.
#
#   ./build.sh shell   then   sh tests/perf.sh
set -e
cd "$(dirname "$0")/.."
BIN=${BIN:-build/armel/brainplay}
D=tests/clips/perf
mkdir -p $D
FF="ffmpeg -hide_banner -loglevel error -y"
SRC="-f lavfi -i mandelbrot=size=854x480:rate=25:end_scale=0.05 -f lavfi -i sine=frequency=440:sample_rate=44100 -t 8"
mk() { f=$D/$1; shift; [ -s "$f" ] || { echo "  generating $f" >&2; $FF $SRC -pix_fmt yuv420p "$@" "$f"; }; }
A="-c:a aac -b:a 96k -ac 2"
mk h264_baseline_320x180.mp4 -vf scale=320:180 -c:v libx264 -profile:v baseline -preset medium -b:v 300k -g 50 $A
mk h264_baseline_424x240.mp4 -vf scale=424:240 -c:v libx264 -profile:v baseline -preset medium -b:v 450k -g 50 $A
mk h264_main_424x240.mp4     -vf scale=424:240 -c:v libx264 -profile:v main -bf 2 -preset medium -b:v 450k -g 50 $A
mk h264_high_640x360.mp4     -vf scale=640:360 -c:v libx264 -preset medium -b:v 800k -g 50 $A
mk h264_high_854x480.mp4     -c:v libx264 -preset medium -b:v 1200k -g 50 $A
mk mpeg4_320x180.avi         -vf scale=320:180 -c:v mpeg4 -vtag XVID -bf 2 -b:v 400k -g 50 -c:a libmp3lame -b:a 128k
mk mpeg4_424x240.avi         -vf scale=424:240 -c:v mpeg4 -vtag XVID -bf 2 -b:v 600k -g 50 -c:a libmp3lame -b:a 128k
mk mpeg4_640x360.avi         -vf scale=640:360 -c:v mpeg4 -vtag XVID -bf 2 -b:v 1000k -g 50 -c:a libmp3lame -b:a 128k
mk mpeg4_854x480.avi         -c:v mpeg4 -vtag XVID -bf 2 -b:v 1500k -g 50 -c:a libmp3lame -b:a 128k
mk mpeg2_720x576.mpg         -vf scale=720:576 -c:v mpeg2video -bf 2 -b:v 3000k -g 12 -c:a mp2 -b:a 192k
mk vp8_424x240.webm          -vf scale=424:240 -c:v libvpx -b:v 450k -g 50 -deadline good -cpu-used 4 -c:a libvorbis -b:a 96k
mk mjpeg_424x240.avi         -vf scale=424:240 -r 15 -c:v mjpeg -pix_fmt yuvj420p -q:v 6 -c:a pcm_s16le

arm-linux-gnueabi-gcc -O2 -static -o /tmp/calib tests/calib.c
q=$(for i in 1 2 3; do qemu-arm -cpu arm926 /tmp/calib; done | awk '{ s += $5 } END { printf "%.2f", s / NR }')
factor=$(awk "BEGIN { printf \"%.1f\", 120 / $q }")
echo "calibration: conversion loop $q ms under qemu, ~120 ms on the Brain: factor $factor"
echo
echo "speed = seconds of video decoded per second of CPU, on the Brain (> 1: fast enough,"
echo "before the sound and the output take their share); pictures/s = what is left to show"
printf "%-28s %-24s %12s %8s %12s\n" clip mode "ms/picture" "speed" "pictures/s"
for f in $D/*; do
	for mode in "--min-level 0" "--min-level 2" "--min-level 3" "--min-level 3 --lowres 1"; do
		out=$(qemu-arm -cpu arm926 $BIN --benchmark $mode "$f" 2>/dev/null | grep "^video:") || continue
		codec=$(echo "$out" | sed 's/^video: \([a-z0-9]*\) .*/\1/')
		case "$mode:$codec" in
		"--min-level 2:h264"|"--min-level 2:vp8") ;;
		"--min-level 2:"*) continue ;;	# no loop filter to skip
		*"lowres 1:h264"|*"lowres 1:vp8") continue ;;
		esac
		fps=$(echo "$out" | sed 's/.* at \([0-9.]*\) fps).*/\1/')
		ms=$(echo "$out" | sed 's/.* fps, \([0-9.]*\) ms\/frame.*/\1/')
		rt=$(echo "$out" | sed 's/.*), \([0-9.]*\)x real time.*/\1/')
		awk "BEGIN { printf \"%-28s %-24s %12.0f %8.2f %12.1f\n\", \"$(basename $f)\", \"$mode\", $ms * $factor, $rt / $factor, $fps }"
	done
done
echo
echo "audio (share of the Brain's CPU, same factor):"
for f in tests/clips/tone.mp3 tests/clips/tone.m4a tests/clips/tone.ogg tests/clips/tone.opus tests/clips/tone.flac $D/mpeg2_720x576.mpg tests/clips/h264_ac3_640x360_25.mkv; do
	out=$(qemu-arm -cpu arm926 $BIN --benchmark "$f" 2>/dev/null | grep "^audio:") || continue
	pct=$(echo "$out" | sed 's/.*(\([0-9.]*\)% of the CPU).*/\1/')
	codec=$(echo "$out" | sed 's/^audio: \([^ ]*\) .*/\1/')
	awk "BEGIN { printf \"  %-18s %5.1f %%\n\", \"$codec\", $pct * $factor }"
done
out=$(qemu-arm -cpu arm926 $BIN --benchmark --float-vorbis tests/clips/tone.ogg 2>/dev/null | grep "^audio:")
pct=$(echo "$out" | sed 's/.*(\([0-9.]*\)% of the CPU).*/\1/')
awk "BEGIN { printf \"  %-18s %5.1f %%   (FFmpeg's float decoder, for comparison: not used)\n\", \"vorbis float\", $pct * $factor }"
