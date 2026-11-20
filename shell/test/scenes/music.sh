# Musica: libreria, comandi al lettore (finto brainplay), avanzamento
# automatico anche a vista chiusa, errori, pulizia dei processi.
D=dev/mu
media_files
music_geom
players() { pgrep -f "test/brainplay --slave" | wc -l; }
restart() { kill $SHELL_PID; wait $SHELL_PID 2> /dev/null; start_shell "$@"; }

key m; key Return; sleep 0.7
shot $D-01-libreria-$GEOM
check "nessun lettore prima del primo tocco" test "$(players)" = 0

echo "--- riproduzione, pausa, ripresa"
tap $MROW1 1.3
check "avvio del primo brano" grep -q 'start registrazione 2.wav' $BRAIN_CTL_STATE/player.log
shot $D-02-in-riproduzione-$GEOM
tap $MPLAY 0.4
check "pausa" grep -q 'cmd pause' $BRAIN_CTL_STATE/player.log
shot $D-03-in-pausa-$GEOM
tap $MPLAY 0.4
check "ripresa" grep -q 'cmd play' $BRAIN_CTL_STATE/player.log

echo "--- cursore di posizione e volume"
press $((MCX + MCW / 2)) $MSEEK_Y 0.2; shot $D-04-cursore-premuto-$GEOM
check "nessun seek finche' il dito non si alza" test "$(grep -c 'cmd seek' $BRAIN_CTL_STATE/player.log)" = 0
xdotool mousemove $((MCX + MCW * 3 / 4)) $MSEEK_Y; sleep 0.2; release 0.4
check "seek al rilascio (un solo comando)" test "$(grep -c 'cmd seek [34]' $BRAIN_CTL_STATE/player.log)" = 1
tap $((MCX + 44 + (MCW - 48) / 4)) $MVOL_Y 0.4
check "volume" grep -q 'cmd volume [12][0-9]$' $BRAIN_CTL_STATE/player.log
shot $D-05-volume-$GEOM

echo "--- successivo / precedente"
tap $MNEXT 0.8
check "successivo: parte il secondo brano" grep -q 'start registrazione 10.wav' $BRAIN_CTL_STATE/player.log
check "il lettore precedente e' stato fermato" grep -q 'quit registrazione 2.wav\|term registrazione 2.wav' $BRAIN_CTL_STATE/player.log
check "il volume scelto passa al nuovo processo" test "$(grep -c 'cmd volume' $BRAIN_CTL_STATE/player.log)" = 2
check "un solo lettore attivo" test "$(players)" = 1
tap $MPREV 0.8
check "precedente (entro 3 s): torna al primo" test "$(grep -c 'start registrazione 2.wav' $BRAIN_CTL_STATE/player.log)" = 2

echo "--- tag ID3 e titolo dal lettore"
tap $MROW3 1.2
check "avvio di volare.mp3" grep -q 'start volare.mp3' $BRAIN_CTL_STATE/player.log
shot $D-06-id3-$GEOM

echo "--- la musica continua fuori dalla vista e passa al brano successivo"
key Escape; sleep 0.4; shot $D-07-griglia-con-musica-$GEOM
check "uscendo dalla vista il lettore resta" test "$(players)" = 1
sleep 6.5
check "fine brano a vista chiusa: parte il successivo" grep -q 'start 01 - Fiori rosa' $BRAIN_CTL_STATE/player.log
key m; key Return; sleep 0.7
shot $D-08-rientro-$GEOM

echo "--- tastiera: Spazio, N, P, frecce"
n0=$(grep -c 'cmd pause' $BRAIN_CTL_STATE/player.log)
key space
check "Spazio mette in pausa" test "$(grep -c 'cmd pause' $BRAIN_CTL_STATE/player.log)" = $((n0 + 1))
key space
key n; sleep 0.5
check "N: brano successivo" grep -q 'start 02 - Dolce di giorno' $BRAIN_CTL_STATE/player.log
key Down; key Down; key Return; sleep 0.6
shot $D-09-fuoco-elenco-$GEOM
key Right; key Right; shot $D-10-fuoco-pannello-$GEOM
check "un solo lettore attivo dopo vari cambi" test "$(players)" = 1
echo "figli (nessuno deve essere Z):"; ps -o pid,stat,comm --ppid $SHELL_PID || true
check "nessuno zombie" test -z "$(ps -o stat= --ppid $SHELL_PID | grep Z)"

echo "--- file che il lettore non riesce ad aprire"
echo broken > "$HOME/Musica/zz rotto.mp3"
key Escape; key m; key Return; sleep 0.7
n0=$(grep -c ' start ' $BRAIN_CTL_STATE/player.log)
tap $MROW4 1.0
shot $D-11-errore-$GEOM
check "il lettore ha rifiutato il file" grep -q 'errore .*zz rotto.mp3' $BRAIN_CTL_STATE/player.log
check "dopo un errore non parte nient'altro" test "$(grep -c ' start ' $BRAIN_CTL_STATE/player.log)" = "$n0"
check "nessun lettore attivo dopo l'errore" test "$(players)" = 0
sleep 2.6

echo "--- chiusura della shell: il lettore si ferma"
tap $MROW1 1.0
check "lettore attivo" test "$(players)" = 1
kill $SHELL_PID; wait $SHELL_PID 2> /dev/null; echo "uscita della shell: $?"; sleep 0.4
check "SIGTERM alla shell: nessun lettore rimasto" test "$(players)" = 0
start_shell --open music; sleep 0.5
tap $MROW1 1.0
kill -9 $SHELL_PID; wait $SHELL_PID 2> /dev/null; sleep 1.0
check "shell uccisa (SIGKILL): il lettore esce da solo" test "$(players)" = 0

echo "--- lettore assente"
BRAIN_PLAYER=/nonexistent/brainplay; export BRAIN_PLAYER
start_shell --open music; sleep 0.5
tap $MROW1 1.0; shot $D-12-lettore-assente-$GEOM
check "la shell resta viva senza lettore" shell_alive
sleep 2.6
echo "--- libreria vuota"
rm -rf $HOME/Musica
key Escape; key m; key Return; sleep 0.7; shot $D-13-vuota-$GEOM
echo "--- registro del finto lettore"; plog | cut -c1-110
