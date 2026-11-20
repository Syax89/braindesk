# test/scenes/rice2.sh - "in riproduzione" nella home, sfondo dell'utente,
# stili dei riquadri, livelli di animazione, avviso di batteria scarica,
# elastico della griglia. Salva in shots/dev/y-*.
D=dev/y-$GEOM
[ $W -gt $H ] && P=landscape || P=portrait
restart() { kill $SHELL_PID; wait $SHELL_PID 2> /dev/null; start_shell "$@"; }
conf() { mkdir -p $HOME/.config/brainshell; printf '%s\n' "$@" > $HOME/.config/brainshell/config; }
media_files; music_geom
# Musica suona -> la home lo mostra; un tocco sul riquadro riapre Musica
key m; key Return; sleep 1.2
tap $MROW2 0.8
tap 30 20 0.8; shot $D-01-in-riproduzione; cp $SHOTS/$D-01-in-riproduzione.png $SHOTS/$P-64-in-riproduzione.png
if [ $W -gt $H ]; then NP="700 84"; else NP="240 156"; fi
press $NP 0.2; shot $D-02-in-riproduzione-premuto; release 0.8
shot $D-03-musica-riaperta
tap $MPLAY 0.6; tap 30 20 0.8; shot $D-04-in-pausa
key m; key Return; sleep 0.6; key Escape; sleep 0.4
# avviso di batteria: scende sotto il 15% mentre gira (lettura ogni 30 s: qui si riavvia con 16 e poi 14)
# stili dei riquadri su ogni tema
for t in teal chiaro aurora; do for s in filled glass outline; do
	conf "theme=$t" "tiles=$s"; restart; shot $D-10-$t-$s
done; done
# sfondo dell'utente (grezzo RGB565, grande quanto lo schermo), tema vetro
convert -size ${W}x${H} plasma:tomato-steelblue -flip -define bmp:subtype=RGB565 bmp3:/tmp/w.bmp
tail -c $((W * H * 2)) /tmp/w.bmp > $HOME/.config/brainshell/wallpaper.rgb565
conf "theme=aurora"; restart; shot $D-20-sfondo-utente-aurora; cp $SHOTS/$D-20-sfondo-utente-aurora.png $SHOTS/$P-65-sfondo-utente.png
conf "theme=chiaro"; restart; shot $D-21-sfondo-utente-chiaro
rm $HOME/.config/brainshell/wallpaper.rgb565
# elastico: trascina oltre l'inizio e cattura mentre e' tirato, poi dopo il rilascio
conf "theme=teal"; restart
xdotool mousemove $((W / 2)) 200 mousedown 1 sleep 0.05 mousemove $((W / 2)) 260 sleep 0.05 mousemove $((W / 2)) 380 sleep 0.2
shot $D-30-elastico-tirato
xdotool mouseup 1; sleep 0.08; shot $D-31-elastico-ritorno; sleep 0.5; shot $D-32-elastico-fermo
