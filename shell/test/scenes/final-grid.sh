# griglia con piu' riquadri di quanti ne entrano: scorrimento cinetico
[ $W -gt $H ] && P=landscape || P=portrait
drag $((W / 2)) $((H - 60)) $((W / 2)) $((H - 220)) 10 16 1.0
shot $P-20-griglia-scorsa
