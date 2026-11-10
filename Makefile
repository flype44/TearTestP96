# TearTestP96: bebbo m68k-amigaos-gcc, libnix (-noixemul)
CC      = m68k-amigaos-gcc
CFLAGS  = -noixemul -O2 -m68020 -fomit-frame-pointer -Wall -Wextra -Wno-pointer-sign
TARGET  = TearTestP96

$(TARGET): TearTestP96.c
	$(CC) $(CFLAGS) -o $@ $< -s

clean:
	rm -f $(TARGET)

.PHONY: clean
