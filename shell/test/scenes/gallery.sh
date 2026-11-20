# test/scenes/gallery.sh - Galleria: griglia, miniature, visualizzatore,
# tastiera, eliminazione, immagini rovinate. Salva in shots/dev/g-*.
sh $SRC/test/mkassets.sh $HOME > /dev/null 2>&1
if [ $W -gt $H ]; then GAL="99 348"; else GAL="394 348"; fi
D=dev/g-$GEOM
BACK="30 68"
t0=$(date +%s%N)
tap $GAL 0.25; shot $D-01-apertura
sleep ${THUMB_WAIT:-6}; shot $D-02-griglia
echo "miniature in cache: $(ls $HOME/.cache/brainshell/thumbs | wc -l)"
drag $((W/2)) $((H-60)) $((W/2)) 150 10 16 1.5; shot $D-03-scorsa
sleep 3; shot $D-04-scorsa-caricata
key Home; key Right; key Right; key Down; shot $D-05-fuoco
key Return; sleep 0.15; shot $D-06-visualizzatore-attesa
sleep 2.5; shot $D-07-visualizzatore
tap $((W/2)) $((H/2)) 0.4; shot $D-08-senza-barra
key Right; sleep 2; shot $D-09-successiva
drag $((W-150)) $((H/2)) 150 $((H/2)) 8 12 2; shot $D-10-dopo-strisciata
tap $((W/2)) $((H/2)) 0.4
tap $((W-40)) $((H/2+14)) 2; shot $D-11-freccia-avanti
tap $((W-32)) 68 0.5; shot $D-12-conferma-elimina
key Escape; sleep 0.3
# Canc (Rireki) fuori dai testi e' la schermata: si elimina dal cestino
tap $((W-32)) 68 0.5; key Right; key Right; key Return; sleep 1.5; shot $D-13-eliminata
key Escape; sleep 0.5; shot $D-14-ritorno-griglia
key End; sleep 2; shot $D-15-fine
key BackSpace; sleep 0.3; shot $D-16-home
# seconda apertura: tutto dalla cache
tap $GAL 0.6; shot $D-17-riapertura-da-cache
key Escape
# schermate definitive
[ $W -gt $H ] && P=landscape || P=portrait
pub() { cp $SHOTS/$D-$1.png $SHOTS/$P-$2.png; }
pub 02-griglia 29-galleria
pub 05-fuoco 30-galleria-fuoco
pub 07-visualizzatore 31-galleria-immagine
pub 08-senza-barra 32-galleria-senza-barra
pub 12-conferma-elimina 33-galleria-elimina
