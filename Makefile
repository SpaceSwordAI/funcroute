# funcroute - C23 OpenAI-compatible request router
#
# Builds with C23 (gcc -std=c2x on gcc 13; plain -std=c23 on gcc 14+).
# Dependencies (external, via pkg-config):
#   libcurl     - upstream HTTP forwarding
#   libjansson  - JSON parse / rewrite
#   libmicrohttpd - embedded HTTP server

CC     ?= gcc
STD    := c2x
PKGS   := libcurl jansson libmicrohttpd openssl sqlite3

CFLAGS += -std=$(STD) -O2 -g -Wall -Wextra -Wpedantic
CFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))
LDLIBS += -pthread

TARGET := funcroute
CLIENT := funcroute-client
SRCS   := $(wildcard src/*.c)
OBJS   := $(SRCS:.c=.o)

all: $(TARGET) $(CLIENT)

$(TARGET): $(filter-out src/client.o,$(OBJS))
	$(CC) $(CFLAGS) -o $@ $(filter-out src/client.o,$(OBJS)) $(LDLIBS)

$(CLIENT): src/client.o
	$(CC) $(CFLAGS) -o $@ src/client.o $(LDLIBS)

src/%.o: src/%.c src/config.h src/provider.h src/logdb.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(CLIENT)

.PHONY: all clean
