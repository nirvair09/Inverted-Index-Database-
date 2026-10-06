CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude
SRCS     := src/analyzer.cpp src/segment.cpp src/query.cpp src/engine.cpp src/main.cpp
OBJS     := $(SRCS:src/%.cpp=build/%.o)
BIN      := indexdb

.PHONY: all clean demo

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp include/*.hpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build:
	mkdir -p build

demo: $(BIN)
	./$(BIN) --demo

clean:
	rm -rf build $(BIN)
