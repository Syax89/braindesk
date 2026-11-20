# stati della batteria, file mancanti, brain-ctl assente, programma inesistente
D=dev/b
restart() { kill $SHELL_PID; wait $SHELL_PID 2>/dev/null; start_shell "$@"; }
bar() { xwd -root -silent | convert xwd:- -crop ${W}x40+0+0 +repage $SHOTS/$1.png; }
sysroot 45 Charging 4012000 1; restart; bar $D-1-charging
tap $((W - 110)) 20; shot $D-1-charging-quick; key Escape
sysroot 15 Discharging 3610000 0; restart; bar $D-2-low
sysroot 5 Discharging 3420000 0; restart; bar $D-3-critical
sysroot 100 Full 4180000 1; restart; bar $D-4-full
rm -rf $BRAIN_SYSROOT; BRAIN_CTL=/nonexistent/brain-ctl; export BRAIN_CTL
restart --open settings; shot $D-5-nothing-available
tap $((W - 110)) 20; shot $D-6-quick-nothing
tap $((W - 150)) 128; key Escape
key Escape
PATH=/usr/bin:/bin; export PATH
restart
tap 591 180 0.6; shot $D-7-launch-failed
sleep 3.2
echo "processi figli rimasti (zombie):"; ps -o pid,stat,comm --ppid $SHELL_PID || true
