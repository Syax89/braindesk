# brainplay: build + test image.
# Same base as docker/userland (Debian 13 "trixie", the distribution that runs
# on the Brain), with the armel cross compiler, a native compiler (used for the
# FFmpeg table generators and for the native test build) and the test tools.
#
#   docker build -t brainplay-dev player/docker
FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture armel && apt-get update && apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabi libc6-dev-armel-cross binutils-arm-linux-gnueabi \
      gcc libc6-dev make pkg-config patch diffutils file xz-utils bzip2 ca-certificates curl \
      libx11-dev:armel libxext-dev:armel libasound2-dev:armel \
      libx11-dev libxext-dev libasound2-dev \
    && rm -rf /var/lib/apt/lists/*
# fixed-point Vorbis decoder (Tremor) as static libraries, and a native
# libopus for the native test build (the ARM libopus is built from source,
# fixed-point, by "make opus-armel")
RUN apt-get update && apt-get install -y --no-install-recommends \
      libvorbisidec-dev:armel libogg-dev:armel libvorbisidec-dev libogg-dev libopus-dev \
    && rm -rf /var/lib/apt/lists/*
# test tools: X server, window manager, input injection, screenshots, clip
# generation, a real-time ALSA sink (PulseAudio null sink), qemu-user for the
# ARM binary, python+PIL for the font/icon baker
RUN apt-get update && apt-get install -y --no-install-recommends \
      xvfb xauth x11-utils x11-apps x11-xserver-utils xdotool imagemagick jwm \
      ffmpeg alsa-utils libasound2-plugins pulseaudio \
      qemu-user gdb strace valgrind procps psmisc \
      python3 python3-pil fonts-dejavu-core \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
