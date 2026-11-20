# test/scenes/media.sh - schermate definitive di Note, Musica e Video
# (orizzontale 854x480 oppure verticale 480x854) in shots/.
[ $W -gt $H ] && P=landscape || P=portrait
media_files
music_geom
NEW="$((W - 98)) 68"; DEL="$((W - 32)) 68"
typ() { xdotool type --delay 15 "$1"; sleep 0.15; }

key n; key Return; sleep 0.6
shot $P-34-note
tap $NEW
typ 'Cose da fare sabato'; key Return; key Return
typ '1) comprare il caff'; key egrave; typ ' e il pane'; key Return
typ '2) telefonare a Niccol'; key ograve; typ ' per la cena (costa 25 '; key EuroSign; typ ' a testa)'; key Return
typ '3) la bici '; key egrave; typ ' dal meccanico: ritirarla prima delle 18, perch'; key eacute; typ ' poi chiude e riapre soltanto luned'; key igrave; typ ' mattina'
sleep 3.4
shot $P-35-nota-editor
key Up; key Up; key Home; key Right; key Right
shot $P-36-nota-cursore
tap $DEL 0.5
shot $P-37-nota-elimina
key Escape; key Escape; sleep 0.5
shot $P-38-note-aggiornate
i=1; : > /tmp/lunga
while [ $i -le 30 ]; do
	echo "$i. Nel mezzo del cammin di nostra vita mi ritrovai per una selva oscura, ch\303\251 la diritta via era smarrita." >> /tmp/lunga
	i=$((i + 1))
done
cp /tmp/lunga "$HOME/Note/divina.txt"
key Escape; key n; key Return; sleep 0.6
tap 200 132 0.5; key Prior
shot $P-39-nota-lunga
key Escape; key Escape

key m; key Return; sleep 0.7
shot $P-40-musica
tap $MROW3 1.8
shot $P-41-musica-in-riproduzione
press $((MCX + MCW * 2 / 3)) $MSEEK_Y 0.25
shot $P-42-musica-cursore
release 0.3
tap $MPLAY 0.4
key Down; key Down; key Down; key Down
shot $P-43-musica-tastiera
key Escape

key v; key Return; sleep 0.7
shot $P-44-video
tap 200 204 0.6
shot $P-45-video-avvio
sleep 2
