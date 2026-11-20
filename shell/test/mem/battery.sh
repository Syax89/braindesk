# batteria: 100% nella striscia, in carica / carica completa, avvisi al 15%
# e al 5%, spegnimento ordinato al 3%; risparmio energetico dal pannello rapido
ENV="BRAINSHELL_BATTERY_MS=300"
Bs=$BRAIN_SYSROOT/sys/class/power_supply
bat() { echo "sh echo $1 > $Bs/brain-battery/capacity; echo $2 > $Bs/brain-battery/status; echo $3 > $Bs/brain-usb/online"; echo "wait 700"; }
scene() {
	echo "wait 300"
	bat 100 Full 1
	echo "shot battery-full"
	bat 64 Charging 1
	echo "shot battery-charging"
	bat 16 Discharging 0
	bat 15 Discharging 0
	echo "expect popups=1"
	echo "shot battery-15"
	echo "wait 5500"
	bat 5 Discharging 0
	echo "expect dialog=1"
	echo "shot battery-5"
	echo "key Escape 500"
	echo "expect dialog=0"
	bat 3 Discharging 0
	echo "wait 500"
	bat 50 Discharging 0
	# risparmio energetico: interruttore in fondo al pannello rapido
	echo "tap $STATUS 900"
	echo "shot battery-quick"
	echo "tap $((W - 70)) 344 600"
	echo "expect saver=1"
	echo "key Escape 400"
	echo "wait 1200"
}
after() {
	ctl_log | grep -q " poweroff" && echo "CHECK ok       3% -> brain-ctl poweroff" || { echo "CHECK FALLITO  poweroff"; ctl_log; }
	[ "$(ctl_log | grep -c ' poweroff')" = 1 ] && echo "CHECK ok       un solo spegnimento" || echo "CHECK FALLITO  piu' spegnimenti"
	ctl_log | grep -q " brightness 40$" && echo "CHECK ok       risparmio: luminosita' 40" || { echo "CHECK FALLITO  luminosita'"; ctl_log | grep bright; }
	grep -q "power_saver=1" $HOME/.config/brainshell/config && grep -q "sleep_after=300" $HOME/.config/brainshell/config &&
		echo "CHECK ok       preferenze salvate" || { echo "CHECK FALLITO  config"; cat $HOME/.config/brainshell/config; }
}
