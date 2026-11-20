D=dev/p
shot $D-01-home
tap 86 180
shot $D-02-settings
drag 240 700 240 300 12 16 1.0
shot $D-03-settings-scrolled
tap 30 68
tap 240 180
shot $D-06-clock
tap 240 112
tap 100 340; sleep 1.2; tap 240 340; sleep 0.6; tap 240 340
shot $D-08-stopwatch
tap 400 112
shot $D-09-timer
tap 30 68
tap 394 180
xdotool key 1 2 asterisk 3 Return 4 5 6 minus 9 9 9 Return 3 comma 1 4; sleep 0.3
shot $D-10-calc
tap 30 68
tap 394 516
shot $D-13-stub
key Escape
tap 370 20
shot $D-21-quick
tap 100 270
shot $D-23-rotate-dialog
key Escape
tap 400 270
shot $D-26-power-dialog
key Escape; key Escape
tap 86 348 1.5
shot $D-30-terminal
tap 30 20 0.6
xfe > /dev/null 2>&1 & sleep 1.5
shot $D-33-two-windows
tap 30 20 0.6
shot $D-37-home
