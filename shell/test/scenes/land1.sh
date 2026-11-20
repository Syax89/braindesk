# scena di sviluppo, 854x480: viste integrate
D=dev/l
press 99 180; shot $D-01-tile-pressed; release
shot $D-02-settings
drag 600 400 600 150 12 16 0.8
shot $D-03-settings-scrolled
press 40 68; shot $D-04-back-pressed; release
shot $D-05-home-again
tap 263 180
shot $D-06-clock
tap 427 112
shot $D-07-stopwatch
tap 150 420; sleep 1.3; tap 290 420; sleep 0.7; tap 290 420; sleep 0.5
shot $D-08-stopwatch-running
tap 590 112
shot $D-09-timer
tap 60 68
tap 427 180
shot $D-10-calc
xdotool key 1 2 asterisk 3 Return; sleep 0.2
xdotool key 7 comma 5 slash 0 Return; sleep 0.3
shot $D-11-calc-error
xdotool key 9 9 minus 1 0 0 Return 5 0 percent; sleep 0.3
shot $D-12-calc-hist
tap 60 68
tap 427 348
shot $D-13-stub
key Escape
shot $D-14-home
