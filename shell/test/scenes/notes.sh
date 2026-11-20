# Note: elenco, editor (scrittura con xdotool, cursore, tocco), salvataggio
# automatico e atomico, nota lunga, eliminazione, errori di scrittura.
D=dev/n
media_files
NEW="$((W - 98)) 68"; DEL="$((W - 32)) 68"; BACK="30 68"; ROW1="200 132"
note() { ls $HOME/Note/nota-*.txt 2> /dev/null | head -1; }
is() { printf "$2" > /tmp/expect; cmp -s "$1" /tmp/expect; }
typ() { xdotool type --delay 20 "$1"; sleep 0.15; }

key n; key Return; sleep 0.5
shot $D-01-elenco-$GEOM

echo "--- nuova nota: scrittura e salvataggio automatico"
tap $NEW; shot $D-02-vuota-$GEOM
typ 'Citt'; key agrave; typ ' da vedere'; key Return
typ 'Perch'; key eacute; typ ' no? Costa 12 '; key EuroSign
check "il file non esiste prima dei 3 s di pausa" test -z "$(note)"
shot $D-03-modificata-$GEOM
sleep 3.4
f=$(note)
check "salvataggio automatico dopo 3 s senza tasti" test -n "$f"
check "contenuto UTF-8 esatto (accenti ed euro)" is "$f" 'Citt\303\240 da vedere\nPerch\303\251 no? Costa 12 \342\202\254'
check "nessun file temporaneo rimasto" test -z "$(ls -A $HOME/Note | grep -v '\.txt$')"
shot $D-04-salvata-$GEOM

echo "--- cursore: Home/End, su/giu', Ctrl+Home/End, Canc, Backspace"
key Home; typ 'X'
key Up; key End; typ '!'
key ctrl+Home; key Delete
key ctrl+End; key BackSpace
key Left; key Left; key Left; key BackSpace
key Down; key ugrave
shot $D-05-cursore-$GEOM
key Escape; sleep 0.4
check "modifiche salvate tornando all'elenco" is "$f" 'itt\303\240 da vedere!\nXPerch\303\251 no? Costa12 \303\271'
shot $D-06-elenco-aggiornato-$GEOM

echo "--- tocco per posizionare il cursore"
tap $ROW1 0.5
tap 17 117; typ 'Z'
tap $((W - 40)) 143; typ '.'
tap $BACK 0.5
check "tocco: inizio della prima riga e fine della seconda" is "$f" 'Zitt\303\240 da vedere!\nXPerch\303\251 no? Costa12 \303\271.'

echo "--- uscita dalla vista mentre si scrive (tasto Home della barra)"
tap $ROW1 0.5; typ 'Q'; tap 30 20 0.5
check "salvata subito uscendo dalla vista" grep -q '\.Q$' "$f"

echo "--- nota lunga: a capo automatico, scorrimento, PagSu/PagGiu'"
i=1; : > /tmp/lunga
while [ $i -le 40 ]; do
	echo "Capoverso $i: nel mezzo del cammin di nostra vita mi ritrovai per una selva oscura, ch\303\251 la diritta via era smarrita. Ahi quanto a dir qual era \303\250 cosa dura." >> /tmp/lunga
	i=$((i + 1))
done
printf "$(cat /tmp/lunga)\n\tcon tab\r\nfine" > $HOME/Note/lunga.txt
key n; key Return; sleep 0.6
shot $D-07-elenco-con-lunga-$GEOM
tap $ROW1 0.6; shot $D-08-lunga-in-fondo-$GEOM
key Prior; key Prior; shot $D-09-lunga-pagsu-$GEOM
drag $((W / 2)) 200 $((W / 2)) 420 8 16 1.0; shot $D-10-lunga-trascinata-$GEOM
key ctrl+Home; typ 'INIZIO '; shot $D-11-lunga-inizio-$GEOM
key Next; key Down; key Down; typ '#'; shot $D-12-lunga-mezzo-$GEOM
key Escape; sleep 0.4
check "nota lunga: testo inserito in cima" grep -q '^INIZIO Capoverso 1:' $HOME/Note/lunga.txt
check "nota lunga: 42 righe come prima" test "$(wc -l < $HOME/Note/lunga.txt)" = 41
check "nota lunga: tab diventato spazi, CR tolto" grep -q '^    con tab$' $HOME/Note/lunga.txt
check "nota lunga: un solo # inserito" test "$(grep -o '#' $HOME/Note/lunga.txt | wc -l)" = 1

echo "--- nota nuova lasciata vuota: nessun file"
n0=$(ls $HOME/Note | wc -l)
tap $NEW; key Escape; sleep 0.3
tap $NEW; typ 'a'; key BackSpace; key Escape; sleep 0.3
check "nessun file per una nota vuota" test "$(ls $HOME/Note | wc -l)" = "$n0"

echo "--- eliminazione con conferma"
tap $ROW1 0.5
tap $DEL 0.5; shot $D-13-conferma-$GEOM
key Escape; sleep 0.3
check "Annulla: la nota resta" test -f $HOME/Note/lunga.txt
tap $DEL 0.5; key Right; key Right; key Return; sleep 0.6
check "Elimina: il file non c'e' piu'" test ! -e $HOME/Note/lunga.txt
shot $D-14-eliminata-$GEOM
sleep 2

echo "--- tastiera nell'elenco: frecce e Invio aprono, N crea"
key Down; key Down; shot $D-15-fuoco-$GEOM
key Return; sleep 0.4; typ '+'; key Escape; sleep 0.4
check "Invio apre la nota scelta con le frecce" test "$(grep -l '+$' $HOME/Note/*.txt | wc -l)" = 1
key n; typ 'Da tastiera'; key Escape; sleep 0.4
check "N crea una nota" test "$(grep -l '^Da tastiera$' $HOME/Note/*.txt | wc -l)" = 1

echo "--- cartella non scrivibile: avviso, niente perdite silenziose"
key Escape
rm -rf $HOME/Note; echo "non e' una cartella" > $HOME/Note
key n; key Return; sleep 0.5; shot $D-16-senza-cartella-$GEOM
tap $NEW; typ 'persa?'; key Escape; sleep 0.5
shot $D-17-non-salvata-$GEOM
key Escape; sleep 0.3; typ '!'
key Escape; sleep 0.4; key Right; key Right; key Return; sleep 0.4
shot $D-18-uscita-comunque-$GEOM
rm -f $HOME/Note
echo "figli rimasti:"; ps -o pid,stat,comm --ppid $SHELL_PID || true
