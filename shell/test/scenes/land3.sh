D=dev/l
# impostazioni rapide
tap 780 20
shot $D-21-quick
tap 520 270
shot $D-23-rotate-dialog
press 350 310; shot $D-24-dialog-pressed; release
shot $D-25-after-cancel
tap 765 270
shot $D-26-power-dialog
key Escape
tap 640 270; sleep 0.9
shot $D-27-screenshot-toast
sleep 3
# finestre esterne
xprop -root _NET_ACTIVE_WINDOW
tap 591 180 1.5
shot $D-30-terminal
xprop -root _NET_ACTIVE_WINDOW
tap 780 20 0.5
xprop -root _NET_ACTIVE_WINDOW
shot $D-31-quick-over-terminal
key Escape
xprop -root _NET_ACTIVE_WINDOW
tap 30 20 0.6
shot $D-32-home-with-chip
xfe > /dev/null 2>&1 & sleep 1.5
shot $D-33-two-windows
tap 120 20 0.6
shot $D-34-chip-activated
xprop -root _NET_ACTIVE_WINDOW _NET_CLIENT_LIST _NET_SHOWING_DESKTOP
press 222 20; shot $D-35-close-pressed; release 0.6
shot $D-36-after-close
tap 30 20 0.6
shot $D-37-home
xprop -root _NET_ACTIVE_WINDOW _NET_CLIENT_LIST _NET_SHOWING_DESKTOP
xdotool key Right Right Return; sleep 0.4
shot $D-38-keyboard-open
cat $BRAIN_CTL_STATE/log
