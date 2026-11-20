# apre e chiude ogni vista integrata (tocco sul riquadro, ritorno con Esc o freccia)
scene() {
	echo "wait 300"
	for v in "settings $SET" "clock $CLK" "calc $CAL" "files $FIL" "gallery $GAL" \
		 "music $MUS" "video $VID" "notes $NOT"; do
		set -- $v
		echo "tap $2 $3 900"
		echo "expect view=$1"
		echo "shot view-$1"
		echo "tap $BACK 800"
		echo "expect view=grid"
	done
	# ancora, chiudendo con la tastiera (Esc dalla vista)
	echo "tap $CLK 800"
	echo "key Escape 700"
	echo "expect view=grid"
	echo "tap $SET 800"
	echo "key BackSpace 700"
	echo "expect view=grid"
	echo "shot views-end"
}
