# impostazioni rapide (cursore della luminosita', conferme), calendario
scene() {
	echo "wait 300"
	echo "tap $STATUS 800"
	echo "expect popups=1 dialog=0"
	echo "shot quick"
	echo "press $QS_X1 $QS_SLIDER_Y 100"
	echo "move $QS_X0 $QS_SLIDER_Y 300"
	echo "shot quick-slider"
	echo "release 600"
	echo "tap $QS_ROT 700"
	echo "expect dialog=1"
	echo "shot quick-rotate-dialog"
	echo "key Escape 600"
	echo "expect dialog=0 popups=1"
	echo "key Escape 600"
	echo "expect popups=0"
	echo "tap $CLOCK 800"
	echo "expect popups=1"
	echo "shot calendar"
	echo "key Escape 600"
	echo "expect popups=0"
	echo "shot quick-end"
}
after() {
	if ctl_log | grep -q "brightness [0-9]"; then echo "CHECK ok       cursore -> brain-ctl brightness"
	else echo "CHECK FALLITO  nessun brain-ctl brightness"; ctl_log; fi
}
