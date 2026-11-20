# test/scenes/rice.sh - temi, animazioni a meta', modo modifica, ricerca,
# calendario, "in riproduzione". Salva in shots/dev/z-*.
D=dev/z-$GEOM
[ $W -gt $H ] && P=landscape || P=portrait
[ $W -gt $H ] && L=1 || L=0
if [ $L = 1 ]; then SET="99 180"; CLK="263 180"; CAL="427 180"; T2="263 180"; else SET="86 180"; CLK="240 180"; CAL="394 180"; T2="240 180"; fi
frames() {	# frames nome n: n catture ravvicinate (fotogrammi intermedi)
	for i in $(seq $2); do xwd -root -silent > /tmp/f-$i.xwd; sleep 0.02; done
	for i in $(seq $2); do convert xwd:/tmp/f-$i.xwd $SHOTS/$1-$i.png; done
}
# transizione di apertura e chiusura
xdotool mousemove $SET mousedown 1 sleep 0.05 mouseup 1; frames $D-01-apertura 4
sleep 0.5; shot $D-02-impostazioni
cp $SHOTS/$D-01-apertura-2.png $SHOTS/$P-63-transizione.png
xdotool mousemove 30 68 mousedown 1 sleep 0.05 mouseup 1; frames $D-03-chiusura 4
sleep 0.5
# pannello rapido che scende, calendario
xdotool mousemove $((W - 110)) 20 mousedown 1 sleep 0.05 mouseup 1; frames $D-04-pannello 3
sleep 0.4; xdotool key Escape; frames $D-05-pannello-chiude 2; sleep 0.4
tap $((W - 30)) 20 0.5; shot $D-06-calendario; cp $SHOTS/$D-06-calendario.png $SHOTS/$P-60-calendario.png
key Next; shot $D-07-calendario-mese-dopo
key Escape
# dialogo che compare
tap $((W - 110)) 20 0.5
if [ $L = 1 ]; then xdotool mousemove 520 270; else xdotool mousemove 145 270; fi
xdotool mousedown 1 sleep 0.05 mouseup 1; frames $D-08-dialogo 3
sleep 0.4; key Escape; key Escape
# ricerca da tastiera
key c; shot $D-09-cerca-c; cp $SHOTS/$D-09-cerca-c.png $SHOTS/$P-62-ricerca.png
key a; shot $D-10-cerca-ca
key x; shot $D-11-cerca-nessuna
key Escape; key o; key Return; sleep 0.5; shot $D-12-cerca-apre-orologio
key Escape; sleep 0.4
# modo modifica: tocco lungo, trascina, nascondi
xdotool mousemove $SET mousedown 1; sleep 0.8; shot $D-13-modifica-presa
if [ $L = 1 ]; then xdotool mousemove 300 200 sleep 0.1 mousemove 470 215 sleep 0.2; else xdotool mousemove 200 200 sleep 0.1 mousemove 380 215 sleep 0.2; fi
shot $D-14-modifica-trascina; cp $SHOTS/$D-14-modifica-trascina.png $SHOTS/$P-61-modifica-home.png
xdotool mouseup 1; sleep 0.4; shot $D-15-modifica-lasciato
if [ $L = 1 ]; then tap 310 148 0.4; else tap 288 148 0.4; fi
shot $D-16-modifica-nascosto
key Escape; shot $D-17-home-riordinata
cat $HOME/.config/brainshell/config
# toast che sale
tap $((W - 110)) 20 0.5
if [ $L = 1 ]; then xdotool mousemove 640 270; else xdotool mousemove 266 270; fi
xdotool mousedown 1 sleep 0.05 mouseup 1; sleep 0.75; frames $D-18-toast 3
sleep 3.5
# temi: dalle Impostazioni, a caldo
key i; key Return; sleep 0.5
key Escape; sleep 0.4
sleep 0.5; cat $HOME/.config/brainshell/config
# ogni tema: home, impostazioni, pannello rapido (riavviando con la preferenza scritta)
n=0
for t in teal brace nero chiaro nord aurora; do
	kill $SHELL_PID; wait $SHELL_PID 2> /dev/null
	mkdir -p $HOME/.config/brainshell; printf 'theme=%s\n' $t > $HOME/.config/brainshell/config
	start_shell
	n=$((n + 1))
	shot $D-30-$t-home; cp $SHOTS/$D-30-$t-home.png $SHOTS/$P-5$n-tema-$t.png
	press $CLK 0.2; shot $D-31-$t-premuto; release 0.5; key Escape; sleep 0.4
	key i; key Return; sleep 0.5; shot $D-32-$t-impostazioni
	key Escape; sleep 0.4
	tap $((W - 110)) 20 0.5; shot $D-33-$t-pannello; key Escape; sleep 0.3
done
# cambio a caldo dalle Impostazioni (solo orizzontale: le coordinate dei campioni)
if [ $L = 1 ]; then
	key i; key Return; sleep 0.5
	tap 658 176 0.7; shot $D-40-tema-a-caldo
	tap 716 316 0.5; shot $D-41-animazioni-nessuna
	key Escape; sleep 0.4; shot $D-42-home-dopo
	cat $HOME/.config/brainshell/config
fi
