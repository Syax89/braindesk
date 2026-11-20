# Terminale: il disegno per differenza (celle cambiate disegnate nella tela,
# righe spostate con memmove) deve dare gli stessi pixel di un ridisegno
# completo. Si mandano al terminale i flussi registrati di test/vt (htop, vim
# che scorre e inserisce righe, apt con la regione di scorrimento, i
# frammenti alla vttest, 300 righe di seguito), si cattura lo schermo, si
# esce con Home e si rientra (tutto ridisegnato dalle celle) e si confronta.
ENV="BRAINSHELL_TERM_SHELL=/bin/sh"
FILES="htop vimfile apt frag less"
scene() {
	echo "wait 300"
	echo "tap $TRM 1200"
	for f in $FILES lines; do
		if [ $f = lines ]; then
			echo "type clear; i=0; while [ \$i -lt 300 ]; do i=\$((i+1)); echo riga \$i \$i \$i; done"
		else
			echo "type clear; cat /src/test/vt/$f.raw; sleep 1"
		fi
		echo "key Return 2500"
		echo "shot termdraw-$f-a"
		echo "key HomePage 600"
		echo "tap $TRM 1200"
		echo "shot termdraw-$f-b"
		echo "type printf '\\033[?1049l\\033[0m\\033[r'"
		echo "key Return 500"
	done
}
after() {
	d=$SHOTS/mem/$GEOM
	for f in $FILES lines; do
		# sotto la barra di stato (l'orologio puo' cambiare)
		c=$(( W ))x$(( H - 40 ))+0+40
		n=$(compare -metric AE \( $d/termdraw-$f-a.png -crop $c +repage \) \( $d/termdraw-$f-b.png -crop $c +repage \) null: 2>&1)
		if [ "$n" = 0 ]; then echo "CHECK ok       $f: disegno per differenza = ridisegno completo"
		else echo "CHECK FALLITO  $f: $n pixel diversi"; fi
	done
}
