CC ?= gcc
CFLAGS ?= -O2
override CFLAGS += -std=c99 -Wall -Wextra -Wno-unused-parameter -Werror
override LDLIBS += -lasound -lm
all: bananad
bananad: bananad.c ../banana-retro/include/banana-retro-control.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)
install:
	install -D -m 755 bananad $(DESTDIR)/usr/sbin/bananad
clean:
	rm -f bananad
.PHONY: all install clean
