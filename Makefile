VERSION    ?= 0.1.0
PREFIX     ?= /usr/local
CC         ?= cc
PKG_CONFIG ?= pkg-config
PYTHON     ?= python3

BUILD := build
UNAME := $(shell uname -s)

# json-c headers as system headers: their warnings are not ours.
JSONC_CFLAGS := $(patsubst -I%,-isystem %,$(shell $(PKG_CONFIG) --cflags json-c))
JSONC_LIBS   := $(shell $(PKG_CONFIG) --libs json-c)

CFLAGS   ?= -O2 -g
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes \
            -Wpointer-arith -Wcast-qual -Wvla
CPPFLAGS += -Iinclude -DJD_VERSION='"$(VERSION)"'
LDLIBS   += -lm
ifeq ($(UNAME),Linux)
CPPFLAGS += -D_DEFAULT_SOURCE
LDLIBS   += -ldl
endif
ALL_CFLAGS = -std=c11 $(WARNINGS) $(CFLAGS) $(JSONC_CFLAGS)

ifeq ($(WERROR),1)
ALL_CFLAGS += -Werror
endif
ifeq ($(SANITIZE),1)
ALL_CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
LDFLAGS    += -fsanitize=address,undefined
endif

PLUGIN_CFLAGS = $(ALL_CFLAGS) -fPIC -fvisibility=hidden
PLUGIN_LDFLAGS = -shared $(LDFLAGS)

SRCS    := $(wildcard src/*.c)
OBJS    := $(SRCS:src/%.c=$(BUILD)/obj/%.o)
PLUGINS := $(patsubst plugins/%/,$(BUILD)/plugins/%.so,$(wildcard plugins/*/))
FIXTURES := $(addprefix $(BUILD)/fixtures/,hello-bonjour.so slow.so slow-ts.so \
            bad-no-mount.so bad-reserved.so bad-init.so)

.PHONY: all plugins fixtures test install uninstall clean

all: $(BUILD)/jsondaemon plugins

plugins: $(PLUGINS)

fixtures: $(FIXTURES)

$(BUILD)/jsondaemon: $(OBJS)
	$(CC) $(LDFLAGS) -pthread -o $@ $^ $(JSONC_LIBS) $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c $(wildcard src/*.h) include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -pthread -c -o $@ $<

# A plugin is one directory of .c files under plugins/.
.SECONDEXPANSION:
$(BUILD)/plugins/%.so: $$(wildcard plugins/%/*.c) include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $(filter %.c,$^) $(JSONC_LIBS) -lm

$(BUILD)/fixtures/hello-bonjour.so: plugins/hello/hello.c include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DHELLO_GREETING='"Bonjour"' $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $< $(JSONC_LIBS)

$(BUILD)/fixtures/slow.so: tests/fixtures/slow.c include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $< $(JSONC_LIBS)

$(BUILD)/fixtures/slow-ts.so: tests/fixtures/slow.c include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DSLOW_THREAD_SAFE $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $< $(JSONC_LIBS)

$(BUILD)/fixtures/bad-no-mount.so: tests/fixtures/bad.c include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DBAD_NO_MOUNT $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $< $(JSONC_LIBS)

$(BUILD)/fixtures/bad-reserved.so: tests/fixtures/bad.c include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DBAD_RESERVED $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $< $(JSONC_LIBS)

$(BUILD)/fixtures/bad-init.so: tests/fixtures/bad.c include/jsondaemon.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -DBAD_INIT $(PLUGIN_CFLAGS) $(PLUGIN_LDFLAGS) -o $@ $< $(JSONC_LIBS)

test: all fixtures
	JD_BIN=$(BUILD)/jsondaemon JD_PLUGINS=$(BUILD)/plugins JD_FIXTURES=$(BUILD)/fixtures \
		$(PYTHON) -m unittest discover -s tests -v

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib/jsondaemon/plugins
	install -m 755 $(BUILD)/jsondaemon $(DESTDIR)$(PREFIX)/bin/jsondaemon
	install -m 644 include/jsondaemon.h $(DESTDIR)$(PREFIX)/include/jsondaemon.h
	install -m 755 $(PLUGINS) $(DESTDIR)$(PREFIX)/lib/jsondaemon/plugins/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jsondaemon $(DESTDIR)$(PREFIX)/include/jsondaemon.h
	rm -rf $(DESTDIR)$(PREFIX)/lib/jsondaemon

clean:
	rm -rf $(BUILD)
