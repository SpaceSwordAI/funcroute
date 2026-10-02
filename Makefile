# funcroute - C23 OpenAI-compatible request router
#
# Builds with C23 (gcc -std=c2x on gcc 13; plain -std=c23 on gcc 14+).
# Dependencies (external, via pkg-config):
#   libcurl       - upstream HTTP forwarding
#   libjansson    - JSON parse / rewrite
#   libmicrohttpd - embedded HTTP server
#   openssl       - base64
#   sqlite3       - optional request log
#
#   make                  build ./funcroute and ./funcroute-client
#   make test             build, then run the offline routing test (no API keys)
#   make install          install into $(DESTDIR)$(PREFIX)/bin
#   make clean
#
# VERSION is stamped into both binaries and reported by GET /api/version and
# the startup banner. The release workflow passes the git tag:
#   make VERSION=1.2.3

CC       ?= gcc
VERSION  ?= 0.5.0
STD      := c2x
PKGS     := libcurl jansson libmicrohttpd openssl sqlite3
PREFIX   ?= /usr/local
DESTDIR  ?=
BINDIR   ?= $(PREFIX)/bin

CPPFLAGS += -DFUNCROUTE_VERSION=\"$(VERSION)\"
CFLAGS   += -std=$(STD) -O2 -g -Wall -Wextra -Wpedantic
CFLAGS   += -D_POSIX_C_SOURCE=200809L
CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))
LDLIBS   += -pthread

# macOS release tarballs rewrite library paths with install_name_tool, which
# needs slack in the Mach-O header to write the longer @executable_path names.
ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-headerpad_max_install_names
endif

TARGET := funcroute
CLIENT := funcroute-client
SRCS   := $(wildcard src/*.c)
OBJS   := $(SRCS:.c=.o)
HDRS   := $(wildcard src/*.h)

all: $(TARGET) $(CLIENT)

$(TARGET): $(filter-out src/client.o,$(OBJS))
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter-out src/client.o,$(OBJS)) $(LDLIBS)

$(CLIENT): src/client.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ src/client.o $(LDLIBS)

src/%.o: src/%.c $(HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: all
	python3 test/routing_test.py

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) $(CLIENT) "$(DESTDIR)$(BINDIR)"

clean:
	rm -f $(OBJS) $(TARGET) $(CLIENT)

.PHONY: all test install clean
