# Epic 05 / S4 — HELLODBG.TOS test program build.
#
# Driven by the local build.sh, which wraps this Makefile inside
# stcmd's docker so the m68k C toolchain (m68k-atari-mint-gcc +
# libcmini) comes from the same image as the main cartridge build.
#
# Modeled after the reference C_LIBCMINI demo:
#   https://github.com/sidecartridge/atarist-toolkit-docker/
#       blob/main/demo/C_LIBCMINI/Makefile

CC = m68k-atari-mint-gcc
LIBCMINI = /freemint/libcmini/lib
# crt0.o lives inside CFLAGS so it's part of the single-step
# compile-and-link invocation, exactly as the reference Makefile
# does it. The order matters: crt0.o must be first on the link
# line so the program's entry point is at offset 0 of the text
# segment, where the GEMDOS loader expects it.
CFLAGS = -std=gnu99 -I/freemint/libcmini/include -nostdlib \
         $(LIBCMINI)/crt0.o
LINKFLAGS = -s -L$(LIBCMINI) -lcmini -lgcc

# 8.3 short name — Atari ST / GEMDOS rejects long filenames.
BIN = HELLODBG.TOS
SRC = hello-debug.c
DIST_DIR = ./dist

.PHONY: all release clean

all: clean $(DIST_DIR)/$(BIN)

release: all

$(DIST_DIR)/$(BIN):
	mkdir -p $(DIST_DIR)
	$(CC) $(CFLAGS) $(SRC) -o $(DIST_DIR)/$(BIN) $(LINKFLAGS)
	@echo "$(BIN) size: `wc -c < $(DIST_DIR)/$(BIN)` bytes"

clean:
	rm -rf $(DIST_DIR)
