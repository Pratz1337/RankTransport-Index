PYTHON ?= python3
CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -I.
EXTERNAL_ROOT ?= external/competitors
EXTERNAL_DATASET ?= url
EXTERNAL_TRIALS ?= 5
EXTERNAL_BIN ?= hrtli_cpp/benchmark_external_bin
EXTERNAL_CPP_DEFS ?= -DHRTLI_WITH_ALEX -DHRTLI_WITH_LIPP -DHRTLI_WITH_PGM -DHRTLI_WITH_ART -DHRTLI_WITH_HOT
EXTERNAL_CXXFLAGS ?= -O3 -std=c++17 -mavx2 -mbmi -mbmi2 -mlzcnt -mpopcnt -Ihrtli_cpp -I$(EXTERNAL_ROOT)/alex/src/core -I$(EXTERNAL_ROOT)/lipp/src/core -I$(EXTERNAL_ROOT)/pgm/include -I$(EXTERNAL_ROOT)/libart/src -I$(EXTERNAL_ROOT)/lits -I$(EXTERNAL_ROOT)/lits/lits -I$(EXTERNAL_ROOT)/hot/libs/hot/single-threaded/include -I$(EXTERNAL_ROOT)/hot/libs/hot/commons/include -I$(EXTERNAL_ROOT)/hot/libs/idx/content-helpers/include
EXTERNAL_SOURCES ?= $(EXTERNAL_ROOT)/libart/src/art.c

.PHONY: figures fig1 table1 replicated-summary verify paper-check test data benchmark cpp-test cpp-benchmark cpp-concurrency-benchmark cpp-characterization-benchmark cpp-range-benchmark cpp-write-sweep cpp-range-degradation cpp-ablation-hpsfc cpp-external cpp-external-smoke external-baselines all

# ---------------------------------------------------------------- paper artefacts
# Figs. 2-6 (PDF) from results/; needs matplotlib and a LaTeX install (text.usetex).
figures:
	$(PYTHON) figures/make_figs.py

# Fig. 1 is TikZ (no data input).
fig1:
	cd figures && pdflatex -interaction=nonstopmode fig1_overview.tex && rm -f fig1_overview.aux fig1_overview.log

# Table 1 rows (LaTeX) from results/replicated_rankcap/replicated_summary.json.
table1:
	$(PYTHON) figures/make_table1.py

# Re-audit the ten replicate records and rebuild replicated_summary.json.
replicated-summary:
	$(PYTHON) figures/summarize_replicates.py

# SHA-256 identity of every experiment record listed in docs/BUILD_PROVENANCE.json.
verify:
	$(PYTHON) scripts/verify_release.py

paper-check: verify replicated-summary table1 figures fig1

# ---------------------------------------------------------------- tests and earlier benchmarks
test:
	$(PYTHON) -m unittest discover -s tests

data:
	$(PYTHON) -m corpora.export_datasets

benchmark: data
	$(PYTHON) -m benchmarks.python.benchmark_q1_full
	$(PYTHON) -m benchmarks.python.benchmark_ablation
	$(PYTHON) -m benchmarks.python.benchmark_range_latency

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
