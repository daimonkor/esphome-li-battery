CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-function

test: test_core
	./test_core

test_core: test_core.cpp ../../components/li_battery/li_battery_core.h
	$(CXX) $(CXXFLAGS) -o $@ test_core.cpp

clean:
	rm -f test_core

.PHONY: test clean
