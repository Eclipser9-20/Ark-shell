BIN := ark
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)
CXX := clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Isrc

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

.PHONY: clean test
clean:
	rm -rf build $(BIN)

test: $(BIN)
	bash tests/run_tests.sh
