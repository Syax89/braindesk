# app X esterne su richiesta, con un finto Xorg (test/fake-xorg):
# cessione e ripresa dello schermo, Ctrl+Alt+Esc, uscita dell'app,
# comando che fallisce, doppio avvio, lettore video
scene() {
	echo "wait 300"
	echo "tap $TRM 100"
	echo "expect xapp=starting suspended=0"
	echo "wait 700"
	echo "expect xapp=running suspended=1"
	echo "shot xapp-handover"
	# a schermo ceduto i tocchi non arrivano alla shell
	echo "tap $SET 600"
	echo "expect view=grid"
	echo "key ctrl+alt+Escape 1200"
	echo "expect xapp=idle suspended=0 view=grid"
	echo "shot xapp-back"
	# l'app esce da sola dopo un secondo
	echo "sh echo time 1 > /tmp/fake-xorg.next"
	echo "tap $TRM 2200"
	echo "expect xapp=idle suspended=0"
	# il comando non parte (X assente, sudo rifiuta...)
	echo "sh echo fail 127 > /tmp/fake-xorg.next"
	echo "tap $TRM 900"
	echo "expect xapp=idle suspended=0 popups=1"
	echo "shot xapp-failed"
	echo "wait 3500"
	# doppio avvio: il secondo e' rifiutato con un avviso
	echo "tap $TRM 60"
	echo "tap $TRM 900"
	echo "expect xapp=running"
	echo "key ctrl+alt+Escape 1200"
	echo "expect xapp=idle"
	echo "wait 2500"
	# un video dalla vista Video: brainplay in una sessione X
	echo "tap $VID 900"
	echo "tap 200 128 1200"
	echo "expect xapp=running"
	echo "key ctrl+alt+Escape 1200"
	echo "expect xapp=idle view=video"
	echo "shot xapp-video-back"
	echo "tap $BACK 700"
	echo "expect view=grid"
	# una tastiera premuta durante l'app: i modificatori non restano "incollati"
	echo "tap $NOT 900"
	echo "key n 800"
	echo "type ok"
	echo "wait 300"
	echo "shot xapp-end"
}
after() {
	L=/tmp/xapp.log
	n=$(grep -c "^start" $L)
	[ "$n" = 5 ] && echo "CHECK ok       5 avvii" || { echo "CHECK FALLITO  avvii: $n"; cat $L; }
	if grep "^start" $L | grep -qv "shell=suspended"; then echo "CHECK FALLITO  X avviato prima della cessione"; cat $L
	else echo "CHECK ok       X parte sempre a schermo gia' ceduto"; fi
	g="$(( (W - 4) / 8 ))x$(( (H - 4) / 16 ))"
	if grep -q "args: st -f DejaVu Sans Mono:size=10 -g $g" $L; then echo "CHECK ok       st a tutto schermo ($g)"
	else echo "CHECK FALLITO  argomenti di st"; head -3 $L; fi
	grep -q "screen=${W}x${H}" $L && echo "CHECK ok       BRAIN_SCREEN" || echo "CHECK FALLITO  BRAIN_SCREEN"
	[ "$(grep -c SIGTERM $L)" = 3 ] && echo "CHECK ok       Ctrl+Alt+Esc -> SIGTERM (3)" || { echo "CHECK FALLITO  SIGTERM"; cat $L; }
	grep -q "fine dell'app" $L && echo "CHECK ok       uscita dell'app = ritorno" || echo "CHECK FALLITO  uscita dell'app"
	grep -q "args: .*test/brainplay .*Video/" $L && echo "CHECK ok       video in X" || { echo "CHECK FALLITO  video"; cat $L; }
}
