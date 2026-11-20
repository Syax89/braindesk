# tasto Home (KEY_HOMEPAGE): alla griglia da una vista (la nota si salva),
# dal pannello rapido e da un dialogo, dalla ricerca; a schermo spento
# sveglia soltanto; durante un'app X la chiude
ENV="BRAINSHELL_IDLE_MS=0,2500,0"
scene() {
	echo "wait 300"
	echo "tap $SET 900"
	echo "expect view=settings"
	echo "key HomePage 700"
	echo "expect view=grid"
	echo "tap $NOT 900"
	echo "key n 800"
	echo "type nota da Home"
	echo "key HomePage 900"
	echo "expect view=grid"
	echo "tap $STATUS 800"
	echo "tap $QS_ROT 700"
	echo "expect dialog=1 popups=2"
	echo "key HomePage 600"
	echo "expect dialog=0 popups=0 view=grid"
	echo "type cal"
	echo "key HomePage 500"
	echo "shot homekey-search-cleared"
	echo "tap $CLK 900"
	echo "wait 3000"
	echo "expect screen=off"
	echo "key HomePage 500"
	echo "expect screen=on view=clock"
	echo "key HomePage 700"
	echo "expect view=grid"
	echo "tap $TRM 900"
	echo "expect xapp=running"
	echo "key HomePage 1200"
	echo "expect xapp=idle suspended=0 view=grid"
}
after() {
	grep -l "nota da Home" $HOME/Note/*.txt > /dev/null 2>&1 && echo "CHECK ok       nota salvata uscendo con Home" ||
		echo "CHECK FALLITO  nota non salvata"
	grep -q SIGTERM /tmp/xapp.log && echo "CHECK ok       Home durante l'app X -> SIGTERM" || echo "CHECK FALLITO  app X non chiusa"
}
