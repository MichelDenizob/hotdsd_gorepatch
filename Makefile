# HOTD: Scarlet Dawn — gore restoration patch
# created by Michel Denizob
#
#   make            -> build ./hotd_gore_patch  (requires zlib)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -Wall
LDLIBS  := -lz

hotd_gore_patch: hotd_gore_patch.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f hotd_gore_patch

.PHONY: clean
