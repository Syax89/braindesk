#!/bin/sh
# Test suite of brainplay; run it inside the brainplay-dev container:
#   ./build.sh test                 all the tests with build/native/brainplay
#   ./build.sh test slave ui        some of them
#   ./build.sh test --bin build/asan/brainplay
#   ./build.sh test --qemu --bin build/armel/brainplay bench audio
set -e
cd "$(dirname "$0")/.."
[ -s tests/clips/long.mp3 ] || sh tests/gen-clips.sh "$PWD/tests/clips"
exec python3 tests/run.py "$@"
