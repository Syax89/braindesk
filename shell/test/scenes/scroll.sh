# scorrimento cinetico: griglia con riquadri in piu', liste lunghe
D=dev/s
shot $D-01-grid-top
drag 400 380 400 120 8 12 0.15
shot $D-02-grid-flinging
sleep 1.5
shot $D-03-grid-rest
drag 400 150 400 300 10 16 1.2
shot $D-04-grid-back
# tocco che ferma l'inerzia: non deve aprire nulla
drag 400 380 400 100 6 10 0.1
tap 400 250 0.5
shot $D-05-grid-caught
key Home 2>/dev/null
# giri del cronometro
drag 400 150 400 420 10 16 1.0
tap 263 180; tap 427 112; tap 100 436
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do xdotool mousemove 255 436 click 1; sleep 0.12; done
sleep 0.3
shot $D-06-laps
drag 680 400 680 220 8 12 0.2
shot $D-07-laps-flinging
sleep 1.2
shot $D-08-laps-rest
