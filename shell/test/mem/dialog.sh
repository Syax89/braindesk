# dialoghi modali sopra il pannello rapido; conferma che arriva a brain-ctl; toast
scene() {
	echo "wait 300"
	echo "tap $STATUS 800"
	# Spegni apre il menu di spegnimento a tutto schermo; un tocco sul fondo annulla
	echo "tap $QS_PWR 700"
	echo "expect power=menu popups=1"
	echo "shot dialog-power"
	echo "tap 4 $((H - 4)) 600"
	echo "expect power=idle popups=0"
	echo "tap $STATUS 800"
	echo "tap $QS_ROT 700"
	echo "expect dialog=1"
	# Tab porta il fuoco sui pulsanti (Annulla, poi la conferma), Invio conferma
	echo "key Tab Tab"
	echo "shot dialog-focus"
	echo "key Return 900"
	echo "expect dialog=0"
	echo "shot dialog-after"
	echo "tap $STATUS 800"
	echo "tap $QS_SHOT 1500"
	echo "shot dialog-toast"
	echo "expect popups=1"
	echo "wait 3500"
	echo "expect popups=0"
}
after() {
	if ctl_log | grep -q " rotate"; then echo "CHECK ok       Invio conferma -> brain-ctl rotate"
	else echo "CHECK FALLITO  rotate non chiamato"; ctl_log; fi
	if ctl_log | grep -q "screenshot"; then echo "CHECK ok       schermata chiesta"
	else echo "CHECK FALLITO  screenshot non chiamato"; fi
}
