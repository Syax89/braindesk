# tastiera attraverso la mappa evdev vera: ricerca nella griglia, calcolatrice,
# frecce con ripetizione, editor delle note con accentate (Alt) e salvataggio
scene() {
	echo "wait 300"
	echo "type calc"
	echo "shot keys-search"
	echo "key Return 900"
	echo "expect view=calc"
	echo "key 1 2 asterisk 3 Return 7 slash 8 Return"
	echo "shot keys-calc"
	echo "key Escape 700"
	echo "expect view=grid"
	echo "key Right Right Down"
	echo "shot keys-focus"
	echo "tap $NOT 900"
	echo "expect view=notes"
	echo "key n 900"
	echo "type Perché è già così"
	echo "key Return"
	echo "type Città, caffè: 3€ più IVA. À È"
	echo "key ctrl+Home"
	echo "hold Right 1000"
	echo "shot keys-note"
	echo "wait 3500"
	echo "key Escape 800"
	echo "shot keys-notes-list"
	echo "key Escape 800"
	echo "expect view=grid"
}
after() {
	f=$(grep -l "già così" $HOME/Note/*.txt 2>/dev/null | head -1)
	if [ -n "$f" ] && grep -q "Perché è già così" "$f" && grep -q "Città, caffè: 3€ più IVA. À È" "$f"; then
		echo "CHECK ok       nota salvata in UTF-8 con le accentate"
	else
		echo "CHECK FALLITO  nota: $(cat $HOME/Note/*.txt 2>/dev/null | head -5)"
	fi
}
