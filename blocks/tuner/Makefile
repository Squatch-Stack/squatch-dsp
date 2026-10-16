# SPDX-License-Identifier: MIT
# Squatch Tuner: header-only core in include/, bench and tests built here. No dependencies.
# The hosts (hosts/ at the top of squatch-dsp) and the Block adapter build from the top Makefile.
ifeq ($(origin CXX),default)
CXX := $(if $(shell command -v clang++ 2>/dev/null),clang++,g++)
endif
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic -Wshadow -Iinclude
BUILD := build

BENCH_COMMON := bench/pv.cpp bench/poly_real.cpp
HEADERS := $(wildcard include/squatch/tuner/*.hpp) $(wildcard bench/*.hpp)

# Guitar-TECHS (CC BY 4.0) single-note DI files, downloaded separately; see RESULTS.md. Set
# GUITAR_TECHS on the command line or in an untracked local.mk beside this file.
-include local.mk
GUITAR_TECHS ?= data/guitar-techs
P1 := $(GUITAR_TECHS)/P1_singlenotes/P1_singlenotes/audio/directinput/directinput_allsinglenotes.wav
P2 := $(GUITAR_TECHS)/P2_singlenotes/P2_singlenotes/audio/directinput/directinput_allsinglenotes.wav

.PHONY: all test results stability lint clean
all: $(BUILD)/tests $(BUILD)/eval_synth $(BUILD)/eval_real $(BUILD)/eval_poly $(BUILD)/eval_stability $(BUILD)/perf

$(BUILD)/%: bench/%.cpp $(BENCH_COMMON) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $< $(BENCH_COMMON) -lpthread

$(BUILD)/tests: tests/test_core.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ $< $(BENCH_COMMON)

$(BUILD):
	mkdir -p $(BUILD)

# Stability in a room (flickers, restarts, jumps, held value). ROOM is a recording of a room's
# ambience (its first 10 s are used); SESSION is any live take to score without a reference.
ROOM ?=
SESSION ?=
stability: $(BUILD)/eval_stability
	./$(BUILD)/eval_stability $(if $(ROOM),--room $(ROOM)) --di $(P1) $(if $(SESSION),--session $(SESSION))

test: $(BUILD)/tests
	./$(BUILD)/tests

# Every number in RESULTS.md (about 20 minutes on an M1 Max; the 48 kHz baselines dominate).
results: all
	mkdir -p results
	./$(BUILD)/perf > results/perf.md
	./$(BUILD)/eval_poly 300 $(P1) > results/poly.md
	./$(BUILD)/eval_real $(P1) $(P2) > results/real.md
	VARIANTS=1 ./$(BUILD)/eval_real $(P1) $(P2) > results/real-variants.md
	PER_NOTE=1 ./$(BUILD)/eval_synth 8 --full-rate > results/synth.md

# Complexity limits of the Squatch project kit: CCN 10, 60 lines, 5 parameters per function.
lint:
	lizard -C 10 -L 60 -a 5 -w include bench tests

clean:
	rm -rf $(BUILD)
