#!/bin/sh
# install-dicts.sh [HOST]: scarica i dizionari FreeDict e il font IPAex Gothic
# dai pacchetti Debian, li converte (mkdict.py) e li copia sul Brain:
#   /usr/local/share/brain/dict/   ita-eng, eng-ita, eng-jpn, jpn-eng
#   /usr/local/share/brain/fonts/fallback.ttf   glifi che i font della shell non hanno
# Serve Docker (per apt-get download) e python3. HOST di default: brain.
# Licenze: FreeDict ita/eng GPL-2.0+, eng-jpn e jpn-eng CC BY-SA 3.0 (WikDict,
# JMdict), IPAex Gothic IPA Font License.
set -e
HOST=${1:-brain}
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "$here/.work.XXXXXX")
trap 'rm -rf "$work"' EXIT
docker run --rm -v "$work":/out debian:trixie sh -c 'apt-get update -qq >/dev/null &&
	cd /out && apt-get download -qq dict-freedict-ita-eng dict-freedict-eng-ita \
	dict-freedict-eng-jpn dict-freedict-jpn-eng fonts-ipaexfont-gothic 2>/dev/null'
mkdir -p "$work/x" "$work/out"
for d in "$work"/*.deb; do (cd "$work/x" && ar x "$d" && tar xf data.tar.* && rm -f data.tar.* control.tar.* debian-binary); done
for n in ita-eng eng-ita eng-jpn jpn-eng; do
	python3 "$here/mkdict.py" "$work/x/usr/share/dictd/freedict-$n.index" \
		"$work/x/usr/share/dictd/freedict-$n.dict.dz" "$work/out/$n"
done
cp "$work/x/usr/share/fonts/opentype/ipaexfont-gothic/ipaexg.ttf" "$work/out/fallback.ttf"
# /tmp sul Brain e' piccolo (tmpfs): si passa dalla home, sulla microSD
ssh "$HOST" 'rm -rf ~/.dict-inst && mkdir -p ~/.dict-inst'
scp -q "$work"/out/* "$HOST":.dict-inst/
ssh "$HOST" 'cd ~/.dict-inst && sudo mkdir -p /usr/local/share/brain/dict /usr/local/share/brain/fonts &&
	sudo mv fallback.ttf /usr/local/share/brain/fonts/ && sudo mv * /usr/local/share/brain/dict/ &&
	sudo chown -R root:root /usr/local/share/brain && sudo chmod -R a+rX /usr/local/share/brain &&
	cd && rmdir ~/.dict-inst && du -sh /usr/local/share/brain/dict /usr/local/share/brain/fonts'
