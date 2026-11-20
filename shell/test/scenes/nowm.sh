# senza gestore di finestre: la shell deve comunque funzionare
D=dev/n
shot $D-01-home
tap 263 180; shot $D-02-clock
xdotool key 2 space; sleep 0.8; shot $D-03-keys
key Escape
xterm -geometry 80x20+100+100 -T "Finestra libera" & sleep 1
shot $D-04-xterm-over
tap 30 20 0.5
shot $D-05-home-raised
tap 780 20; shot $D-06-quick
key Escape
