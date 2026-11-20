# misure: avvio, CPU a riposo, rettangoli inviati (BUI_TRACE)
log() { tr -d '\000' < /tmp/shell.log | sed 's/^/    /'; : > /tmp/shell.log; }
echo "--- avvio (ms da main() al primo fotogramma completo sullo schermo)"
grep -a "primo fotogramma" /tmp/shell.log
for i in 1 2 3 4 5; do
	( BRAINSHELL_TIMING=1 $BIN 2>&1 & p=$!; sleep 0.7; kill $p ) | grep -a primo
done
echo "--- caricamento del programma (exec + ld.so + uscita)"
for i in 1 2 3; do /usr/bin/time -f "%e s" $BIN --version; done
echo "--- memoria"
grep -E "VmRSS|RssAnon|RssFile|RssShmem" /proc/$SHELL_PID/status
echo "--- riposo per ${IDLE:-45} s"
: > /tmp/shell.log
a=$(awk '{print $14+$15}' /proc/$SHELL_PID/stat); v=$(grep ^voluntary /proc/$SHELL_PID/status | awk '{print $2}')
date +%T
sleep ${IDLE:-45}
b=$(awk '{print $14+$15}' /proc/$SHELL_PID/stat); v2=$(grep ^voluntary /proc/$SHELL_PID/status | awk '{print $2}')
date +%T
echo "tick di CPU (1/100 s) usati: $((b - a)); risvegli (cambi di contesto volontari): $((v2 - v))"
echo "rettangoli inviati a riposo:"; log
echo "--- rettangoli inviati durante l'uso"
press 99 180 0.2; echo "[riquadro Impostazioni premuto]"; log
release 0.4; echo "[rilasciato: si apre la vista]"; log
press 345 268 0.2; echo "[pulsante Ruota premuto]"; log
xdotool mousemove 345 420; sleep 0.2; echo "[dito uscito dal pulsante]"; log
release 0.3; echo "[rilasciato fuori: annullato]"; log
press 120 204 0.2; echo "[cursore luminosita' premuto]"; log
release 0.3; echo "[cursore rilasciato]"; log
press 380 330 0.2; release 0.3; echo "[interruttore percentuale: premuto e rilasciato]"; log
press 380 330 0.2; release 0.3; echo "[interruttore di nuovo]"; log
tap 30 68; log > /dev/null
tap 263 180 0.2; log > /dev/null; sleep 2.1; echo "[orologio: 2 s]"; log
tap 427 112 0.3; tap 100 436 0.1; log > /dev/null; sleep 1.05; echo "[cronometro in corsa: 1 s]"; log
tap 30 20 0.3; log > /dev/null
echo "[tocco su Home con la griglia gia' visibile]"; tap 30 20 0.4; log
press 780 20 0.2; echo "[stato premuto]"; log; release 0.4; echo "[impostazioni rapide aperte]"; log
press 600 128 0.2; echo "[cursore nel pannello premuto]"; log; release 0.3; log > /dev/null
key Escape; echo "[pannello chiuso]"; log
