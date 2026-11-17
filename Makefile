CFLAGS ?= -O2 -g -Wall -Wextra
PREFIX ?= /usr/local
# For the user unit's ExecStartPre (systemd wants an absolute path)
SLEEP ?= $(shell command -v sleep)
# Evaluated only by the targets that use them, so `make ucxcfg` needs no libusb
LIBUSB_CFLAGS = $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS = $(shell pkg-config --libs libusb-1.0)

ALSA_CFLAGS = $(shell pkg-config --cflags alsa)
ALSA_LIBS = $(shell pkg-config --libs alsa)

JANSSON_CFLAGS = $(shell pkg-config --cflags jansson)
JANSSON_LIBS = $(shell pkg-config --libs jansson)

all: build/ucxctl build/looptest ucxcfg ucxtui module

# The configuration tool and its library (lib/): config model + device access
ucxcfg: build/ucxcfg

LIBUCX_OBJS := build/lib/config.o build/lib/device.o

build/lib/%.o: lib/%.c lib/ucxcfg.h include/ucx_proto.h include/ucx_hwdep.h
	@mkdir -p build/lib
	$(CC) $(CFLAGS) -Iinclude $(ALSA_CFLAGS) $(JANSSON_CFLAGS) -c -o $@ $<

build/libucx.a: $(LIBUCX_OBJS)
	$(AR) rcs $@ $^

build/ucxcfg: tools/ucxcfg.c build/libucx.a lib/ucxcfg.h
	$(CC) $(CFLAGS) -Ilib -o $@ $< build/libucx.a $(ALSA_LIBS) $(JANSSON_LIBS) -lm

# The same config on screen: an editor on top of the library (tui/)
ucxtui: build/ucxtui

TUI_SRCS := tui/ucxtui.c tui/doc.c tui/term.c
TUI_HDRS := tui/doc.h tui/term.h lib/ucxcfg.h

build/ucxtui: $(TUI_SRCS) $(TUI_HDRS) build/libucx.a
	$(CC) $(CFLAGS) -Ilib -o $@ $(TUI_SRCS) build/libucx.a $(ALSA_LIBS) $(JANSSON_LIBS) -lm

build/doc-test: tests/doc.c tui/doc.c tui/doc.h build/libucx.a
	$(CC) $(CFLAGS) -Ilib -Itui -o $@ tests/doc.c tui/doc.c build/libucx.a $(ALSA_LIBS) $(JANSSON_LIBS) -lm

build/plan-test: tests/plan.c build/libucx.a lib/ucxcfg.h
	$(CC) $(CFLAGS) -Ilib -o $@ $< build/libucx.a $(ALSA_LIBS) $(JANSSON_LIBS) -lm

build/proto-test: tests/proto.c include/ucx_proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude -o $@ $< -lm

build/pcm-device-test: tests/pcm-device.c include/ucx_proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude $(ALSA_CFLAGS) -o $@ $< $(ALSA_LIBS)

check-pcm:
	python3 tests/pcm.py

# File format and protocol helper tests; no device needed
check: build/ucxcfg build/proto-test build/doc-test build/plan-test
	tests/config.sh build/ucxcfg
	build/proto-test
	build/doc-test
	build/plan-test

# libFuzzer on the config parser (tests/fuzz_config.c); not part of all or
# check. Needs clang and jansson only (lib/config.c, no ALSA, libusb or
# device). Runs FUZZ_TIME seconds on build/fuzz/corpus, seeded from
# examples/ and tests/config.sh; crashes land in build/fuzz/.
# FUZZ_ARGS passes more libFuzzer options (-fork=4; -runs=0 replays the corpus).
FUZZ_CC ?= clang
FUZZ_TIME ?= 60
FUZZ_ARGS ?=
FUZZ_FLAGS = -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
	-fno-sanitize-recover=undefined

build/fuzz/fuzz_config: tests/fuzz_config.c lib/config.c lib/ucxcfg.h
	@mkdir -p build/fuzz
	$(FUZZ_CC) $(FUZZ_FLAGS) -Ilib -Iinclude $(JANSSON_CFLAGS) -o $@ \
		tests/fuzz_config.c lib/config.c $(JANSSON_LIBS) -lm

fuzz: build/fuzz/fuzz_config
	@mkdir -p build/fuzz/corpus build/fuzz/seeds
	cp examples/*.json build/fuzz/seeds/
	UCX_SEEDS=build/fuzz/seeds tests/config.sh
	cd build/fuzz && ./fuzz_config -max_total_time=$(FUZZ_TIME) -max_len=65536 \
		-timeout=10 -dict=../../tests/fuzz_config.dict \
		$(FUZZ_ARGS) corpus seeds

install-ucxcfg: build/ucxcfg build/ucxtui
	install -Dm755 build/ucxcfg $(DESTDIR)$(PREFIX)/bin/ucxcfg
	install -Dm755 build/ucxtui $(DESTDIR)$(PREFIX)/bin/ucxtui
	install -Dm644 build/libucx.a $(DESTDIR)$(PREFIX)/lib/libucx.a
	install -Dm644 lib/ucxcfg.h $(DESTDIR)$(PREFIX)/include/ucxcfg.h
	install -Dm644 examples/desktop.json $(DESTDIR)$(PREFIX)/share/doc/ucxcfg/desktop.json
	install -Dm644 udev/70-snd-fireface-ucx.rules \
		$(DESTDIR)$(PREFIX)/lib/udev/rules.d/70-snd-fireface-ucx.rules
	install -Dm644 wireplumber/51-fireface-ucx-latency.conf \
		$(DESTDIR)$(PREFIX)/share/wireplumber/wireplumber.conf.d/51-fireface-ucx-latency.conf
	@mkdir -p $(DESTDIR)$(PREFIX)/lib/systemd/user
	sed -e 's|@BINDIR@|$(PREFIX)/bin|' -e 's|@SLEEP@|$(SLEEP)|' systemd/ucxcfg-apply.service.in \
		>$(DESTDIR)$(PREFIX)/lib/systemd/user/ucxcfg-apply.service

build/ucxctl: tools/ucxctl.c include/ucx_proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Iinclude $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS) -lm

build/looptest: tools/looptest.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(ALSA_CFLAGS) -o $@ $< $(ALSA_LIBS) -lm

module:
	$(MAKE) -C kmod

module-pcm-test: build/pcm-device-test
	mkdir -p build/pcm-kmod/kmod build/pcm-kmod/include
	cp kmod/Kbuild kmod/ucx.h kmod/ucx_*.c build/pcm-kmod/kmod/
	cp include/ucx_*.h build/pcm-kmod/include/
	$(MAKE) -C build/pcm-kmod/kmod -f $(CURDIR)/kmod/Makefile UCX_PCM_TEST=y

# As root, after `make`: install-module installs the kernel module, install
# the tools with their udev rule, systemd user unit and WirePlumber latency rule
install-module:
	$(MAKE) -C kmod install

install: install-ucxcfg

clean:
	rm -rf build
	$(MAKE) -C kmod clean

.PHONY: all module module-pcm-test clean ucxcfg ucxtui check check-pcm fuzz install install-ucxcfg install-module
