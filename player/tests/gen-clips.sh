#!/bin/sh
# Generates the test clips with the distribution's ffmpeg (inside the
# brainplay-dev container). Every clip shows a counter and has a beep every
# second, so that a screenshot tells where playback is.
set -e
out=${1:-/work/tests/clips}
mkdir -p "$out"
cd "$out"
FF="ffmpeg -hide_banner -loglevel error -y"

# video source: test pattern with a running timestamp; audio: 1 kHz beeps
src() { # size fps seconds
	echo "-f lavfi -i testsrc2=size=$1:rate=$2:duration=$3 -f lavfi -i sine=frequency=1000:beep_factor=2:sample_rate=${4:-44100}:duration=$3"
}
mk() { # file, then ffmpeg args
	f=$1; shift
	[ -s "$f" ] && return 0
	echo "  $f"
	$FF "$@" "$f"
}

X264="-c:v libx264 -preset veryfast -pix_fmt yuv420p"
AAC="-c:a aac -b:a 96k -ac 2"

mk h264_aac_424x240_25.mp4   $(src 424x240 25 12)  $X264 -b:v 400k -g 50 $AAC
mk h264_aac_640x360_30.mp4   $(src 640x360 30 12)  $X264 -b:v 700k -g 60 $AAC
mk h264_aac_854x480_24.mp4   $(src 854x480 24 12)  $X264 -b:v 1000k -g 48 $AAC
mk h264_aac_1280x720_30.mp4  $(src 1280x720 30 12) $X264 -b:v 2000k -g 60 $AAC
# long: seeking, pause, slave tests
mk h264_aac_640x360_long.mp4 $(src 640x360 25 60)  $X264 -b:v 500k -g 50 $AAC
# baseline profile: every P frame is a reference (nothing to skip), key frame every 2 s
mk h264base_aac_424x240_25.mp4 $(src 424x240 25 20) $X264 -profile:v baseline -b:v 400k -g 50 $AAC
# long GOP (10 s) with B frames
mk h264_longgop_640x360_25.mp4 $(src 640x360 25 30) $X264 -bf 3 -b:v 500k -g 250 $AAC
mk mpeg4_mp3_640x360_25.avi  $(src 640x360 25 12)  -c:v mpeg4 -vtag XVID -bf 2 -b:v 800k -g 50 -c:a libmp3lame -b:a 128k -ac 2
mk mpeg4_mp3_720x576_25.avi  $(src 720x576 25 20)  -c:v mpeg4 -vtag XVID -bf 2 -b:v 1500k -g 25 -c:a libmp3lame -b:a 128k -ac 2
mk msmpeg4v3_mp3_424x240_25.avi $(src 424x240 25 8) -c:v msmpeg4 -b:v 500k -c:a libmp3lame -b:a 128k -ac 2
mk vp8_vorbis_640x360_30.webm $(src 640x360 30 12) -c:v libvpx -b:v 700k -g 60 -deadline realtime -cpu-used 8 -c:a libvorbis -b:a 96k -ac 2
mk h264_opus_640x360_25.mkv  $(src 640x360 25 12 48000) $X264 -b:v 600k -g 50 -c:a libopus -b:a 96k -ac 2
mk h264_ac3_640x360_25.mkv   $(src 640x360 25 10 48000) $X264 -b:v 600k -g 50 -c:a ac3 -b:a 192k -ac 6
mk mpeg2_mp2_720x576_25.mpg  $(src 720x576 25 12 48000) -c:v mpeg2video -b:v 3000k -g 12 -bf 2 -aspect 16:9 -c:a mp2 -b:a 192k -ac 2
mk h264_aac_640x360_25.ts    $(src 640x360 25 12 48000) $X264 -b:v 700k -g 50 $AAC -f mpegts
mk mjpeg_pcm_424x240_15.avi  $(src 424x240 15 8)   -c:v mjpeg -q:v 5 -pix_fmt yuvj422p -c:a pcm_s16le -ac 2
mk flv1_mp3_424x240_25.flv   $(src 424x240 25 8)   -c:v flv -b:v 500k -c:a libmp3lame -b:a 96k -ar 44100 -ac 2
mk h264_noaudio_640x360_25.mp4 -f lavfi -i testsrc2=size=640x360:rate=25:duration=10 $X264 -b:v 600k -g 50
mk h264_odd_854x482_25.mp4   $(src 854x482 25 6)   $X264 -b:v 800k $AAC
mk mpeg1_mp2_352x288_25.mpg  $(src 352x288 25 8)   -c:v mpeg1video -b:v 1150k -c:a mp2 -b:a 128k -ac 2
mk h263_352x288.3gp          $(src 352x288 25 6)   -c:v h263 -b:v 400k -c:a aac -b:a 64k -ac 1 -ar 22050

# audio only (with tags)
TAGS='-metadata title=Canzone\ di\ prova -metadata artist=L’Orchestra\ Città -metadata album=Però\ Già'
asrc() { echo "-f lavfi -i sine=frequency=440:beep_factor=2:sample_rate=${2:-44100}:duration=$1"; }
eval mk tone.mp3   $(asrc 20) -c:a libmp3lame -b:a 128k -ac 2 $TAGS
eval mk tone.flac  $(asrc 15) -c:a flac -ac 2 $TAGS
eval mk tone.ogg   $(asrc 15) -c:a libvorbis -b:a 96k -ac 2 $TAGS
eval mk tone.opus  $(asrc 15 48000) -c:a libopus -b:a 64k -ac 2 $TAGS
eval mk tone.wav   $(asrc 10) -c:a pcm_s16le -ac 2
eval mk tone.m4a   $(asrc 15) -c:a aac -b:a 96k -ac 2 $TAGS
eval mk tone.aac   $(asrc 10) -c:a aac -b:a 96k -ac 2 -f adts
eval mk tone_22k_mono.mp3 $(asrc 10 22050) -c:a libmp3lame -b:a 48k -ac 1
eval mk long.mp3   $(asrc 240) -c:a libmp3lame -b:a 128k -ac 2 $TAGS
ls -la "$out"
