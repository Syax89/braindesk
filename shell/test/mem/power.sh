# tasto di accensione: breve a batteria = sospensione (finto brain-ctl che
# "dorme" 2 s), eventi accodati prima del risveglio scartati, lungo = menu,
# Sospendi e Spegni dal menu, tasto a schermo spento = sveglia soltanto,
# col cavo USB solo schermo spento, timer dell'Orologio -> sveglia RTC
ENV="BRAINSHELL_IDLE_MS=0,0,0 BRAINSHELL_BATTERY_MS=300"
if [ $PORTRAIT = 0 ]; then M_OFF="139 270"; M_SLEEP="523 270"
else M_OFF="144 389"; M_SLEEP="144 525"; fi
B=$BRAIN_SYSROOT/sys/class/power_supply
scene() {
	echo "wait 300"
	echo "tap $CAL 900"
	echo "expect view=calc"
	echo "key Power 300"
	echo "expect power=sleeping"
	echo "wait 2600"
	echo "expect power=idle screen=on view=calc"
	# un tasto con un tempo di prima del risveglio non conta
	echo "stalekey Escape"
	echo "expect view=calc"
	echo "key Escape 700"
	echo "expect view=grid"
	echo "shot power-resumed"
	# pressione lunga: il menu compare mentre il tasto e' ancora premuto
	echo "hold Power 1800"
	echo "expect power=menu"
	echo "shot power-menu"
	echo "key Escape 400"
	echo "expect power=idle popups=0"
	echo "hold Power 1700"
	echo "tap $M_SLEEP 300"
	echo "expect power=sleeping"
	echo "wait 2600"
	echo "expect power=idle"
	echo "hold Power 1700"
	echo "tap $M_OFF 600"
	echo "expect power=idle"
	# 1,2 s: ne' breve ne' lungo, non fa nulla
	echo "hold Power 1200"
	echo "expect power=idle screen=on"
	# col cavo USB: solo lo schermo; a schermo spento il tasto sveglia soltanto
	echo "sh echo 1 > $B/brain-usb/online; echo Charging > $B/brain-battery/status"
	echo "wait 700"
	echo "key Power 300"
	echo "expect screen=off power=idle"
	echo "key Power 300"
	echo "expect screen=on power=idle"
	echo "key Power 300"
	echo "expect screen=off"
	echo "key a 300"
	echo "expect screen=on view=grid"
	echo "sh echo 0 > $B/brain-usb/online; echo Discharging > $B/brain-battery/status"
	echo "wait 700"
	# un timer dell'Orologio in corso: brain-ctl suspend riceve i secondi rimasti
	echo "tap $CLK 900"
	echo "tap $TAB3 600"
	echo "tap $TM_GO 600"
	echo "key Power 300"
	echo "wait 2600"
	echo "expect power=idle"
}
after() {
	n=$(ctl_log | grep -c " suspend")
	[ "$n" = 3 ] && echo "CHECK ok       3 sospensioni" || { echo "CHECK FALLITO  sospensioni: $n"; ctl_log; }
	[ "$(ctl_log | grep -c ' resume')" = 3 ] && echo "CHECK ok       3 risvegli" || echo "CHECK FALLITO  risvegli"
	ctl_log | grep -q " poweroff" && echo "CHECK ok       Spegni -> brain-ctl poweroff" || echo "CHECK FALLITO  poweroff"
	if ctl_log | grep -E -q " suspend (29[0-9]|300)$"; then echo "CHECK ok       timer -> suspend $(ctl_log | grep -E -o 'suspend [0-9]+$')"
	else echo "CHECK FALLITO  secondi del timer"; ctl_log | grep suspend; fi
}
