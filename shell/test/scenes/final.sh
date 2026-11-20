# test/scenes/final.sh - percorre tutta l'interfaccia e salva le schermate
# definitive in shots/ (orizzontale 854x480 oppure verticale 480x854).
if [ $W -gt $H ]; then
	P=landscape
	SET="99 180"; CLK="263 180"; CAL="427 180"; TRM="591 180"; VID="427 348"
	TAB2="427 112"; TAB3="590 112"
	SW_GO="100 436"; SW_LAP="255 436"; TM_GO="323 436"; TM_RST="531 436"
	QS_ROT="520 270"; QS_SHOT="640 270"; QS_PWR="765 270"; QS_SLIDER_Y=128; QS_X0=560; QS_X1=700
	SCROLL="600 420 600 140"
else
	P=portrait
	SET="86 180"; CLK="240 180"; CAL="394 180"; TRM="86 348"; VID="240 516"
	TAB2="240 112"; TAB3="386 112"
	SW_GO="88 412"; SW_LAP="240 412"; TM_GO="136 568"; TM_RST="344 568"
	QS_ROT="145 270"; QS_SHOT="266 270"; QS_PWR="390 270"; QS_SLIDER_Y=128; QS_X0=200; QS_X1=330
	SCROLL="240 760 240 300"
fi
BACK="30 68"; STATUS="$((W - 110)) 20"; HOMEBTN="30 20"

shot $P-01-home
press $SET; shot $P-02-riquadro-premuto; release
shot $P-03-impostazioni
drag $SCROLL 12 16 1.2
shot $P-04-impostazioni-scorse
tap $BACK
tap $CLK; shot $P-05-orologio
tap $TAB2; tap $SW_GO 1.3; tap $SW_LAP 0.9; tap $SW_LAP 0.7; tap $SW_LAP 0.4
shot $P-06-cronometro
tap $SW_GO
tap $TAB3; tap $TM_GO 2.2
shot $P-07-timer
tap $TM_RST
tap $BACK
tap $CAL
xdotool key 1 2 asterisk 3 Return 4 5 6 minus 9 9 9 Return 2 2 slash 7 Return 3 comma 1 4; sleep 0.4
shot $P-08-calcolatrice
tap $BACK
tap $VID; shot $P-09-video-vuoto
key Escape
tap $STATUS; shot $P-10-impostazioni-rapide
press $QS_X1 $QS_SLIDER_Y; xdotool mousemove $QS_X0 $QS_SLIDER_Y; sleep 0.2; shot $P-11-cursore-trascinato; release
tap $QS_ROT; shot $P-12-conferma-rotazione
key Escape
tap $QS_PWR; shot $P-13-conferma-spegnimento
key Escape
tap $QS_SHOT 1.1; shot $P-14-schermata-salvata
sleep 3.2
tap $TRM 1.6; shot $P-15-terminale
tap $HOMEBTN 0.7
xfe > /dev/null 2>&1 & sleep 1.6; shot $P-16-due-finestre
tap $HOMEBTN 0.7; shot $P-17-home-con-linguette
key Right; key Right; key Down; shot $P-18-fuoco-da-tastiera
kill $SHELL_PID; wait $SHELL_PID 2> /dev/null
sysroot 34 Charging 3890000 1
start_shell --open demo
shot $P-19-campionario-in-carica
