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

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEP)

.PHONY: clean test
clean:
	rm -rf build $(BIN)

test: $(BIN)
	bash tests/run_tests.sh
