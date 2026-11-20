# confronto fra binario nativo e binario ARM (qemu-user, CPU arm926): stessi pixel?
D=dev/q-$TAG
grep -a "primo fotogramma" /tmp/shell.log
shot $D-home
tap 427 180
xdotool key 1 2 asterisk 3 Return 7 slash 8 Return 1 comma 5; sleep 0.5
shot $D-calc
tap 30 68
tap 99 180 0.6
drag 600 400 600 150 12 16 2.5
shot $D-settings
tap 30 68
tap 427 348; shot $D-stub
key Escape
tap 780 20 0.8; shot $D-quick
tap 520 270 0.8; shot $D-dialog
key Escape; key Escape
grep -E "VmRSS|RssAnon" /proc/$SHELL_PID/status
