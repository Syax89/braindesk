#!/bin/bash
# test/fuzz.sh [passi [seme]] - prova a caso l'editor delle note con ASan +
# UBSan, senza server X (vedi test/edit_fuzz.c). Dalla cartella shell/.
cd "$(dirname "$0")/.."
exec docker run --rm -v "$PWD":/src brain-shelltest sh -c '
mkdir -p build/debug &&
cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -I. \
	-o build/debug/edit_fuzz test/edit_fuzz.c \
	bui.c bui_x.c bui_w.c gen/bui_fonts.c gen/bui_icons.c -lX11 -lXext &&
build/debug/edit_fuzz '"${1:-200000} ${2:-1}"
