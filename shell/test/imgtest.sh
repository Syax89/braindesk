#!/bin/bash
# test/imgtest.sh - prove di bs_img.c (decodifica, riduzione, orientamento,
# file rovinati) fuori dalla shell. Dalla cartella shell/:
#   test/imgtest.sh            tutto
#   FUZZ=2000 test/imgtest.sh  piu' file rovinati
cd "$(dirname "$0")/.."
docker run --rm -e FUZZ=${FUZZ:-400} -v "$PWD":/src brain-shelltest bash -c '
set -e
W="-std=gnu11 -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes"
mkdir -p build/test
gcc $W -O2 -o build/test/imgtest test/imgtest.c bs_img.c
gcc $W -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o build/test/imgtest-asan test/imgtest.c bs_img.c
set +e
sh test/mkassets.sh /tmp/home > /dev/null 2>&1
I=/tmp/home/Immagini; T=build/test/imgtest; fail=0
echo "--- confronto con ImageMagick (PSNR in dB: sopra 30 e'"'"' la stessa immagine a meno di RGB565 + dithering)"
cmp_ref() {	# file larghezza altezza
	$T $1 $2 $3 fit /tmp/o.ppm > /tmp/o.txt || { echo "FALLITA $1"; fail=1; return; }
	sz=$(sed -n "s/.*rc=0 \([0-9]*x[0-9]*\) .*/\1/p" /tmp/o.txt)
	convert "$1" -auto-orient -background "#171c24" -flatten -filter box -resize "$sz!" /tmp/ref.ppm
	p=$(compare -metric PSNR /tmp/o.ppm /tmp/ref.ppm null: 2>&1 | sed "s/ .*//")
	printf "%-26s %-9s PSNR %s\n" "$(basename $1)" "$sz" "$p"
	awk "BEGIN{exit !($p < 30)}" && { echo "  TROPPO DIVERSA"; fail=1; }
}
cmp_ref $I/tramonto.jpg 854 440
cmp_ref $I/Vacanze/torre.jpg 480 798
cmp_ref $I/Vacanze/prato.jpg 854 440
cmp_ref $I/Vacanze/ruotata.jpg 854 440
cmp_ref $I/progressivo.jpg 854 440
cmp_ref $I/grigio.jpg 854 440
cmp_ref $I/logo.png 854 440
cmp_ref $I/disegno.bmp 480 798
cmp_ref $I/animazione.gif 854 440
cmp_ref $I/panorama.jpg 854 440
cmp_ref $I/piccola.png 854 440
cmp_ref $I/Schermate/schermo-20261111-103001.png 480 798
echo "--- miniature (cover 160x160) e tempi sul PC"
for f in tramonto.jpg Vacanze/torre.jpg Vacanze/prato.jpg progressivo.jpg logo.png panorama.jpg piccola.png; do
	$T $I/$f 160 160 cover /tmp/t.ppm || fail=1
done
$T $I/tramonto.jpg 854 440 fit; $T $I/Vacanze/prato.jpg 854 440 fit
echo "--- rifiuti attesi (rc: 2 formato, 3 troppo grande; 0 per il file troncato)"
for f in enorme.jpg finta.jpg vuota.png troncata.jpg; do $T $I/$f 854 440 fit; done
convert -size 2500x2000 plasma: -interlace Plane /tmp/prog5mp.jpg; $T /tmp/prog5mp.jpg 854 440 fit
echo "--- file rovinati con ASan + UBSan ($FUZZ varianti)"
A=build/test/imgtest-asan; bad=0; n=0
convert $I/Vacanze/dipinto.jpg -resize 320x240 /tmp/s.jpg
convert $I/Vacanze/dipinto.jpg -resize 320x240 -interlace Plane /tmp/sp.jpg
convert $I/logo.png -resize 128x128 /tmp/s.png
convert $I/animazione.gif -resize 160x120 /tmp/s.gif
convert $I/disegno.bmp -resize 160x120 /tmp/s.bmp
for src in /tmp/s.jpg /tmp/sp.jpg /tmp/s.png /tmp/s.gif /tmp/s.bmp; do
	size=$(stat -c %s $src); i=0
	while [ $i -lt $((FUZZ / 5)) ]; do
		cp $src /tmp/f.bin
		if [ $((i % 4)) = 0 ]; then
			head -c $((RANDOM * RANDOM % size + 1)) $src > /tmp/f.bin
		else
			k=0; while [ $k -lt $((i % 7 + 1)) ]; do
				off=$((RANDOM * RANDOM % size)); [ $((i % 3)) = 0 ] && off=$((RANDOM % 700))
				printf "\\x$(printf %02x $((RANDOM % 256)))" | dd of=/tmp/f.bin bs=1 seek=$off conv=notrunc 2> /dev/null
				k=$((k + 1))
			done
		fi
		for mode in "854 440 fit" "160 160 cover"; do
			timeout 20 $A /tmp/f.bin $mode > /tmp/a.txt 2>&1; rc=$?
			if grep -q "ERROR\|runtime error" /tmp/a.txt || [ $rc -ge 124 ]; then
				bad=$((bad + 1)); cp /tmp/f.bin /src/build/test/bad-$bad-$(basename $src)
				grep -m3 "ERROR\|runtime error" /tmp/a.txt; [ $rc -ge 124 ] && echo "rc=$rc"
			fi
		done
		n=$((n + 1)); i=$((i + 1))
	done
done
echo "varianti provate: $n, con errori di memoria/UB o blocchi: $bad"
[ $bad = 0 ] || fail=1
[ $fail = 0 ] && echo "imgtest: ok" || echo "imgtest: FALLITO"
exit $fail'
