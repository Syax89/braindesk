# tasti del dizionario (keys.c) attraverso la mappa evdev vera: Kokugo = app
# recenti (pressioni ripetute, Invio, tenuto e rilasciato), Eiwa =
# impostazioni rapide (apre e chiude), Rireki = schermata, Brain English =
# Musica, My Dictionary = Note (tenuto: nota nuova), Onsei + tasto =
# luminosita' e volume; Maiusc+Tab resta Tab; Impostazioni -> Tasti cambia
# Rireki in "Apri Calcolatrice" (solo in orizzontale: coordinate della pagina)
scene() {
	echo "wait 700"
	echo "tap $SET 900"
	echo "expect view=settings"
	echo "key Escape 700"
	echo "tap $CLK 900"
	echo "expect view=clock"
	echo "key HomePage 700"
	echo "expect view=grid switcher=0"
	# Kokugo: app recenti (dalla griglia la scelta parte dalla piu' recente)
	echo "key Prior 600"
	echo "expect switcher=1 popups=1 view=grid"
	echo "shot keys-01-recenti"
	echo "key Prior 400"
	echo "key Return 900"
	echo "expect view=settings switcher=0 popups=0"
	# da una vista la prima pressione sceglie la precedente (come Alt+Tab)
	echo "key Prior 500"
	echo "expect switcher=1"
	echo "key Escape 500"
	echo "expect switcher=0 view=settings"
	echo "hold Prior 800"
	echo "wait 700"
	echo "expect view=clock switcher=0"
	# Eiwa: impostazioni rapide, di nuovo per chiuderle
	echo "key Next 700"
	echo "expect quick=1"
	echo "shot keys-02-rapide"
	echo "key Next 700"
	echo "expect quick=0 view=clock"
	# Rireki: schermata (brain-ctl screenshot dopo 400 ms, poi un avviso)
	echo "key Delete 1500"
	# Brain English: nessun brano caricato -> Musica
	echo "key Tab 900"
	echo "expect view=music"
	# My Dictionary: Note; tenuto mezzo secondo: nota nuova
	echo "key Insert 900"
	echo "expect view=notes"
	echo "hold Insert 900"
	echo "wait 400"
	echo "type nota dal tasto"
	# nell'editor i tasti restano tasti: Tab scrive, Canc cancella, niente schermata
	echo "key Tab Delete Prior 400"
	echo "expect view=notes switcher=0 quick=0"
	echo "key HomePage 800"
	echo "expect view=grid"
	# Onsei (Alt) + tasto: luminosita' e volume, ovunque
	echo "key alt+Next 500"
	echo "key alt+Prior 400"
	echo "key alt+Prior 600"
	echo "key alt+Delete 500"
	echo "key alt+Insert 500"
	echo "key alt+Tab 600"
	echo "key alt+Tab 600"
	# Maiusc+Tab e Ctrl+PagSu restano tasti normali
	echo "key shift+Tab ctrl+Prior 500"
	echo "expect view=grid switcher=0 quick=0"
	if [ $PORTRAIT = 0 ]; then
		echo "tap $SET 900"
		for i in 1 2; do
			echo "press 600 420 50"
			for y in $(seq 400 -20 100); do echo "move 600 $y 16"; done
			echo "move 600 100 250"
			echo "release 400"
		done
		echo "tap 200 424 900"
		echo "expect view=keys"
		echo "shot keys-03-tasti"
		echo "tap 300 388 700"
		echo "shot keys-04-azioni"
		echo "tap 620 300 700"
		echo "shot keys-05-app"
		echo "tap 200 244 900"
		echo "shot keys-06-tasti-cambiati"
		echo "tap 300 452 700"
		echo "shot keys-07-guida"
		echo "key Escape 500"
		echo "key Escape 900"
		echo "expect view=settings"
		echo "key Delete 900"
		echo "expect view=calc"
	fi
}
after() {
	chk() { if eval "$2"; then echo "CHECK ok       $1"; else echo "CHECK FALLITO  $1"; ctl_log | tail -12; fi; }
	chk "Rireki -> brain-ctl screenshot" 'ctl_log | grep -q screenshot'
	chk "Onsei+Eiwa: luminosita' 80" 'ctl_log | grep -q "brightness 80"'
	chk "Onsei+Kokugo due volte: 60" 'ctl_log | grep -q "brightness 60"'
	chk "Onsei+Rireki: volume 55" 'ctl_log | grep -q "volume 55"'
	chk "Onsei+Brain English: muto e ripristino" '[ "$(ctl_log | grep -o "volume [0-9]*" | tail -2 | tr "\n" ,)" = "volume 0,volume 45," ]'
	chk "My Dictionary tenuto: nota nuova salvata" 'grep -qs "nota dal tasto" $HOME/Note/*.txt'
	if [ $PORTRAIT = 0 ]; then
		chk "Tasti: Rireki = app:calc nel file" 'grep -q "^key_history=app:calc$" $HOME/.config/brainshell/config'
	fi
}
