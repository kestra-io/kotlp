# koltp - portable OpenTelemetry process wrapper
#
# Builds a single Actually Portable Executable (APE) with the Cosmopolitan
# toolchain. The resulting binary runs natively on Linux, macOS, Windows,
# FreeBSD, OpenBSD and NetBSD across amd64 and arm64.

NAME            := koltp
COSMOCC_VERSION := 4.0.2
COSMOCC_DIR     := build/cosmocc
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

CFLAGS  := -O2 -g -std=c11 -D_GNU_SOURCE -Wall -Wextra -Iinclude -pthread
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
	@curl -fsSL -o build/cosmocc.zip \
		https://cosmo.zip/pub/cosmocc/cosmocc-$(COSMOCC_VERSION).zip
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
	rm -rf build/obj $(BIN) $(TEST_BIN)

distclean:
	rm -rf build

help:
	@echo "targets: all (default), toolchain, test-unit, test, check, run, install, clean, distclean"
