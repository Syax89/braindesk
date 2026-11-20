D=dev/d
shot $D-01-demo-$GEOM
drag $((W/2)) $((H-80)) $((W/2)) 120 10 16 1.2
shot $D-02-demo-scrolled-$GEOM
drag $((W/2)) $((H-100)) $((W/2)) $((H-300)) 8 12 0.15
shot $D-03-demo-list-fling-$GEOM
sleep 1
tap $((W/2)) $((H-200)) 0.5
shot $D-04-demo-list-tap-$GEOM
