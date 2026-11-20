# Compilatore per i programmi del Brain: stesso Debian 13 armel del sistema
FROM debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture armel && apt-get update && apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabi libc6-dev-armel-cross pkg-config make \
      libx11-dev:armel libxext-dev:armel libxft-dev:armel && rm -rf /var/lib/apt/lists/*
ENV PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabi/pkgconfig:/usr/share/pkgconfig
WORKDIR /src
