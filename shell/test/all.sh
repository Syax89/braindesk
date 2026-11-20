#!/bin/bash
# test/all.sh - esegue tutte le scene col binario di debug (ASan + UBSan) in
# entrambi gli orientamenti e riassume l'esito. Dalla cartella shell/:
#   test/all.sh
# Una scena e' "ok" se brainshell arriva vivo in fondo, esce con codice 0 a
# SIGTERM (quindi senza errori o perdite segnalate da ASan/LSan) e non scrive
# nulla su stderr.
cd "$(dirname "$0")/.."
fail=0
run() {
	local name="$*" out
	out=$(test/run.sh -d "$@" 2>&1 | tr -d '\000')
	if echo "$out" | grep -q "ancora vivo" && echo "$out" | grep -q "uscita con codice 0" &&
	   ! echo "$out" | grep -q "CHECK FALLITO" &&
	   [ -z "$(echo "$out" | sed '1,/--- stderr di brainshell/d')" ]; then
		echo "ok    $name"
	else
		echo "FALLITA $name"; echo "$out" | tail -30; fail=1
	fi
}
for g in 854x480 480x854; do
	run $g final
	run -e BRAINSHELL_EXTRA_TILES=9 $g final-grid
	run $g demo --open demo
	run $g files
	run -e THUMB_WAIT=12 $g gallery
done
run 854x480 land1
run 854x480 land2
run 854x480 land3
run 480x854 port1
run -e BRAINSHELL_EXTRA_TILES=14 854x480 scroll
run 854x480 keys
run 854x480 states
run -e BRAINSHELL_EXTRA_TILES=36 -e BRAIN_CTL_DELAY=2 854x480 misc
run -e NO_WM=1 854x480 nowm
run 854x480 resize
for g in 854x480 480x854; do
	run -e BRAINSHELL_EXTRA_TILES=14 $g rice
	run -e BRAINSHELL_EXTRA_TILES=14 $g rice2
done
run -e BRAINSHELL_IDLE_MS=2000,4000 854x480 idle
run -e BRAINSHELL_IDLE_MS=2000,4000 -e BRAIN_CTL_NO_SCREEN=1 480x854 idle
exit $fail
