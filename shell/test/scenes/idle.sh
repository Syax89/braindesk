# test/scenes/idle.sh - schermo attenuato e spento per inattivita', risveglio.
# Va lanciata con -e BRAINSHELL_IDLE_MS=2000,4000 (soglie corte per la prova);
# con -e BRAIN_CTL_NO_SCREEN=1 prova il ripiego sulla luminosita',
# con -e BRAINSHELL_NO_SYNC=1 il ripiego senza l'estensione SYNC.
D=dev/i
calls() { cut -d' ' -f2- $BRAIN_CTL_STATE/log | tr '\n' ';'; echo; : > $BRAIN_CTL_STATE/log; }
sleep 1; calls > /dev/null
xdotool mousemove 400 300; sleep 0.2; : > $BRAIN_CTL_STATE/log
sleep 2.6; echo "dopo 2,6 s fermi (atteso: brightness 21):"; calls
check "attenuato dopo la prima soglia" grep -qx 21 $BRAIN_CTL_STATE/brightness
xdotool mousemove 420 300; sleep 0.6; echo "movimento (atteso: brightness 70):"; calls
check "luminosita' ripristinata" grep -qx 70 $BRAIN_CTL_STATE/brightness
sleep 4.8; echo "dopo 4,8 s fermi (atteso: attenuato, poi spento):"; calls
if [ -z "$BRAIN_CTL_NO_SCREEN" ]; then check "schermo spento" grep -qx off $BRAIN_CTL_STATE/screen
else check "spento con la luminosita' a zero" grep -qx 0 $BRAIN_CTL_STATE/brightness; fi
# il primo tocco sveglia soltanto: cade sul riquadro Impostazioni ma non lo apre
tap 99 180 0.7; echo "tocco a schermo spento:"; calls
shot $D-01-sveglio
[ -n "$BRAIN_CTL_NO_SCREEN" ] || check "schermo riacceso" grep -qx on $BRAIN_CTL_STATE/screen
check "luminosita' ripristinata dopo lo spegnimento" grep -qx 70 $BRAIN_CTL_STATE/brightness
xwd -root -silent | convert xwd:- -crop 300x40+20+48 +repage /tmp/a.png
tap 99 180 0.7; shot $D-02-secondo-tocco-apre
xwd -root -silent | convert xwd:- -crop 300x40+20+48 +repage /tmp/b.png
check "il primo tocco non ha aperto nulla, il secondo si'" sh -c '[ "$(compare -metric AE /tmp/a.png /tmp/b.png null: 2>&1)" != 0 ]'
key Escape
