# Video: elenco, avvio di brainplay come programma esterno, pausa della musica.
D=dev/v
media_files
music_geom
key m; key Return; sleep 0.6; tap $MROW1 1.0; key Escape
key v; key Return; sleep 0.7
shot $D-01-elenco-$GEOM
press 200 132 0.2; shot $D-02-premuto-$GEOM; release 0.6
shot $D-03-avvio-$GEOM
check "la musica viene messa in pausa" grep -q 'cmd pause' $BRAIN_CTL_STATE/player.log
check "brainplay FILE avviato" grep -q 'video /tmp/home/Video/clip.webm$' $BRAIN_CTL_STATE/player.log
check "pausa prima dell'avvio" test "$(grep -n 'cmd pause\|video ' $BRAIN_CTL_STATE/player.log | head -1 | grep -c pause)" = 1
sleep 2.2
key Down; key Down; shot $D-04-fuoco-$GEOM
key Return; sleep 0.5
check "Invio avvia il video scelto con le frecce" grep -q 'video /tmp/home/Video/Il postino.mkv$' $BRAIN_CTL_STATE/player.log
sleep 2.5
echo "figli:"; ps -o pid,stat,comm --ppid $SHELL_PID || true
check "nessuno zombie" test -z "$(ps -o stat= --ppid $SHELL_PID | grep Z)"
echo "--- lettore assente"
kill $SHELL_PID; wait $SHELL_PID 2> /dev/null
BRAIN_PLAYER=/nonexistent/brainplay; export BRAIN_PLAYER
start_shell --open video; sleep 0.5
tap 200 132 0.8; shot $D-05-lettore-assente-$GEOM
check "la shell resta viva" shell_alive
sleep 3.2
echo "--- cartella assente"
rm -rf $HOME/Video
key Escape; key v; key Return; sleep 0.6; shot $D-06-vuota-$GEOM
plog | cut -c1-110
