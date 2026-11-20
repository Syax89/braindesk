# cambio di dimensione dello schermo a caldo (RandR): le finestre devono seguire
D=dev/r
xrandr --fb 480x480 > /dev/null 2>&1; sleep 1
xwininfo -root -tree | grep brainshell
shot $D-02-after-shrink
tap 240 180; shot $D-03-view-after-shrink
tap 370 20; shot $D-04-quick-after-shrink
key Escape
xrandr --fb 854x480 > /dev/null 2>&1; sleep 1
xwininfo -root -tree | grep brainshell
shot $D-05-back
