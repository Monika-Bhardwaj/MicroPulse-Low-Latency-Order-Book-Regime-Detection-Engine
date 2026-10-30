# Quick-build Makefile (CMakeLists.txt is the canonical build; this mirrors it).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -pthread -Iinclude
BUILD    ?= build

.PHONY: all test bench python clean
all: $(BUILD)/test_main $(BUILD)/bench_main

$(BUILD)/test_main: tests/test_main.cpp $(wildcard include/micropulse/*.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/bench_main: bench/bench_main.cpp $(wildcard include/micropulse/*.hpp)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -O3 -DNDEBUG $< -o $@

test: $(BUILD)/test_main
	./$(BUILD)/test_main

bench: $(BUILD)/bench_main
	./$(BUILD)/bench_main all

python:
	@mkdir -p python/micropulse_research
	$(CXX) -std=c++17 -O3 -DNDEBUG -shared -fPIC -Iinclude \
	  $$(python3 -m pybind11 --includes) bindings/pybind_module.cpp \
	  -o python/micropulse_research/_micropulse$$(python3-config --extension-suffix)

clean:
	rm -rf $(BUILD)
