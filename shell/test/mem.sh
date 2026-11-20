#!/bin/bash
# test/mem.sh - scene del backend "mem" (nessun X, nessun pannello): la shell
# compone lo schermo in memoria come sul Brain e uno script la guida.
#
#   test/mem.sh [-d] [-q] [-b DIR] [GEOMETRIA [SCENA...]]
#
#   -d   binario di debug (ASan + UBSan)
#   -q   binario armel sotto qemu-arm -cpu arm926 (dopo "make armel")
#   -b   cartella dei binari (default build)
#
# Le scene sono test/mem/*.txt (comandi in bui_mem.c); le schermate vanno in
# shots/mem/. Una scena e' ok se nessun CHECK fallisce, la shell esce con 0 e
# non scrive nulla su stderr (ASan/LSan compresi).
cd "$(dirname "$0")/.."
target=native; wrap=; B=build
while [ "${1#-}" != "$1" ]; do
	case "$1" in
	-d) target=debug ;;
	-q) target=armel; wrap="qemu-arm -cpu arm926" ;;
	-b) B=$2; shift ;;
	esac
	shift
done
geom=${1:-854x480}; shift
scenes=${*:-home views quick scroll dialog keys xapp idle power autosleep battery homekey}
docker image inspect brain-shelltest > /dev/null 2>&1 || docker build -t brain-shelltest test
exec docker run --rm -e B=$B -e TARGET=$target -e WRAP="$wrap" -e GEOM=$geom -e SCENES="$scenes" -e VERBOSE \
	-v "$PWD":/src brain-shelltest test/mem-run.sh
