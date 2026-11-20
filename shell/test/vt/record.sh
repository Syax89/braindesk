#!/bin/bash
# test/vt/record.sh - registra i flussi di byte usati da test/vt_test.c.
#
# Ogni programma gira in un pannello di tmux 80x24 con TERM=xterm-256color;
# "pipe-pane" salva esattamente i byte che il programma scrive (NOME.raw) e
# "capture-pane" il testo che tmux mostra in quel momento (NOME.txt, la
# risposta attesa), con la posizione del cursore (NOME.cur). tmux fa quindi
# da emulatore di riferimento. Dalla cartella shell/ (serve la rete, solo
# per rigenerare i file; le prove usano quelli salvati):
#
#   test/vt/record.sh
#
# Il contenitore e' debian:trixie con tmux, vim-tiny, nano, less, htop.
cd "$(dirname "$0")"
exec docker run --rm -v "$PWD":/out debian:trixie bash -c '
set -e
apt-get update > /dev/null 2>&1
apt-get install -y --no-install-recommends tmux vim-tiny nano less htop procps locales > /dev/null 2>&1
export LANG=C.UTF-8 HOME=/root
for i in $(seq 1 100); do echo "riga $i: testo di prova con accentate àèìòù e simboli € °"; done > /tmp/f.txt; cp /tmp/f.txt /tmp/g.txt
mkdir -p /tmp/d/cartella /tmp/d/altra; : > /tmp/d/file.txt; : > /tmp/d/foto.jpg
printf "#!/bin/sh\n" > /tmp/d/script.sh; chmod +x /tmp/d/script.sh; ln -sf file.txt /tmp/d/link
PS1_DEB="\[\e]0;\u@\h: \w\a\]\[\e[01;32m\]\u@\h\[\e[00m\]:\[\e[01;34m\]\w\[\e[00m\]\$ "

# rec NOME COMANDO ATTESA [TASTI...]: i tasti sono argomenti di tmux send-keys
rec() {
	local name=$1 cmd=$2 wait=$3
	shift 3
	tmux kill-server 2> /dev/null || true
	tmux -u new-session -d -x 80 -y 24 -s r "sleep 0.5; exec env TERM=xterm-256color LANG=C.UTF-8 $cmd"
	tmux set -g status off
	tmux resize-window -t r -x 80 -y 24
	tmux pipe-pane -t r -O "cat > /out/$name.raw"
	sleep $wait
	for k in "$@"; do
		if [ "$k" = SLEEP ]; then sleep 0.6; else tmux send-keys -t r "$k"; fi
	done
	sleep 1.2
	tmux capture-pane -p -t r > /out/$name.txt
	tmux display -p -t r "#{cursor_x} #{cursor_y}" > /out/$name.cur
	tmux pipe-pane -t r
	sleep 0.3
	tmux kill-server || true
	echo "$name: $(wc -c < /out/$name.raw) byte"
}

rec bash "env -i HOME=/root TERM=xterm-256color LANG=C.UTF-8 PS1=\"$PS1_DEB\" bash --norc --noprofile -i" 1.5 \
	"cd /tmp" Enter SLEEP "echo città €" Enter SLEEP "ls --color=auto -F /tmp/d" Enter SLEEP \
	"printf \"\\e[1;31mrosso\\e[0m \\e[38;5;208mara\\e[0m \\e[38;2;10;200;30mvero\\e[0m \\e[7minv\\e[0m\\n\"" Enter SLEEP \
	"echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx fine" Enter SLEEP \
	"echo ciao" Left Left Left Left "X"
rec top "top -d 100" 2
rec htop "htop -d 1000" 2.5
rec vim "vi -u NONE -N" 1.5
rec vimfile "vi -u NONE -N /tmp/f.txt" 1.5 40G SLEEP C-e C-e C-e SLEEP O "nuova riga" Escape SLEEP C-y SLEEP
rec nano "nano /tmp/g.txt" 1.5 Down Down Down "ciao" SLEEP
rec less "less /tmp/f.txt" 1.5 Space SLEEP
# dpkg/apt: riga di avanzamento in fondo con una regione di scorrimento
cat > /tmp/apt.sh << "EOF"
printf "Lettura...\n"
printf "\n\0337\033[0;23r\0338\033[1A"
for i in $(seq 1 40); do
	printf "Configurazione del pacchetto %d...\n" $i
	printf "\0337\033[24;0f\033[42m\033[30mProgress: [%3d%%]\033[49m\033[39m [%-40s] \0338" $((i * 2)) "$(printf "%*s" $i | tr " " "#")"
done
sleep 100
EOF
rec apt "sh /tmp/apt.sh" 2.5
# frammenti alla vttest: inserimenti, cancellazioni, margini, schermo alternativo, DEC grafico
cat > /tmp/frag.sh << "EOF"
printf "\033[2J\033[H"
printf "\033#8"
printf "\033[3;20H\033[1;31mmargini\033[0m"
printf "\033[5;10r\033[5;1Hpriva\nseconda\nterza\nquarta\nquinta\nsesta\nsettima\033[r"
printf "\033[12;1HABCDEFGHIJ\033[12;3H\033[2@\033[12;8H\033[3P"
printf "\033[13;1HXXXXXXXXXX\033[13;4H\033[3X"
printf "\033[14;1H\033(0lqqqqk\033(B testo \033(0x\033(B"
printf "\033[15;1H\033[4h1234\033[15;2Hii\033[4l"
printf "\033[16;5H\033[1K\033[17;1Hcancella-fine\033[17;9H\033[K"
printf "\033[18;1H\033[2Ma\033[18;1H\033[1L"
printf "\033[20;70Hlarghezza: 日本語の文字\033[21;1Htab:\tA\tB\tC"
printf "\033[22;1H\033[?1049hALTERNATO\033[?1049l"
printf "\033[23;1H\033[38;5;46mverde256\033[48;2;0;0;200m blu\033[0m \033[9mbarrato\033[0m"
printf "\033[24;75Hfine!X"
sleep 100
EOF
rec frag "sh /tmp/frag.sh" 2
'
