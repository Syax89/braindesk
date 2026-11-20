# prove varie: inerzia fermata dal tocco, molte finestre, brain-ctl lento, cambio di dimensione
D=dev/m
log() { tr -d '\000' < /tmp/shell.log | sed 's/^/    /'; : > /tmp/shell.log; }
echo "--- 1. un tocco durante l'inerzia la ferma e non apre nulla"
drag 400 420 400 60 5 8 0.05
xdotool mousemove 400 250 mousedown 1 sleep 0.05 mouseup 1
sleep 0.15; shot $D-01-caught-a
sleep 1.0; shot $D-01-caught-b
n=$(compare -metric AE $SHOTS/$D-01-caught-a.png $SHOTS/$D-01-caught-b.png null: 2>&1)
[ "$n" = 0 ] && echo "fermo dopo il tocco: OK" || echo "ANCORA IN MOVIMENTO ($n pixel diversi)"
xwininfo -root -tree | grep -c 'brainshell-home' 
key Home; drag 400 100 400 460 6 12 1.2; drag 400 100 400 460 6 12 1.2; drag 400 100 400 460 6 12 1.2
echo "--- 2. brain-ctl lento (2 s): il tocco risponde subito lo stesso"
: > /tmp/shell.log
tap 780 20 0.5
t0=$(date +%s%N)
drag 560 128 760 128 8 16 0.1
key Escape
press 99 180 0.25
t1=$(date +%s%N)
echo "tempo per trascinare il cursore, chiudere e premere un riquadro: $(( (t1 - t0) / 1000000 )) ms"
shot $D-02-pressed-while-ctl-busy
grep -a "put.*home 148x152" /tmp/shell.log | head -2
release 0.3; key Escape
sleep 5
echo "chiamate a brain-ctl:"; cat $BRAIN_CTL_STATE/log
echo "--- 3. molte finestre"
for i in 1 2 3 4 5 6 7; do xclock -title "Finestra numero $i" & sleep 0.25; done; sleep 1
shot $D-03-many-windows
tap 30 20 0.6
shot $D-04-many-windows-home
