#!/bin/bash
# test/all-media.sh - le scene di Note, Musica e Video col binario di debug
# (ASan + UBSan) nei due orientamenti. Dalla cartella shell/:
#   test/all-media.sh
# Una scena e' "ok" se nessun CHECK fallisce, brainshell arriva vivo in
# fondo, esce con codice 0 (quindi niente errori o perdite per ASan/LSan) e
# non scrive nulla su stderr.
cd "$(dirname "$0")/.."
fail=0
run() {
	local name="$*" out
	out=$(test/run.sh -d "$@" 2>&1 | tr -d '\000')
	if echo "$out" | grep -q "ancora vivo" && echo "$out" | grep -q "uscita con codice 0" &&
	   ! echo "$out" | grep -q "CHECK FALLITO" &&
	   [ -z "$(echo "$out" | sed '1,/--- stderr di brainshell/d')" ]; then
		echo "ok    $name ($(echo "$out" | grep -c 'CHECK ok') controlli)"
	else
		echo "FALLITA $name"; echo "$out" | grep "CHECK FALLITO"; echo "$out" | tail -30; fail=1
	fi
}
for g in 854x480 480x854; do
	run $g notes
	run $g music
	run $g video
	run $g media
done
exit $fail
