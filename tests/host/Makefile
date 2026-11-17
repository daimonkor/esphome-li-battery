CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable
CORE = ../../components/li_battery/li_battery_core.h legacy.h
DATA = ../data/ina226_hivescale_2026-11.csv.gz

test: test_core replay
	./test_core

test_core: test_core.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ test_core.cpp

test_replay: test_replay.cpp $(CORE)
	$(CXX) $(CXXFLAGS) -o $@ test_replay.cpp

ina226.csv: $(DATA)
	gzip -dc $(DATA) > $@

replay: test_replay ina226.csv
	./test_replay ina226.csv

clean:
	rm -f test_core test_replay ina226.csv

.PHONY: test replay clean
