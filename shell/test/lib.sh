# test/lib.sh - funzioni per pilotare brainshell sotto Xvfb (da "sourcare").
# Usate da test/session.sh dentro l'immagine Docker brain-shelltest.

SRC=${SRC:-/src}
SHOTS=${SHOTS:-$SRC/shots}
BIN=${BIN:-$SRC/build/native/brainshell-x11}
export BRAIN_CTL=$SRC/test/brain-ctl
export BRAIN_CTL_STATE=/tmp/ctl
export BRAIN_SYSROOT=/tmp/sysroot
export HOME=/tmp/home
export BRAIN_PLAYER=${BRAIN_PLAYER-$SRC/test/brainplay}	# finto brainplay

# controlli per le scene: una riga "CHECK ok ..." oppure "CHECK FALLITO ..."
check() {	# check "descrizione" comando...
	d=$1; shift
	if "$@" > /dev/null 2>&1; then echo "CHECK ok       $d"; else echo "CHECK FALLITO  $d"; fi
}
plog() { cat $BRAIN_CTL_STATE/player.log 2> /dev/null; }

media_files() {	# file di esempio per Note, Musica e Video
	mkdir -p "$HOME/Note" "$HOME/Musica/Battisti - Emozioni" "$HOME/Musica/Classica" "$HOME/Video/Vacanze"
	printf 'Lista della spesa\npane, latte, caff\303\250\npomodori e basilico\n' > "$HOME/Note/spesa.txt"
	printf 'Idee per il viaggio\n\nFirenze: Uffizi al mattino, poi Ponte Vecchio.\nPrenotare il treno delle 8:15.\n' > "$HOME/Note/viaggio.txt"
	printf 'Perch\303\251 il cielo \303\250 blu\nLa luce del sole viene diffusa dalle molecole dell\047aria: il blu, che ha lunghezza d\047onda pi\303\271 corta, si sparpaglia di pi\303\271 del rosso.\n' > "$HOME/Note/cielo.txt"
	touch -d '2026-11-18 09:30' "$HOME/Note/viaggio.txt"
	touch -d '2026-03-02 18:00' "$HOME/Note/cielo.txt"
	touch -d '2025-12-24 21:10' "$HOME/Note/spesa.txt"
	# un mp3 con un vero tag ID3v2.3 (TIT2 in UTF-8, TPE1 in Latin-1), gli altri senza tag
	printf 'ID3\003\000\000\000\000\000\054TIT2\000\000\000\007\000\000\003VolareTPE1\000\000\000\021\000\000\000Domenico Modugno\ndur=6\ntitle=Volare\n' > "$HOME/Musica/volare.mp3"
	i=1
	for t in "Fiori rosa, fiori di pesco" "Dolce di giorno" "Il tempo di morire" "Mi ritorni in mente" "7 e 40" "Emozioni" "Dieci ragazze" "Acqua azzurra, acqua chiara"; do
		printf 'dur=%d\nartist=Lucio Battisti\n' $((4 + i)) > "$HOME/Musica/Battisti - Emozioni/$(printf %02d $i) - $t.mp3"
		i=$((i + 1))
	done
	printf 'dur=8\n' > "$HOME/Musica/Classica/Vivaldi - La primavera.flac"
	printf 'dur=8\n' > "$HOME/Musica/Classica/Verdi - Va, pensiero.ogg"
	printf 'dur=5\n' > "$HOME/Musica/registrazione 2.wav"
	printf 'dur=5\n' > "$HOME/Musica/registrazione 10.wav"
	echo "non e' musica" > "$HOME/Musica/copertina.jpg"
	truncate -s 734003200 "$HOME/Video/Il postino.mkv"
	truncate -s 48300000 "$HOME/Video/Vacanze/Sardegna 2025.mp4"
	truncate -s 1300000000 "$HOME/Video/Vacanze/Dolomiti.avi"
	truncate -s 820000 "$HOME/Video/clip.webm"
	touch -d '2026-08-14 12:00' "$HOME/Video/Vacanze/Sardegna 2025.mp4"
	touch -d '2025-02-01 12:00' "$HOME/Video/Vacanze/Dolomiti.avi"
	echo x > "$HOME/Video/appunti.txt"
}

sysroot() {	# sysroot [capacita' [stato [microvolt [usb]]]]
	b=$BRAIN_SYSROOT/sys/class/power_supply
	mkdir -p $b/brain-battery $b/brain-usb $BRAIN_SYSROOT/proc/sys/kernel
	echo "${1:-87}" > $b/brain-battery/capacity
	echo "${2:-Discharging}" > $b/brain-battery/status
	echo "${3:-3984000}" > $b/brain-battery/voltage_now
	echo "${4:-0}" > $b/brain-usb/online
	echo "6.1.70-brain" > $BRAIN_SYSROOT/proc/sys/kernel/osrelease
	printf 'MemTotal:         114688 kB\nMemFree:           31200 kB\nMemAvailable:      66560 kB\n' \
		> $BRAIN_SYSROOT/proc/meminfo
}

start_x() {	# start_x 854x480
	GEOM=$1
	W=${GEOM%x*}; H=${GEOM#*x}
	mkdir -p $HOME $SHOTS /tmp/bin $BRAIN_CTL_STATE
	rm -f $BRAIN_CTL_STATE/*
	# sul Brain: st e xfe. Qui: xterm e xclock con gli stessi nomi.
	printf '#!/bin/sh\nexec xterm -fa "DejaVu Sans Mono" -fs 10 -T Terminale\n' > /tmp/bin/st
	printf '#!/bin/sh\nexec xclock -name xfe -title "Gestore file"\n' > /tmp/bin/xfe
	chmod +x /tmp/bin/st /tmp/bin/xfe
	PATH=/tmp/bin:$PATH
	# le scene x11 provano il Terminale come app esterna (st): il ripiego X
	# della vista integrata, che ha le sue scene nel backend mem (test/mem/term.sh)
	export BRAINSHELL_TERMINAL=${BRAINSHELL_TERMINAL-x11}
	Xvfb :99 -screen 0 ${GEOM}x16 -nolisten tcp > /tmp/xvfb.log 2>&1 &
	export DISPLAY=:99
	for i in $(seq 50); do xdpyinfo > /dev/null 2>&1 && break; sleep 0.1; done
	xsetroot -solid '#30343c'
}

start_wm() {
	jwm -f $SRC/test/jwmrc > /tmp/jwm.log 2>&1 &
	sleep 0.6
}

start_shell() {	# argomenti passati a brainshell
	$SHELL_WRAP $BIN "$@" 2> /tmp/shell.log &
	SHELL_PID=$!
	sleep ${SHELL_WAIT:-0.8}
}

tap() {		# tap x y [pausa dopo]
	xdotool mousemove $1 $2 mousedown 1 sleep 0.07 mouseup 1
	sleep ${3:-0.3}
}

press() { xdotool mousemove $1 $2 mousedown 1; sleep ${3:-0.15}; }
release() { xdotool mouseup 1; sleep ${1:-0.3}; }

drag() {	# drag x0 y0 x1 y1 [passi] [ms per passo]: trascina e rilascia in corsa
	n=${5:-10}; ms=${6:-16}
	cmd="mousemove $1 $2 mousedown 1 sleep 0.03"
	i=1
	while [ $i -le $n ]; do
		x=$(( $1 + ($3 - $1) * i / n )); y=$(( $2 + ($4 - $2) * i / n ))
		cmd="$cmd mousemove $x $y sleep 0.0$ms"
		i=$((i + 1))
	done
	xdotool $cmd mouseup 1
	sleep ${7:-0.3}
}

key() { xdotool key "$@"; sleep 0.25; }

# xwd e non "import": quest'ultimo non cattura i popup (save-under) e disturba i grab
shot() { mkdir -p "$(dirname $SHOTS/$1)"; xwd -root -silent | convert xwd:- $SHOTS/$1.png; }

shell_alive() { kill -0 $SHELL_PID 2> /dev/null; }

finish() {
	if shell_alive; then echo "brainshell: ancora vivo (ok)"; else echo "brainshell: TERMINATO"; fi
	kill $SHELL_PID 2> /dev/null
	wait $SHELL_PID 2> /dev/null
	echo "brainshell: uscita con codice $?"
	echo "--- stderr di brainshell ---"; cat /tmp/shell.log
}

# geometria della vista Musica per le scene (pannello a destra o in basso)
music_geom() {
	if [ $W -gt $H ]; then MCX=$((W - 360)); MCW=348; MY0=$((104 + (H - 96 - 16 - 276) / 2))
	else MCX=12; MCW=$((W - 24)); MY0=$((H - 292)); fi
	MSEEK_Y=$((MY0 + 92)); MBTN_Y=$((MY0 + 176)); MVOL_Y=$((MY0 + 244))
	MPREV="$((MCX + (MCW - 272) / 2 + 36)) $MBTN_Y"
	MPLAY="$((MCX + (MCW - 272) / 2 + 136)) $MBTN_Y"
	MNEXT="$((MCX + (MCW - 272) / 2 + 236)) $MBTN_Y"
	MROW1="200 128"; MROW2="200 192"; MROW3="200 256"; MROW4="200 320"
}
