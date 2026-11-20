# navigazione da tastiera (tasti fisici del Brain)
D=dev/k
key Right; shot $D-01-grid-focus
key Right; key Down; shot $D-02-grid-focus-moved
key c; shot $D-03-typeahead
key Return; shot $D-04-calc-open
xdotool key 4 2 plus 8 Return; sleep 0.2; shot $D-05-calc-typed
key Escape
key i; key Return; sleep 0.3
key Down; shot $D-06-settings-slider-focus
key Left; key Left; key Left; shot $D-07-settings-slider-changed
key Down; key Down; shot $D-08-settings-toggle-focus
key Return; shot $D-09-settings-toggled
key Up; key Return; shot $D-10-rotate-dialog
key Right; key Right; shot $D-11-dialog-focus
key Escape; shot $D-12-dialog-cancelled
key BackSpace; shot $D-13-home
# Tab (Brain English) fuori dai testi e' un tasto della shell: le schede con le cifre
key o; key Return; key 2; shot $D-14-clock-tab-by-keys
key space; sleep 1.2; key space; shot $D-15-stopwatch-space
cat $BRAIN_CTL_STATE/log
