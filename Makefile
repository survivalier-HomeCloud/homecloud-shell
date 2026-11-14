# HomeCloud Shell : un compilateur C et make suffisent.
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -fstack-protector-strong -Isrc
LDLIBS  += -lutil -lpthread
COMMON   = src/util.c src/ui.c src/proto.c src/config.c
SERVER   = $(COMMON) src/auth.c src/admin.c src/sandbox.c src/identity.c src/ports.c src/server.c src/shelld.c
CLIENT   = $(COMMON) src/client.c
PREFIX  ?= /usr/local

all: homecloud-shelld homecloud-shell

homecloud-shelld: $(SERVER) $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(SERVER) -o $@ $(LDFLAGS) $(LDLIBS)

homecloud-shell: $(CLIENT) $(wildcard src/*.h)
	$(CC) $(CFLAGS) $(CLIENT) -o $@ $(LDFLAGS) $(LDLIBS)

install: all
	install -Dm755 homecloud-shelld $(DESTDIR)$(PREFIX)/bin/homecloud-shelld
	install -Dm755 homecloud-shell  $(DESTDIR)$(PREFIX)/bin/homecloud-shell

clean:
	rm -f homecloud-shelld homecloud-shell

.PHONY: all install clean
