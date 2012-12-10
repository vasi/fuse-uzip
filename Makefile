OPT = -O0 -g

PREFIX = /usr

FUSE_CFLAGS = -D_FILE_OFFSET_BITS=64

.PHONY: all clean

all: uzip

clean:
	rm -rf uzip uzip.dSYM

uzip: uzip.c
	$(CC) $(OPT) $(FUSE_CFLAGS) -I$(PREFIX)/include -o $@ $< -L$(PREFIX)/lib -lfuse -llzma -lz
