#!/bin/sh
# brainplay build wrapper: runs the Makefile inside the brainplay-dev container.
#
#   ./build.sh image          build the container image (player/docker)
#   ./build.sh ffmpeg-armel   minimal static FFmpeg for the Brain
#   ./build.sh armel          build/armel/brainplay (stripped ARM binary)
#   ./build.sh ffmpeg-native  same FFmpeg for the build machine
#   ./build.sh native         build/native/brainplay (with the PXP simulator)
#   ./build.sh asan           build/asan/brainplay (ASan + UBSan)
#   ./build.sh armel-sim      ARM binary with the PXP simulator (qemu tests)
#   ./build.sh test [name..]  run the test suite (tests/run.sh) in the container
#   ./build.sh install        copy build/armel/brainplay to ../os/brainplay
#   ./build.sh shell          interactive shell in the container
#   ./build.sh all            image, both FFmpeg builds, armel, native, asan
#
# Any other argument is passed to make (e.g. ./build.sh clean).
set -e
here=$(cd "$(dirname "$0")" && pwd)
image=${BRAINPLAY_IMAGE:-brainplay-dev}

run() {
	# shellcheck disable=SC2086
	docker run --rm $DOCKER_OPTS -v "$here":/work -w /work "$image" "$@"
}

[ $# -gt 0 ] || set -- armel

case "$1" in
image)
	docker build -t "$image" "$here/docker"
	;;
all)
	docker image inspect "$image" >/dev/null 2>&1 || docker build -t "$image" "$here/docker"
	run make ffmpeg-armel ffmpeg-native
	run make armel native asan armel-sim
	;;
test)
	shift
	run sh tests/run.sh "$@"
	;;
install)
	cp "$here/build/armel/brainplay" "$here/../os/brainplay"
	ls -l "$here/../os/brainplay"
	;;
shell)
	DOCKER_OPTS="$DOCKER_OPTS -it" run bash
	;;
*)
	run make "$@"
	;;
esac
