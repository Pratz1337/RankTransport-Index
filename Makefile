PYTHON ?= python
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -I.
EXTERNAL_ROOT ?= external/competitors
EXTERNAL_DATASET ?= url
EXTERNAL_TRIALS ?= 5
EXTERNAL_BIN ?= hrtli_cpp/benchmark_external_bin
EXTERNAL_CPP_DEFS ?= -DHRTLI_WITH_ALEX -DHRTLI_WITH_LIPP -DHRTLI_WITH_PGM -DHRTLI_WITH_ART -DHRTLI_WITH_HOT
EXTERNAL_CXXFLAGS ?= -O3 -std=c++17 -mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -Ihrtli_cpp -I$(EXTERNAL_ROOT)/alex/src/core -I$(EXTERNAL_ROOT)/lipp/src/core -I$(EXTERNAL_ROOT)/pgm/include -I$(EXTERNAL_ROOT)/libart/src -I$(EXTERNAL_ROOT)/lits -I$(EXTERNAL_ROOT)/lits/lits -I$(EXTERNAL_ROOT)/hot/libs/hot/single-threaded/include -I$(EXTERNAL_ROOT)/hot/libs/hot/commons/include -I$(EXTERNAL_ROOT)/hot/libs/idx/content-helpers/include
EXTERNAL_SOURCES ?= $(EXTERNAL_ROOT)/libart/src/art.c

.PHONY: test data benchmark cpp-test cpp-benchmark cpp-concurrency-benchmark cpp-characterization-benchmark cpp-range-benchmark cpp-write-sweep cpp-range-degradation cpp-ablation-hpsfc cpp-external cpp-external-smoke external-baselines all

test:
	$(PYTHON) -m unittest discover -s tests

data:
	$(PYTHON) export_datasets.py

benchmark: data
	$(PYTHON) benchmark_q1.py
	$(PYTHON) benchmark_ablation.py
	$(PYTHON) benchmark_range_latency.py

cpp-test:
	$(CXX) $(CXXFLAGS) -o hrtli_cpp/test_rank_transport hrtli_cpp/test_rank_transport.cpp
	./hrtli_cpp/test_rank_transport

cpp-benchmark: data
	$(CXX) $(CXXFLAGS) -o hrtli_cpp/benchmark hrtli_cpp/benchmark.cpp
	./hrtli_cpp/benchmark

cpp-concurrency-benchmark: data
	$(CXX) $(CXXFLAGS) -o hrtli_cpp/benchmark_concurrency hrtli_cpp/benchmark_concurrency.cpp -pthread
	./hrtli_cpp/benchmark_concurrency

cpp-characterization-benchmark: data
	$(CXX) $(CXXFLAGS) -o hrtli_cpp/benchmark_characterization hrtli_cpp/benchmark_characterization.cpp -pthread
	./hrtli_cpp/benchmark_characterization



cpp-range-benchmark: data
	$(CXX) $(CXXFLAGS) -o hrtli_cpp/benchmark_range hrtli_cpp/benchmark_range.cpp
	./hrtli_cpp/benchmark_range synthetic
	sleep 1
	./hrtli_cpp/benchmark_range wiki_ts
	sleep 1
	./hrtli_cpp/benchmark_range osm_cellids

cpp-external: data
	$(CXX) $(EXTERNAL_CXXFLAGS) $(EXTERNAL_CPP_DEFS) hrtli_cpp/benchmark_external.cpp $(EXTERNAL_SOURCES) -o $(EXTERNAL_BIN)
	HRTLI_EXTERNAL_TRIALS=$(EXTERNAL_TRIALS) HRTLI_EXTERNAL_WRITE_JSON=1 ./$(EXTERNAL_BIN) $(EXTERNAL_DATASET)

cpp-external-smoke: data
	$(CXX) $(CXXFLAGS) -Ihrtli_cpp hrtli_cpp/benchmark_external.cpp -o hrtli_cpp/benchmark_external_smoke
	./hrtli_cpp/benchmark_external_smoke $(EXTERNAL_DATASET) 2

cpp-write-sweep: data
	$(CXX) $(EXTERNAL_CXXFLAGS) -DHRTLI_WITH_ALEX -DHRTLI_WITH_PGM -DHRTLI_WITH_ART -DHRTLI_WITH_HOT hrtli_cpp/benchmark_write_sweep.cpp $(EXTERNAL_SOURCES) -o hrtli_cpp/benchmark_write_sweep_bin
	./hrtli_cpp/benchmark_write_sweep_bin --trials $(EXTERNAL_TRIALS)

cpp-range-degradation: data
	$(CXX) $(EXTERNAL_CXXFLAGS) -DHRTLI_WITH_PGM -DHRTLI_WITH_HOT hrtli_cpp/benchmark_range_degradation.cpp -o hrtli_cpp/benchmark_range_degradation_bin
	./hrtli_cpp/benchmark_range_degradation_bin --trials $(EXTERNAL_TRIALS) dns

cpp-ablation-hpsfc: data
	$(CXX) $(CXXFLAGS) -o hrtli_cpp/benchmark_ablation_hpsfc hrtli_cpp/benchmark_ablation_hpsfc.cpp -pthread
	./hrtli_cpp/benchmark_ablation_hpsfc synthetic dns

external-baselines:
	bash scripts/setup_external_baselines.sh

all: test cpp-test benchmark cpp-benchmark cpp-range-benchmark cpp-external-smoke
