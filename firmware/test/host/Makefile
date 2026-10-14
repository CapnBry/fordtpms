# Host-side tests for the portable parts of the firmware (decoder, UI renderer).
CFLAGS ?= -O1 -g -Wall -Wextra -std=gnu11 -fsanitize=address,undefined
SRC := ../../src

all: test_decoder
	./test_decoder

test_decoder: test_decoder.c $(SRC)/tpms_ford.c $(SRC)/tpms_ford.h
	$(CC) $(CFLAGS) -I$(SRC) -o $@ test_decoder.c $(SRC)/tpms_ford.c

clean:
	rm -rf test_decoder ui_preview *.pgm shots


ui_preview: ui_preview.c $(SRC)/gfx.c $(SRC)/ui.c $(SRC)/gfx.h $(SRC)/ui.h
	$(CC) $(CFLAGS) -I$(SRC) -o $@ ui_preview.c $(SRC)/gfx.c $(SRC)/ui.c

preview: ui_preview
	./ui_preview

# Regenerates the screenshots in docs/images (needs ImageMagick).
DOCS_IMG := ../../../docs/images
screenshots: ui_preview
	mkdir -p $(DOCS_IMG) shots
	./ui_preview shots
	for f in shots/*.pgm; do magick $$f $(DOCS_IMG)/$$(basename $$f .pgm).png; done
	rm -rf shots

.PHONY: all clean preview screenshots
