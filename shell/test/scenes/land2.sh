D=dev/l
tap 427 180
shot $D-10-calc
xdotool key 1 2 asterisk 3 Return; sleep 0.2
xdotool key 7 comma 5 slash 0 Return; sleep 0.3
shot $D-11-calc-error
xdotool key 9 9 minus 1 0 0 Return 5 0 percent; sleep 0.3
press 680 215; shot $D-12-calc-hist-keypressed; release
tap 30 68
tap 427 348
shot $D-13-stub
key Escape
tap 263 180; tap 590 112
shot $D-09-timer
tap 60 68
# impostazioni rapide
press 780 20; shot $D-20-status-pressed; release
shot $D-21-quick
drag 700 135 560 135 8 16
shot $D-22-quick-bright
tap 540 300
shot $D-23-rotate-dialog
press 560 305; shot $D-24-dialog-pressed; release
shot $D-25-after-cancel
tap 780 20; tap 780 300
shot $D-26-power-dialog
key Escape
tap 780 20; tap 660 300; sleep 0.9
shot $D-27-screenshot-toast
cat $BRAIN_CTL_STATE/log
