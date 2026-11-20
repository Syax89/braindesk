# sospensione automatica: dopo lo spegnimento dello schermo, solo a batteria,
# mai con la musica che suona, un'app X aperta o il cavo collegato
# il Terminale come app X (ripiego st): "un'app X aperta" impedisce la sospensione
ENV="BRAINSHELL_IDLE_MS=0,1500,1500 BRAINSHELL_BATTERY_MS=300 BRAINSHELL_TERMINAL=x11"
B=$BRAIN_SYSROOT/sys/class/power_supply
scene() {
	echo "wait 300"
	# musica: lo schermo si spegne, il sistema no
	echo "tap $MUS 900"
	echo "tap 200 128 1000"
	echo "wait 4000"
	echo "expect screen=off power=idle"
	echo "key a 300"
	echo "key space 300"
	# app X aperta: niente sospensione
	echo "tap $BACK 700"
	# in verticale "In riproduzione" sposta la griglia: Terminale dalla ricerca
	echo "type term"
	echo "key Return 900"
	echo "expect xapp=running"
	echo "wait 4000"
	echo "expect power=idle"
	echo "key ctrl+alt+Escape 1200"
	echo "expect xapp=idle"
	# col cavo: niente sospensione
	echo "sh echo 1 > $B/brain-usb/online; echo Charging > $B/brain-battery/status"
	echo "wait 4000"
	echo "expect screen=off power=idle"
	echo "sh echo 0 > $B/brain-usb/online; echo Discharging > $B/brain-battery/status"
	# staccato il cavo, a schermo gia' spento: entro 30 s (ricontrollo) si sospende
	echo "log INFO cavo staccato, si aspetta la sospensione"
	echo "wait 31000"
	echo "expect power=idle screen=on"
}
after() {
	n=$(ctl_log | grep -c " suspend")
	[ "$n" = 1 ] && echo "CHECK ok       una sola sospensione automatica" || { echo "CHECK FALLITO  sospensioni: $n"; ctl_log | grep -v brightness; }
}
