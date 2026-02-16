# DS1821 1-Wire Thermometer/Thermostat
# ─────────────────────────────────────

CC       := gcc
CFLAGS   := -Wall -Wextra -Wpedantic -std=c11 -g -O2

READ_SRC := ds1821-read.c
READ_BIN := ds1821-read
PROG_SRC := ds1821-program.c
PROG_BIN := ds1821-program

.PHONY: all clean install

all: $(READ_BIN) $(PROG_BIN)

$(READ_BIN): $(READ_SRC)
	$(CC) $(CFLAGS) -o $@ $(READ_SRC)

$(PROG_BIN): $(PROG_SRC)
	$(CC) $(CFLAGS) -o $@ $(PROG_SRC) -lpigpio -lrt -lpthread

PREFIX   ?= /usr/local
install: $(PROG_BIN)
	install -D -m 0755 $(PROG_BIN) $(DESTDIR)$(PREFIX)/bin/ds1821
	install -D -m 0755 ds1821-update $(DESTDIR)$(PREFIX)/bin/ds1821-update
	install -D -m 0644 ds1821-update.service $(DESTDIR)/lib/systemd/system/ds1821-update.service
	install -D -m 0644 ds1821-update.timer   $(DESTDIR)/lib/systemd/system/ds1821-update.timer
	install -D -m 0644 sensors.conf $(DESTDIR)/etc/ds1821/sensors.conf

clean:
	rm -f $(READ_BIN) $(PROG_BIN)
