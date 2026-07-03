BIN := ark
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)
DEP := $(OBJ:.o=.d)
CXX := clang++
# -MMD -MP emit a per-object .d file listing every header the object depends
# on, so editing a header (e.g. a struct layout in token.h/ast.h) triggers a
# rebuild of everything that includes it. Without this, a stale .o compiled
# against an OLD struct layout links against new ones -- an ABI mismatch that
# corrupts the heap (manifested as random "pointer being freed was not
# allocated" / "__next_prime overflow" aborts). Learned the hard way, twice.
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Isrc -MMD -MP

# `make` builds ark AND the bundled pistin editor -- ark-settings opens pistin,
# so shipping them together means a clean install has an editor out of the box.
.DEFAULT_GOAL := all
all: $(BIN) pistin

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Bundled editor: pistin -- a single-translation-unit terminal IDE, vendored
# from its own repo (refreshed by scripts/arkbrewbuild at release time) into
# vendor/ (a dir named 'pistin' would collide with this binary target). Built
# with the same toolchain/standard as ark.
pistin: vendor/pistin.cpp
	$(CXX) -std=c++20 -O2 -Wall -Wextra -o $@ vendor/pistin.cpp

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEP)

.PHONY: clean test unittest check install
clean:
	rm -rf build $(BIN) pistin

# Install to a stable path so it can be a login shell that survives rebuilds.
# Re-run after `make` to update the installed copy.
PREFIX ?= /usr/local
install: all
	@mkdir -p $(PREFIX)/bin
	@# Apple Silicon: overwriting a Mach-O IN PLACE (same inode) leaves the
	@# kernel's cached code-signature (cdhash) stale for that path, and AMFI
	@# SIGKILLs the new binary on launch -- which shows up as Ghostty's 39ms
	@# "failed to launch the requested command". `rm` first so the copy lands
	@# on a FRESH inode, then re-sign ad-hoc so the on-disk signature matches
	@# the freshly-written bytes. (A plain in-place `cp` regressed this once.)
	@rm -f $(PREFIX)/bin/ark
	@cp $(BIN) $(PREFIX)/bin/ark
	@chmod u+rwx,go+rx $(PREFIX)/bin/ark
	@codesign --force --sign - $(PREFIX)/bin/ark 2>/dev/null || true
	@# pistin: the bundled editor, same fresh-inode + ad-hoc re-sign dance as ark.
	@rm -f $(PREFIX)/bin/pistin
	@cp pistin $(PREFIX)/bin/pistin
	@chmod u+rwx,go+rx $(PREFIX)/bin/pistin
	@codesign --force --sign - $(PREFIX)/bin/pistin 2>/dev/null || true
	@# assh: the "ark over SSH" companion (a plain script, no signing needed).
	@rm -f $(PREFIX)/bin/assh
	@cp assh $(PREFIX)/bin/assh && chmod u+rwx,go+rx $(PREFIX)/bin/assh
	@echo "installed ark -> $(PREFIX)/bin/ark (signed)  +  pistin (signed)  +  assh -> $(PREFIX)/bin/assh"

test: $(BIN)
	bash tests/run_tests.sh

# Per-module unit tests: each tests/*_test.cpp links against every non-main
# source (main.cpp excluded -- it has the real main()). Kept as a target so
# the link list stays correct as cross-module dependencies grow.
UNIT_SRC := $(filter-out src/main.cpp,$(SRC))
unittest:
	@for t in tests/*_test.cpp; do \
	  name=$$(basename $$t .cpp); \
	  $(CXX) $(CXXFLAGS) $$t $(UNIT_SRC) -o build/$$name 2>/dev/null && ./build/$$name || echo "FAIL: $$name"; \
	done

check: test unittest
