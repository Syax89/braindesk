#!/bin/sh
# test/cg.sh - conteggio delle istruzioni con callgrind (dentro un'immagine
# brain-shelltest con valgrind installato):
#   docker run --rm -v "$PWD":/src brain-shelltest-vg test/cg.sh [854x480]
# Stampa le istruzioni (Ir) da main() al primo fotogramma, per un tocco su un
# riquadro (apertura della vista, animazione compresa) e per il ritorno.
# Con una preferenza animations=none in $HOME si misura senza animazioni.
. /src/test/lib.sh
make -s native 2>&1 | grep -v 'Clock skew\|modification time'
sysroot
[ -z "$CG_ANIM" ] || { mkdir -p $HOME/.config/brainshell; echo "animations=$CG_ANIM" > $HOME/.config/brainshell/config; }
start_x ${1:-854x480}
start_wm
ir() { grep -a '^totals:' "$1" | tail -1 | awk '{print $2}'; }
BRAINSHELL_EXIT_AFTER_FIRST_FRAME=1 valgrind -q --tool=callgrind --toggle-collect=main \
	--callgrind-out-file=/tmp/cg-start.out $BIN 2> /dev/null
echo "avvio (main -> primo fotogramma): $(ir /tmp/cg-start.out) Ir"
valgrind -q --tool=callgrind --callgrind-out-file=/tmp/cg-run.out $BIN 2> /tmp/shell.log &
SHELL_PID=$!
sleep 8
# callgrind_control agisce quando il programma esegue qualcosa: un tasto
# muto (Shift) lo sveglia senza fargli fare altro
poke() { xdotool key Shift_L; sleep 0.8; }
zero() { callgrind_control -z > /dev/null 2>&1; poke; }
dump() { callgrind_control -d > /dev/null 2>&1; poke; sleep 0.5; ir $(ls -t /tmp/cg-run.out.* | head -1); }
measure() {	# measure "descrizione" x y: tocco su un riquadro, poi ritorno
	zero; press $2 $3 0.25; release 3
	echo "tocco su $1 (premuto + apertura della vista): $(dump) Ir"
	zero; tap 30 68 3
	echo "ritorno alla griglia da $1: $(dump) Ir"
}
measure Calcolatrice 427 180
measure Impostazioni 99 180
kill $SHELL_PID; wait $SHELL_PID 2> /dev/null
