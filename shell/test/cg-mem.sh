#!/bin/sh
# test/cg-mem.sh - istruzioni (callgrind) da main() al primo fotogramma col
# backend mem (lo stesso percorso del backend drm, senza gli ioctl) e, per
# confronto, col backend x11 sotto Xvfb + JWM. Nell'immagine con valgrind:
#   docker run --rm -v "$PWD":/src brain-shelltest-vg test/cg-mem.sh [854x480]
. /src/test/lib.sh
B=${B:-build}
make -s B=$B native 2>&1 | grep -v 'Clock skew\|modification time'
sysroot
mkdir -p $HOME
ir() { grep -a '^totals:' "$1" | tail -1 | awk '{print $2}'; }
G=${1:-854x480}
BRAINSHELL_BACKEND=mem BRAINSHELL_MEM_SIZE=$G BRAINSHELL_EXIT_AFTER_FIRST_FRAME=1 \
	valgrind -q --tool=callgrind --toggle-collect=main --callgrind-out-file=/tmp/cg-mem.out \
	$SRC/$B/native/brainshell 2> /dev/null
echo "mem (= drm senza ioctl), main -> primo fotogramma: $(ir /tmp/cg-mem.out) Ir"
start_x $G
start_wm
BRAINSHELL_EXIT_AFTER_FIRST_FRAME=1 valgrind -q --tool=callgrind --toggle-collect=main \
	--callgrind-out-file=/tmp/cg-x11.out $SRC/$B/native/brainshell-x11 2> /dev/null
echo "x11, main -> primo fotogramma: $(ir /tmp/cg-x11.out) Ir"
