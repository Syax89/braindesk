#!/bin/sh
# test/session.sh GEOMETRIA SCRIPT [argomenti di brainshell]
# Avvia Xvfb + JWM + brainshell e poi esegue SCRIPT (comandi di lib.sh).
# Va lanciato dentro l'immagine brain-shelltest, es. dalla cartella shell/:
#   docker run --rm -v "$PWD":/src brain-shelltest test/session.sh 854x480 test/scenes/all.sh
. /src/test/lib.sh
GEOM=$1; SCRIPT=$2; shift 2
sysroot
start_x $GEOM
[ -n "$NO_WM" ] || start_wm
start_shell "$@"
. $SCRIPT
finish
