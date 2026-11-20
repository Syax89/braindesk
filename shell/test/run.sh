#!/bin/bash
# test/run.sh - lancia una scena di prova nell'immagine Docker brain-shelltest.
#
#   test/run.sh [-d] [-e VAR=VAL]... GEOMETRIA SCENA [argomenti di brainshell]
#
#   -d   usa il binario di debug (AddressSanitizer + UBSan)
#   es.  test/run.sh 854x480 all
#        test/run.sh -d -e BRAINSHELL_EXTRA_TILES=14 480x854 scroll
#
# Ricompila prima di partire. Le immagini finiscono in shots/.
set -e
cd "$(dirname "$0")/.."
target=native; envs=()
while [ "${1#-}" != "$1" ]; do
	case "$1" in
	-d) target=debug; envs+=(-e SHELL_WAIT=1.5 -e ASAN_OPTIONS=detect_leaks=1) ;;
	-e) envs+=(-e "$2"); shift ;;
	esac
	shift
done
geom=$1; scene=$2; shift 2
docker image inspect brain-shelltest > /dev/null 2>&1 || docker build -t brain-shelltest test
exec docker run --rm --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
	-e BIN=/src/build/$target/brainshell-x11 "${envs[@]}" -v "$PWD":/src brain-shelltest \
	sh -c "make -s $target 2>&1 | grep -v 'Clock skew\|modification time'; test/session.sh $geom test/scenes/$scene.sh $*"
