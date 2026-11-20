#!/bin/sh
# test/mkassets.sh - prepara la home finta per le scene di Esplora e Galleria
# (dentro l'immagine brain-shelltest: usa ImageMagick). Le immagini sono
# generate, non c'e' nulla da scaricare.
#   mkassets.sh [CARTELLA_HOME]
H=${1:-/tmp/home}
I=$H/Immagini
mkdir -p $H/Documenti/Lavoro $H/Documenti/Vuota $I/Vacanze $I/Schermate $H/Musica $H/Video $H/Note $H/.config
cd $I
# foto "vere": grandi, JPEG baseline, soggetti diversi
convert -size 4000x3000 plasma:tomato-steelblue -blur 0x2 -quality 88 tramonto.jpg
convert -size 3000x4000 gradient:'#0b3d91-#f6c177' -swirl 120 -quality 88 Vacanze/torre.jpg
convert -size 3264x2448 plasma:seagreen-khaki -blur 0x3 -quality 85 Vacanze/prato.jpg
convert -size 2400x1600 radial-gradient:'#ffd166-#073b4c' -quality 90 Vacanze/sole.jpg
convert -size 1600x1200 pattern:hexagons -scale 400% -fill '#5ccfbe' -opaque black -quality 90 Vacanze/esagoni.jpg
convert -size 1280x853 plasma:purple-gold -paint 4 -quality 85 Vacanze/dipinto.jpg
# con orientamento EXIF 6 (ruotata dalla fotocamera): la freccia deve puntare in alto
convert -size 1200x800 xc:'#1d3557' -fill '#f1faee' -draw 'polygon 1100,400 700,150 700,650' \
	-fill '#e63946' -draw 'rectangle 100,340 700,460' -quality 90 /tmp/freccia.jpg
convert /tmp/freccia.jpg -set exif:Orientation 6 -orient right-top Vacanze/ruotata.jpg
# progressivo (va a stb_image), scala di grigi, PNG con trasparenza, BMP, GIF
convert -size 1024x768 plasma:navy-orange -interlace Plane -quality 85 progressivo.jpg
convert -size 1500x1000 plasma:gray20-gray90 -colorspace Gray -quality 85 grigio.jpg
convert -size 640x640 xc:none -fill '#5ccfbe' -draw 'circle 320,320 320,40' -fill '#0e1116' \
	-draw 'circle 320,320 320,180' logo.png
convert -size 800x600 gradient:'#222-#eee' -type truecolor disegno.bmp
convert -size 320x240 plasma:red-blue animazione.gif
convert -size 2600x400 gradient:'#073b4c-#ffd166' -rotate 0 -quality 90 panorama.jpg
convert -size 96x96 plasma:green-yellow piccola.png
# schermate finte (854x480 PNG)
for n in 1 2 3; do
	convert -size 854x480 xc:'#0e1116' -fill '#171c24' -draw "roundrectangle 24,60 $((300 + n * 150)),$((200 + n * 60)) 16,16" \
		-fill '#5ccfbe' -draw "rectangle 24,24 $((120 * n)),40" Schermate/schermo-2026111$n-10300$n.png
done
# rovinati: troncato, finto, vuoto, enorme (solo intestazione)
head -c 200000 tramonto.jpg > troncata.jpg
echo "questo non e' un jpeg" > finta.jpg
: > vuota.png
convert -size 9000x6000 xc:gray -quality 20 enorme.jpg 2> /dev/null	# 54 MP: rifiutata
i=0; while [ $i -lt 24 ]; do
	convert -size 640x480 plasma:fractal -modulate 100,120,$((i * 15)) -quality 80 Vacanze/serie-$(printf %02d $i).jpg
	i=$((i + 1))
done
# date diverse, per l'ordinamento
touch -d '2026-11-18 09:15' tramonto.jpg; touch -d '2026-08-02 18:40' Vacanze/*.jpg
touch -d '2026-11-19 22:05' Schermate/*.png

cd $H
printf 'Lista della spesa\n\n- pane\n- latte\n- caff\303\250\n- mele (quelle rosse)\n' > Note/spesa.txt
{
	echo "# Appunti di viaggio"; echo
	i=1; while [ $i -le 60 ]; do
		echo "Giorno $i. Siamo partiti presto dalla citt\303\240 e abbiamo seguito la strada lungo il fiume fino al ponte vecchio, dove il mercato era gi\303\240 pieno di gente; pi\303\271 tardi \303\250 arrivata la pioggia."
		echo
		i=$((i + 1))
	done
} > Documenti/viaggio.md
printf '[schermo]\nluminosita = 70\n\trotazione = 0\n\n[audio]\nvolume = 45\n' > Documenti/brain.conf
printf '#!/bin/sh\n# salva una copia dei documenti\ntar czf /tmp/copia.tgz "$HOME/Documenti"\n' > Documenti/copia.sh
printf 'nome;quantita;prezzo\npane;2;1,80\nlatte;1;1,20\n' > Documenti/Lavoro/conti.csv
printf '{\n  "nome": "brain",\n  "versione": 3\n}\n' > Documenti/Lavoro/dati.json
printf 'Perch\351 no? Citt\340 in Latin-1.\n' > Documenti/latin1.txt
echo "file senza estensione ma di testo" > Documenti/LEGGIMI
head -c 3000 /bin/ls > Documenti/programma
head -c 50000 /dev/urandom > Documenti/archivio.zip
yes "riga di un file di registro molto lungo, ripetuta tante volte per superare il megabyte" | head -c 1500000 > Documenti/grande.log
: > Documenti/vuoto.txt
head -c 400000 /dev/zero > Musica/canzone.mp3
head -c 900000 /dev/zero > Musica/"Brano con un nome davvero molto lungo che non entra nella riga.flac"
head -c 5000000 /dev/zero > Video/film.mp4
echo "nascosto" > .profilo
i=0; while [ $i -lt 40 ]; do echo "x" > Documenti/Lavoro/relazione-$(printf %02d $i).txt; i=$((i + 1)); done
touch -d '2026-10-03 08:00' Documenti/Lavoro Documenti/viaggio.md
