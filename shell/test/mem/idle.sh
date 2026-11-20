# inattivita' dai tempi degli eventi: attenua, spegne, primo tocco che sveglia
# soltanto; anche mentre un'app X ha lo schermo
# il Terminale come app X (ripiego): serve un'app che cede lo schermo
ENV="BRAINSHELL_IDLE_MS=1500,3000 BRAINSHELL_TERMINAL=x11"
scene() {
	echo "wait 2000"
	echo "expect screen=dim"
	echo "tap 4 $((H - 4)) 300"
	echo "expect screen=on"
	echo "wait 3600"
	echo "expect screen=off"
	echo "tap $SET 900"
	echo "expect screen=on view=grid"
	echo "tap $SET 900"
	echo "expect view=settings"
	echo "tap $BACK 700"
	echo "tap $TRM 900"
	echo "expect xapp=running"
	echo "wait 3600"
	echo "expect screen=off"
	echo "key a 300"
	echo "expect screen=on xapp=running"
	echo "key ctrl+alt+Escape 1200"
	echo "expect xapp=idle"
}
after() {
	ctl_log | grep -q "screen off" && echo "CHECK ok       brain-ctl screen off" || { echo "CHECK FALLITO  screen off"; ctl_log; }
	ctl_log | grep -q "screen on" && echo "CHECK ok       brain-ctl screen on" || echo "CHECK FALLITO  screen on"
}
