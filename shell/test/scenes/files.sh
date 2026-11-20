# test/scenes/files.sh - Esplora: elenco, scorciatoie, apertura per tipo,
# azioni (proprieta', rinomina, elimina), lettore di testi, tastiera.
# Salva in shots/dev/f-*.
sh $SRC/test/mkassets.sh $HOME > /dev/null 2>&1
mkdir -p /media/SD/DCIM; echo "/dev/mmcblk1p1 /media/SD vfat rw 0 0" > $BRAIN_SYSROOT/proc/mounts
printf '#!/bin/sh\necho "$@" >> /tmp/player.log\nsleep 1\n' > /tmp/bin/brainplay; chmod +x /tmp/bin/brainplay
D=dev/f-$GEOM
if [ $W -gt $H ]; then
	FIL="755 180"; LX=520; Y0=128; DOC="100 184"; IMM="100 240"; MUS="100 296"; SD="100 464"; MORE="$((W-36))"
else
	FIL="240 348"; LX=200; Y0=272; DOC="180 136"; IMM="296 136"; MUS="412 136"; SD="296 204"; MORE="$((W-36))"
fi
row() { echo "$LX $((Y0 + 64 * $1))"; }
UP="$((W-36)) 68"; EYE="$((W-92)) 68"; BACK="30 68"

tap $FIL 0.5; shot $D-01-home
tap $EYE 0.5; shot $D-02-nascosti
tap $EYE 0.5
press $(row 0); shot $D-03-riga-premuta; release 0.5
shot $D-04-documenti
tap $(row 0) 0.5; shot $D-05-sottocartella
drag $LX $((H-40)) $LX $((Y0+20)) 10 16 1.5; shot $D-06-scorsa
tap $UP 0.5; shot $D-07-su
# azioni dal pulsante "tre puntini" (brain.conf e' la quarta riga)
tap $MORE $(row 3 | cut -d' ' -f2) 0.5; shot $D-08-azioni
key Down; key Down; key Down; key Return; sleep 0.3; shot $D-09-proprieta
tap $BACK 0.4
key Down; key Down; key Return; sleep 0.3; shot $D-10-rinomina
xdotool key BackSpace BackSpace BackSpace BackSpace BackSpace; xdotool type --delay 30 -- "-2.conf"; sleep 0.3
shot $D-11-rinomina-scritto
key Return; sleep 0.6; shot $D-12-rinominato
# testo: file lungo con a capo, scorrimento, file troncato, Latin-1, senza estensione
key v; key Return; sleep 0.6; shot $D-13-testo
drag $LX $((H-60)) $LX $((Y0)) 10 16 1.2; shot $D-14-testo-scorso
key End; shot $D-15-testo-fine
key Escape; sleep 0.3
key g; key Return; sleep 1.0; shot $D-16-testo-troncato
key BackSpace; sleep 0.3
key l; key Return; sleep 0.5; shot $D-17-testo-latin1
key Escape; sleep 0.3
key l; key Return; sleep 0.5; shot $D-18-senza-estensione
key Escape; sleep 0.3
key p; key Return; sleep 0.6; shot $D-19-binario-rifiutato
sleep 2.6
key a; key Return; sleep 0.5; shot $D-20-non-supportato
sleep 2.4
# pressione lunga sulla prima riga (cartella Lavoro) ed eliminazione
key Home
press $(row 0) 0.9; shot $D-21-pressione-lunga; release 0.4
key Down; key Down; key Down; key Down; key Return; sleep 0.4; shot $D-22-conferma-elimina-cartella
key Right; key Right; key Return; sleep 1.0; shot $D-23-cartella-eliminata
sleep 2.4
[ -e $HOME/Documenti/Lavoro ] && echo "ERRORE: Lavoro esiste ancora"
[ -e $HOME/Documenti/brain-2.conf ] || echo "ERRORE: rinomina non riuscita"
# immagini e multimedia
tap $IMM 0.5; shot $D-24-immagini
key t; key Return; sleep 2.0; shot $D-25-immagine
key Right; sleep 1.5; shot $D-26-immagine-successiva
key Escape; sleep 0.4; shot $D-27-ritorno
tap $MUS 0.5; tap $(row 1) 0.6; shot $D-28-audio-lanciato
sleep 2.5
tap $SD 0.5; shot $D-29-scheda-sd
key BackSpace; key BackSpace; sleep 0.4; shot $D-30-radice
key Down; key Down; key Left; shot $D-31-fuoco-scorciatoie
cat /tmp/player.log
ps -o stat,comm | grep -c '^Z' | sed 's/^/zombie: /'
key Escape; sleep 0.3; shot $D-32-chiusa
# schermate definitive
[ $W -gt $H ] && P=landscape || P=portrait
pub() { cp $SHOTS/$D-$1.png $SHOTS/$P-$2.png; }
pub 04-documenti 21-esplora
pub 08-azioni 22-esplora-azioni
pub 09-proprieta 23-esplora-proprieta
pub 11-rinomina-scritto 24-esplora-rinomina
pub 13-testo 25-esplora-testo
pub 22-conferma-elimina-cartella 26-esplora-elimina
pub 25-immagine 27-esplora-immagine
pub 28-audio-lanciato 28-esplora-audio
