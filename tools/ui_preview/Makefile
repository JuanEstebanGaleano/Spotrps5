# Renders the screens with made-up data: make -C tools/ui_preview; then ./preview /tmp/ui (and convert the .ppm files).
SRC = ../../loader/platform/spotify
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -I../../loader/include -I$(SRC)
preview: preview.c overlay_sw.c $(SRC)/ui.c ../../loader/src/overlay_assets.c
	cc $(CFLAGS) $^ -o $@ -lz -lm
