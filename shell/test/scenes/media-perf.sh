# misure per Note e Musica: rettangoli inviati (BUI_TRACE=1) e CPU a riposo.
#   test/run.sh -e BUI_TRACE=1 854x480 media-perf
log() { tr -d '\000' < /tmp/shell.log | sed 's/^/    /'; : > /tmp/shell.log; }
cpu() { awk '{print $14+$15}' /proc/$SHELL_PID/stat; }
wake() { grep ^voluntary /proc/$SHELL_PID/status | awk '{print $2}'; }
media_files
music_geom
i=1; : > "$HOME/Note/lunga.txt"
while [ $i -le 400 ]; do
	echo "Capoverso $i: nel mezzo del cammin di nostra vita mi ritrovai per una selva oscura." >> "$HOME/Note/lunga.txt"
	i=$((i + 1))
done
key n; key Return; sleep 0.6; log > /dev/null
echo "[apertura di una nota di 33 kB, 400 capoversi]"; tap 200 132 0.5; log
xdotool key ctrl+Home; sleep 0.3; log > /dev/null
echo "[un carattere in mezzo a una riga]"; xdotool key Right Right Right; sleep 0.3; log > /dev/null
xdotool key x; sleep 0.3; log
echo "[un secondo carattere]"; xdotool key y; sleep 0.3; log
echo "[freccia a destra: solo il cursore]"; xdotool key Right; sleep 0.3; log
echo "[freccia giu']"; xdotool key Down; sleep 0.3; log
echo "[Backspace]"; xdotool key BackSpace; sleep 0.3; log
echo "[Invio: le righe sotto scendono]"; xdotool key Return; sleep 0.3; log
echo "[20 caratteri di fila, 12 ms l'uno]"; xdotool type --delay 12 'abcdefghij klmnopqrs'; sleep 0.3; log
echo "[salvataggio automatico (3 s dopo l'ultimo tasto)]"; sleep 3.2; log
echo "--- editor aperto, a riposo per 20 s"
a=$(cpu); v=$(wake); sleep 20
echo "tick di CPU: $(($(cpu) - a)); risvegli: $(($(wake) - v))"; log
key Escape; key Escape; sleep 0.3
key m; key Return; sleep 0.8; log > /dev/null
echo "[tocco su un brano]"; tap $MROW3 0.3; log
echo "--- musica in riproduzione, vista aperta: 4 s"
sleep 1; log > /dev/null
a=$(cpu); v=$(wake); sleep 4
echo "tick di CPU: $(($(cpu) - a)); risvegli: $(($(wake) - v))"; log
tap $MPLAY 0.5; log > /dev/null
echo "--- musica in pausa, vista aperta: 20 s"
a=$(cpu); v=$(wake); sleep 20
echo "tick di CPU: $(($(cpu) - a)); risvegli: $(($(wake) - v))"; log
tap $MPLAY 0.5; key Escape; sleep 0.5; log > /dev/null
echo "--- musica in riproduzione, vista chiusa (griglia): 4 s"
a=$(cpu); v=$(wake); sleep 4
echo "tick di CPU: $(($(cpu) - a)); risvegli: $(($(wake) - v))"; log
grep -E "VmRSS|RssAnon" /proc/$SHELL_PID/status
