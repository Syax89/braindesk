#!/bin/bash
# test/mem-run.sh - dentro brain-shelltest: esegue le scene del backend mem
# (vedi test/mem.sh). Variabili: B, TARGET, WRAP, GEOM, SCENES.
#
# Una scena e' test/mem/NOME.sh e definisce:
#   scene()   stampa i comandi dello script (vedi bui_mem.c); puo' usare
#             $W, $H, $PORTRAIT per le coordinate dei due orientamenti
#   ENV       (facoltativa) variabili in piu' per brainshell
#   after()   (facoltativa) controlli a scena finita: stampa righe CHECK
. /src/test/lib.sh
[ "$TARGET" = armel ] || make -s B=$B $TARGET 2>&1 | grep -v 'Clock skew\|modification time'
BIN=$SRC/$B/$TARGET/brainshell
W=${GEOM%x*}; H=${GEOM#*x}
if [ $W -lt $H ]; then PORTRAIT=1; else PORTRAIT=0; fi
# coordinate dei riquadri e dei comandi nei due orientamenti (come test/scenes/final.sh)
if [ $PORTRAIT = 0 ]; then
	SET="99 180"; CLK="263 180"; CAL="427 180"; TRM="591 180"; FIL="755 180"
	GAL="99 348"; MUS="263 348"; VID="427 348"; NOT="591 348"
	TAB3="590 112"; TM_GO="323 436"
	QS_ROT="520 270"; QS_SHOT="640 270"; QS_PWR="765 270"; QS_SLIDER_Y=128; QS_X0=560; QS_X1=700
	SCROLL="600 420 600 140"; GRID_SCROLL="427 400 427 120"; GRID_BACK="427 130 427 470"
else
	SET="86 180"; CLK="240 180"; CAL="394 180"; TRM="86 348"; FIL="240 348"
	GAL="394 348"; MUS="86 516"; VID="240 516"; NOT="394 516"
	TAB3="386 112"; TM_GO="136 568"
	QS_ROT="145 270"; QS_SHOT="266 270"; QS_PWR="390 270"; QS_SLIDER_Y=128; QS_X0=200; QS_X1=330
	SCROLL="240 760 240 300"; GRID_SCROLL="240 760 240 200"; GRID_BACK="240 200 240 840"
fi
BACK="30 68"; STATUS="$((W - 110)) 20"; CLOCK="$((W - 40)) 20"; HOMEBTN="30 20"
ctl_log() { cat $BRAIN_CTL_STATE/log 2> /dev/null; }
fail=0
for name in $SCENES; do
	sysroot
	rm -rf $HOME; mkdir -p $HOME $BRAIN_CTL_STATE; rm -f $BRAIN_CTL_STATE/*
	media_files
	export BUI_MEM_STATE=/tmp/mem-state XAPP_LOG=/tmp/xapp.log BRAINSHELL_XORG="$SRC/test/fake-xorg"
	rm -f $XAPP_LOG /tmp/fake-xorg.*
	unset -f scene after; ENV=
	. $SRC/test/mem/$name.sh
	scene > /tmp/scene.txt
	t0=$(date +%s.%N)
	out=$(env $ENV BRAINSHELL_BACKEND=mem BRAINSHELL_MEM_SIZE=$GEOM BRAINSHELL_SCRIPT=/tmp/scene.txt \
		BRAINSHELL_SHOTS=$SHOTS/mem/$GEOM ASAN_OPTIONS=detect_leaks=1 \
		$WRAP $BIN 2> /tmp/shell.log)
	code=$?
	if type after > /dev/null 2>&1; then out="$out
$(after)"; fi
	if [ $code = 0 ] && ! echo "$out" | grep -q "CHECK FALLITO" && [ ! -s /tmp/shell.log ]; then
		echo "ok    $GEOM $name ($(echo "$out" | grep -c 'CHECK ok') controlli)"
	else
		echo "FALLITA $GEOM $name (uscita $code)"; echo "$out" | grep -v "CHECK ok"
		echo "--- stderr ---"; head -40 /tmp/shell.log; fail=1
	fi
	[ -n "$VERBOSE" ] && echo "$out"
	echo "$out" | grep -E "^(RSS|STATS|PIXEL|INFO)" | sed "s/^/    /"
done
exit $fail
