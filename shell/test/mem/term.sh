# Terminale integrato: un vero /bin/sh su un PTY dentro il contenitore.
# Comandi scritti dalla tastiera evdev, colori, storico trascinato, Ctrl+C
# (tasto Ctrl e Ctrl della barra), Esc al programma, tasti del dizionario che
# restano tasti, Home e ritorno con la sessione viva, uscita e "Riavvia",
# sospensione automatica trattenuta solo da un programma che scrive.
# Lo schermo del terminale e' scritto in $DUMP a ogni fotogramma.
DUMP=/tmp/term-dump.txt
rm -f $DUMP
ENV="BRAINSHELL_TERM_SHELL=/bin/sh BRAINSHELL_TERM_DUMP=$DUMP BRAINSHELL_TERM_AWAKE_MS=2500"
if [ $PORTRAIT = 0 ]; then
	CTRLKEY="354 68"; TABKEY="296 68"; UPKEY="412 68"; TEXT_Y0=140; TEXT_Y1=440
else
	CTRLKEY="110 119"; TABKEY="67 119"; UPKEY="153 119"; TEXT_Y0=260; TEXT_Y1=760
fi
scene() {
	echo "wait 300"
	echo "tap $TRM 1200"
	echo "expect view=terminal term=running"
	echo "type echo ciao \$((6*7)) > \$HOME/uno; echo ciao \$((6*7))"
	echo "key Return 700"
	echo "type printf '\\033[1;31mrosso\\033[0m \\033[38;5;46mverde\\033[0m \\033[7minverso\\033[0m\\n'"
	echo "key Return 700"
	echo "type echo città è €"
	echo "key Return 600"
	echo "sh cp $DUMP /tmp/term-dump-1.txt"
	echo "shot term-01-comandi"
	# 120 righe: lo storico
	echo "type i=0; while [ \$i -lt 120 ]; do i=\$((i+1)); echo riga \$i; done"
	echo "key Return 1500"
	echo "stats"
	# trascinamento verso il basso: righe vecchie (senza inerzia: pausa prima del rilascio)
	echo "press 400 $TEXT_Y0 50"
	for y in $(seq $((TEXT_Y0 + 20)) 20 $TEXT_Y1); do echo "move 400 $y 16"; done
	echo "move 400 $TEXT_Y1 250"
	echo "release 500"
	echo "shot term-02-storico"
	# un tasto torna in fondo (e arriva alla shell)
	echo "key space 500"
	echo "key BackSpace 400"
	# Ctrl+C dal tasto Ctrl ("Page up" sul Brain)
	echo "type sleep 30"
	echo "key Return 500"
	echo "key ctrl+c 600"
	echo "type echo dopo-ctrl-c > \$HOME/due"
	echo "key Return 600"
	# Ctrl della barra a tocco, poi c dalla tastiera
	echo "type sleep 30"
	echo "key Return 500"
	echo "tap $CTRLKEY 300"
	echo "key c 600"
	echo "type echo dopo-barra > \$HOME/tre"
	echo "key Return 600"
	# Esc (Go Back) va al programma, non chiude la vista
	echo "key Escape 500"
	echo "expect view=terminal"
	# tasti del dizionario: dentro il terminale restano tasti
	echo "key Prior 400"
	echo "key Next 400"
	echo "key Delete 400"
	echo "expect view=terminal switcher=0 quick=0"
	# sh non ha l'editor di riga: Esc e i tasti sono finiti nella riga, Ctrl+U la cancella
	echo "key ctrl+u 300"
	# un programma che scrive da solo trattiene la sospensione
	echo "type for i in 1 2 3 4 5 6 7 8 9 10; do echo tic \$i; sleep 0.3; done"
	echo "key Return 2000"
	echo "expect awake=1"
	echo "wait 4000"
	echo "expect awake=0"
	# Home: alla griglia, la sessione resta
	echo "key HomePage 800"
	echo "expect view=grid term=running"
	echo "tap $TRM 1000"
	echo "expect view=terminal term=running"
	echo "shot term-03-ritorno"
	# Tab della barra (sh lo scrive come un carattere qualsiasi)
	echo "tap $TABKEY 300"
	echo "key BackSpace 300"
	# la freccia dell'intestazione chiude la vista
	echo "tap $BACK 800"
	echo "expect view=grid term=running"
	echo "tap $TRM 1000"
	# uscita: messaggio e Riavvia (Invio)
	echo "type exit"
	echo "key Return 1000"
	echo "expect term=exited view=terminal"
	echo "shot term-04-terminato"
	echo "key Return 1200"
	echo "expect term=running"
	echo "type echo riavviato > \$HOME/quattro"
	echo "key Return 800"
	echo "shot term-05-riavviato"
	echo "rss"
}
after() {
	chk() { if eval "$2"; then echo "CHECK ok       $1"; else echo "CHECK FALLITO  $1"; fi; }
	chk "comando eseguito dalla shell vera" 'grep -qx "ciao 42" $HOME/uno'
	chk "Ctrl+C interrompe sleep" 'grep -qx dopo-ctrl-c $HOME/due'
	chk "Ctrl della barra" 'grep -qx dopo-barra $HOME/tre'
	chk "Riavvia: una shell nuova" 'grep -qx riavviato $HOME/quattro'
	chk "schermo scritto" 'test -s $DUMP'
	chk "ultima riga del terminale" 'grep -q "riavviato" $DUMP'
	chk "accentate in UTF-8 sullo schermo" 'grep -q "^città è €$" /tmp/term-dump-1.txt'
	chk "testo dei colori" 'grep -q "^rosso verde inverso$" /tmp/term-dump-1.txt'
}
