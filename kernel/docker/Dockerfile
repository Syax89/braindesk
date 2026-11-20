FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi libc6-dev-armel-cross make bc bison flex \
      libssl-dev libelf-dev cpio kmod git ca-certificates python3 rsync \
      gcc libc6-dev xz-utils lz4 lzop u-boot-tools device-tree-compiler && rm -rf /var/lib/apt/lists/*
ENV ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
WORKDIR /work
