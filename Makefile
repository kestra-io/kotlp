# kotlp - portable OpenTelemetry process wrapper
#
# Builds a single Actually Portable Executable (APE) with the Cosmopolitan
# toolchain. The resulting binary runs natively on Linux, macOS, Windows,
# FreeBSD, OpenBSD and NetBSD across amd64 and arm64.

NAME            := kotlp
COSMOCC_VERSION := 4.0.2
COSMOCC_SHA256  := 85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44
COSMOCC_DIR     := build/cosmocc
# GitHub release first: cosmo.zip has been unreachable from CI runners.
COSMOCC_URLS    := https://github.com/jart/cosmopolitan/releases/download/$(COSMOCC_VERSION)/cosmocc-$(COSMOCC_VERSION).zip \
                   https://cosmo.zip/pub/cosmocc/cosmocc-$(COSMOCC_VERSION).zip
SHA256SUM       := $(shell command -v sha256sum 2>/dev/null || echo shasum -a 256)
BIN             := build/$(NAME)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/obj/%.o)

# Unit tests link against the project objects (minus main.o, which the test
# runner replaces with its own main()).
TEST_SRC  := $(wildcard tests/*.c)
TEST_OBJ  := $(TEST_SRC:tests/%.c=build/obj/tests/%.o)
LIB_OBJ   := $(filter-out build/obj/main.o,$(OBJ))
TEST_BIN  := build/test-unit

# Prefer a cosmocc already on PATH; otherwise use the toolchain we download
# into build/cosmocc (see the toolchain target).
COSMOCC := $(shell command -v cosmocc 2>/dev/null)
ifeq ($(COSMOCC),)
COSMOCC := $(abspath $(COSMOCC_DIR))/bin/cosmocc
# When relying on the bundled toolchain, make the objects order-only depend on
# its presence so it is fetched once and never triggers a rebuild afterwards.
TOOLCHAIN_DEP := $(COSMOCC_DIR)/bin/cosmocc
endif

# Release builds pass VERSION=<tag> (see .github/workflows/release.yml) so the
# binary reports exactly the git tag it was released under. Local/dev builds
# fall back to `git describe`, or "dev" outside a git checkout.
VERSION := $(shell git describe --tags --dirty --always 2>/dev/null || echo dev)

CFLAGS  := -O2 -g -std=c11 -D_GNU_SOURCE -DKOTLP_VERSION=\"$(VERSION)\" -Wall -Wextra -Iinclude -pthread
LDFLAGS := -pthread

.PHONY: all clean distclean toolchain test test-unit check run install help

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(COSMOCC) $(LDFLAGS) -o $@ $(OBJ)
	@echo "built $@ (APE: runs on Linux/macOS/Windows/BSD, amd64+arm64)"

build/obj/%.o: src/%.c | $(TOOLCHAIN_DEP)
	@mkdir -p $(dir $@)
	$(COSMOCC) $(CFLAGS) -c -o $@ $<

build/obj/tests/%.o: tests/%.c | $(TOOLCHAIN_DEP)
	@mkdir -p $(dir $@)
	$(COSMOCC) $(CFLAGS) -Itests -c -o $@ $<

# Download and unpack the Cosmopolitan toolchain locally.
toolchain: $(COSMOCC_DIR)/bin/cosmocc
$(COSMOCC_DIR)/bin/cosmocc:
	@echo "downloading cosmocc $(COSMOCC_VERSION)..."
	@mkdir -p $(COSMOCC_DIR)
	@for url in $(COSMOCC_URLS); do \
		curl -fsSL --retry 3 --connect-timeout 20 -o build/cosmocc.zip "$$url" && break; \
		echo "failed to download $$url" >&2; rm -f build/cosmocc.zip; \
	done; test -f build/cosmocc.zip
	@echo "$(COSMOCC_SHA256)  build/cosmocc.zip" | $(SHA256SUM) -c -
	@unzip -q -o build/cosmocc.zip -d $(COSMOCC_DIR)
	@rm -f build/cosmocc.zip
	@chmod +x $(COSMOCC_DIR)/bin/* 2>/dev/null || true
	@echo "toolchain ready: $(COSMOCC_DIR)/bin/cosmocc"

# Unit tests (fast, pure-function level).
$(TEST_BIN): $(TEST_OBJ) $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(COSMOCC) $(LDFLAGS) -o $@ $(TEST_OBJ) $(LIB_OBJ)

test-unit: $(TEST_BIN)
	@$(TEST_BIN)

# Integration smoke test (runs the real APE end-to-end).
test: $(BIN)
	@scripts/smoke_test.sh $(abspath $(BIN))

# Everything: unit tests then the end-to-end smoke test.
check: test-unit test

run: $(BIN)
	@$(BIN) -- sh -c 'echo hello from child; sleep 1; echo done >&2'

install: $(BIN)
	install -d $(DESTDIR)/usr/local/bin
	install -m 0755 $(BIN) $(DESTDIR)/usr/local/bin/$(NAME)

clean:
	# cosmocc emits .aarch64.elf/.com.dbg sidecars next to each binary.
	rm -rf build/obj $(BIN) $(TEST_BIN)
	rm -f $(BIN).aarch64.elf $(BIN).com.dbg \
	      $(TEST_BIN).aarch64.elf $(TEST_BIN).com.dbg

distclean:
	rm -rf build

help:
	@echo "targets: all (default), toolchain, test-unit, test, check, run, install, clean, distclean"
