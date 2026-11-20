#!/bin/bash
# test/qemu.sh - esegue il binario ARM (build/armel/brainshell) sotto
# qemu-user con CPU arm926 e confronta le schermate, pixel per pixel, con
# quelle del binario nativo. Dalla cartella shell/, dopo "make armel":
#   test/qemu.sh
cd "$(dirname "$0")/.."
docker run --rm -v "$PWD":/src brain-shelltest sh -c '
make -s native 2>&1 | grep -v "Clock skew\|modification time"
mkdir -p shots/dev
TAG=native BRAINSHELL_TIMING=1 test/session.sh 854x480 test/scenes/qemu.sh 2>&1 | tr -d "\000" | tail -3
pkill Xvfb; sleep 0.5; rm -f /tmp/.X99-lock
TAG=armel BRAINSHELL_TIMING=1 BIN=/src/build/armel/brainshell-x11 SHELL_WRAP="qemu-arm -cpu arm926" SHELL_WAIT=3 \
	test/session.sh 854x480 test/scenes/qemu.sh 2>&1 | tr -d "\000" | tail -3
cd shots/dev
bad=0
for s in home calc settings stub quick dialog; do
	# la barra (orologio) resta fuori dal confronto; nella home anche la
	# striscia con ora e data
	c=854x440+0+40; [ $s = home ] && c=854x360+0+120
	n=$(compare -metric AE \( q-native-$s.png -crop $c +repage \) \
		\( q-armel-$s.png -crop $c +repage \) null: 2>&1)
	echo "$s: $n pixel diversi fra nativo e ARM"
	[ "$n" = 0 ] || bad=1
done
rm -f q-native-*.png q-armel-*.png
exit $bad'
