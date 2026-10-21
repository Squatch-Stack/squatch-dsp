# SPDX-License-Identifier: MIT
# squatch-dsp: the core library, its tests and benchmarks. System clang++, no fetched dependencies.
# clang++ where it exists (this Mac), g++ otherwise (the Pi). CXX=... on the command line wins.
ifeq ($(origin CXX),default)
CXX := $(if $(shell command -v clang++ 2>/dev/null),clang++,g++)
endif
ifeq ($(origin CC),default)
CC := $(if $(shell command -v clang 2>/dev/null),clang,gcc)
endif
BUILD := build
UNAME := $(shell uname -s)

WARN := -Wall -Wextra -Wpedantic -Wshadow -Werror
CORE := -std=c++17 -fno-exceptions -fno-rtti $(WARN) -Iinclude -Iblocks/tuner/include
RELEASE := $(CORE) -O3
SANITIZE := $(CORE) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
TSAN := $(CORE) -O1 -g -fsanitize=thread

HEADERS := $(shell find include -name '*.hpp')
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=$(BUILD)/obj/%.o)
TEST_SRC := $(filter-out tests/test_queue_threads.cpp tests/test_model_swap_threads.cpp,$(wildcard tests/*.cpp))
TEST_DEPS := $(wildcard tests/*.hpp) $(wildcard tests/golden/*.inc)

# Locks taken inside process() are counted by an interposing dylib (dyld honours __interpose in
# a linked library). Elsewhere the lock check is reported as unmeasured, never as passed.
ifeq ($(UNAME),Darwin)
LOCKS := $(BUILD)/liblockcount.dylib
LOCK_LINK := -L$(BUILD) -llockcount -Wl,-rpath,@loader_path
BENCH_OUT ?= results/bench-mac.md
else
TEST_DEFS := -DSQUATCH_NO_LOCK_COUNT
BENCH_OUT ?= results/bench-$(shell uname -m).md
endif

.PHONY: all lib test test-tsan test-tsan-negative lint headers bench golden ci live rig wasm wasm-test wasm-shots record-session-test clean FORCE \
        contracts contracts-check tuner-csv plugins plugins-install plugins-check export-check release-scan-test lv2 lv2-linux
all: lib $(BUILD)/tests $(BUILD)/bench

# The core as the targets build it: optimised, no exceptions, no RTTI.
lib: $(BUILD)/libsquatch-dsp.a

$(BUILD)/obj/%.o: src/%.cpp $(HEADERS) $(wildcard blocks/tuner/include/squatch/tuner/*.hpp) | $(BUILD)/obj
	$(CXX) $(RELEASE) -c -o $@ $<

$(BUILD)/libsquatch-dsp.a: $(OBJ)
	ar rcs $@ $^

$(BUILD)/liblockcount.dylib: tests/lockcount.c | $(BUILD)
	$(CC) -O1 -Wall -Wextra -Werror -dynamiclib -o $@ $<

# Tests compile the sources again with ASan and UBSan.
# The tuner's headers live under blocks/, outside $(HEADERS); without them here an edit to tuner.hpp left the
# test binary stale and every tuner test passed against the old code (found 2026-10-02).
$(BUILD)/tests: $(TEST_SRC) $(SRC) $(HEADERS) $(wildcard blocks/tuner/include/squatch/tuner/*.hpp) $(TEST_DEPS) $(LOCKS) | $(BUILD)
	$(CXX) $(SANITIZE) $(TEST_DEFS) -o $@ $(TEST_SRC) $(SRC) $(LOCK_LINK)

# The Seed3 firmware's measurement code and its overdrive block, on this machine, with ASan, UBSan
# and the allocation watch. The firmware itself is `make -C hosts/daisy` (arm-none-eabi-gcc;
# hosts/daisy/README.md). `make test` also runs the Python tools' guard tests there.
DAISY := $(wildcard hosts/daisy/tests)
ifneq ($(DAISY),)
DAISY_TEST_SRC := $(wildcard hosts/daisy/tests/*.cpp) tests/runner.cpp tests/rt_watch.cpp
DAISY_HEADERS := $(wildcard hosts/daisy/common/*.hpp hosts/daisy/faust/*.hpp)
$(BUILD)/test_daisy: $(DAISY_TEST_SRC) $(DAISY_HEADERS) $(HEADERS) $(LOCKS) | $(BUILD)
	$(CXX) $(SANITIZE) $(TEST_DEFS) -o $@ $(DAISY_TEST_SRC) $(LOCK_LINK)
endif

# The parameter queue is tested across threads, so it gets ThreadSanitizer instead.
$(BUILD)/test_queue_threads: tests/test_queue_threads.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(TSAN) -o $@ $< -lpthread

# ModelSwap's handoff across a control and an audio thread. test-tsan runs the stress under
# ThreadSanitizer and again under ASan and UBSan. test-tsan-negative rebuilds it three times with
# one memory ordering of the handoff weakened (SQUATCH_BREAK_HANDOFF=1,2,3) and requires
# ThreadSanitizer to report a data race each time: a guard only counts if breaking it fails.
# TSan refuses the Pi 5's 47-bit address space (see HOST_SAN below), so this is a Mac/x86 target.
SWAP_THREADS := tests/test_model_swap_threads.cpp src/model_swap.cpp src/cabinet.cpp src/crossfade.cpp src/fft.cpp

$(BUILD)/test_swap_tsan: $(SWAP_THREADS) $(HEADERS) tests/signals.hpp | $(BUILD)
	$(CXX) $(TSAN) -o $@ $(SWAP_THREADS) -lpthread

$(BUILD)/test_swap_asan: $(SWAP_THREADS) $(HEADERS) tests/signals.hpp | $(BUILD)
	$(CXX) $(SANITIZE) -o $@ $(SWAP_THREADS) -lpthread

$(BUILD)/test_swap_broken%: $(SWAP_THREADS) $(HEADERS) tests/signals.hpp | $(BUILD)
	$(CXX) $(TSAN) -DSQUATCH_BREAK_HANDOFF=$* -o $@ $(SWAP_THREADS) -lpthread

test-tsan: $(BUILD)/test_swap_tsan $(BUILD)/test_swap_asan test-tsan-negative
	./$(BUILD)/test_swap_tsan
	./$(BUILD)/test_swap_asan

test-tsan-negative: $(BUILD)/test_swap_broken1 $(BUILD)/test_swap_broken2 $(BUILD)/test_swap_broken3
	@for v in 1 2 3; do \
	  if ./$(BUILD)/test_swap_broken$$v > $(BUILD)/swap_broken$$v.log 2>&1; then \
	    echo "test-tsan-negative: handoff variant $$v ran clean, so the stress test would not notice it"; exit 1; fi; \
	  grep -q 'WARNING: ThreadSanitizer: data race' $(BUILD)/swap_broken$$v.log || { echo "variant $$v failed without a race report"; exit 1; }; \
	  echo "test-tsan-negative: variant $$v caught ($$(grep -c 'WARNING: ThreadSanitizer: data race' $(BUILD)/swap_broken$$v.log) race reports)"; \
	done

# Hosts (hosts/): not core, so exceptions are allowed, but the same warnings. The host layer's
# handoff is tested for data races with TSan, which refuses the Pi 5's 47-bit address space:
# there, `make test HOST_SAN=`.
HOST := -std=c++17 -O3 $(WARN) -Iinclude -Iblocks/tuner/include
HOST_SAN ?= -fsanitize=thread
HOST_HEADERS := $(wildcard hosts/common/*.hpp) $(wildcard hosts/jack/*.hpp) $(wildcard hosts/coreaudio/*.hpp) $(HEADERS)
TUNER_HEADERS := $(wildcard blocks/tuner/include/squatch/tuner/*.hpp)

$(BUILD)/test_hosts: hosts/tests/test_hosts.cpp $(HOST_HEADERS) $(TUNER_HEADERS) | $(BUILD)
	$(CXX) $(HOST) -O1 -g $(HOST_SAN) -o $@ $< -lpthread

# The CoreAudio host's audio path, offline (no device, no CoreAudio types: runs anywhere). ASan and UBSan.
$(BUILD)/test_host_audio: hosts/tests/test_host_audio.cpp $(HOST_HEADERS) $(TUNER_HEADERS) | $(BUILD)
	$(CXX) $(HOST) -O1 -g -fsanitize=address,undefined -o $@ $< -lpthread

# The live tuner and its test-tone generator. On macOS they use CoreAudio directly (hosts/coreaudio): only
# system frameworks, nothing to install. Elsewhere they use JACK (hosts/jack; needs libjack-jackd2-dev).
# `make live LIVE_BACKEND=jack` forces JACK on a Mac that has it. Built on the Pi bench, not part of
# `all`. hosts/pi/ expects them in build/.
ifeq ($(shell uname -s),Darwin)
LIVE_BACKEND ?= coreaudio
else
LIVE_BACKEND ?= jack
endif
ifeq ($(LIVE_BACKEND),coreaudio)
LIVE_LIBS := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation -lpthread
# The tuner reads a microphone: its Info.plist (embedded in the binary) carries the text macOS shows when
# it asks, so a launch from anywhere but Terminal is asked instead of being killed.
TUNER_LINK := -Wl,-sectcreate,__TEXT,__info_plist,hosts/coreaudio/Info.plist
else
LIVE_LIBS := -ljack -lpthread
TUNER_LINK :=
endif
live: $(BUILD)/squatch-tuner-live $(BUILD)/squatch-tone

$(BUILD)/squatch-tuner-live: hosts/$(LIVE_BACKEND)/tuner_live.cpp $(HOST_HEADERS) $(TUNER_HEADERS) | $(BUILD)
	$(CXX) $(HOST) -o $@ $< $(LIVE_LIBS) $(TUNER_LINK)

$(BUILD)/squatch-tone: hosts/$(LIVE_BACKEND)/tone.cpp $(HOST_HEADERS) | $(BUILD)
	$(CXX) $(HOST) -o $@ $< $(LIVE_LIBS)

# The live rig (gate, tuner, amp, cabinet, EQ, limiter as one JACK client) and the loopback
# latency probe. JACK headers as for `live`; not part of `all`. The amp stage runs NAM Core,
# which is optional and never part of the library: name a checkout with NAM_CORE=DIR (its NAM/
# sources; Eigen and nlohmann/json from NAM_EIGEN and NAM_JSON, by default its Dependencies/).
# Without NAM_CORE the rig builds and runs with the amp bypassed.
NAM_CORE ?=
NAM_EIGEN ?= $(NAM_CORE)/Dependencies/eigen
NAM_JSON ?= $(NAM_CORE)/Dependencies/nlohmann
NAM_ARCH ?= $(if $(filter Linux-aarch64,$(UNAME)-$(shell uname -m)),-mcpu=native,)
NAM_INC := -isystem $(NAM_CORE) -isystem $(NAM_EIGEN) -isystem $(NAM_JSON)
NAM_FLAGS := -std=c++17 -O3 -DNDEBUG $(NAM_ARCH) -DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DEIGEN_DONT_PARALLELIZE $(NAM_INC)
NAM_SRC := $(if $(NAM_CORE),$(wildcard $(NAM_CORE)/NAM/*.cpp $(NAM_CORE)/NAM/wavenet/*.cpp))
NAM_OBJ := $(patsubst $(NAM_CORE)/NAM/%.cpp,$(BUILD)/nam/%.o,$(NAM_SRC))
RIG_NAM := $(if $(NAM_CORE),-DSQUATCH_WITH_NAM -DNDEBUG -DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DEIGEN_DONT_PARALLELIZE $(NAM_INC))
RIG_HEADERS := $(wildcard hosts/jack/rig_*.hpp) hosts/jack/nam_amp.hpp hosts/common/ir_file.hpp

rig: $(BUILD)/squatch-rig-live $(BUILD)/squatch-latency

# Rebuilds the rig when NAM_CORE changes (with or without the amp).
$(BUILD)/rig.cfg: FORCE | $(BUILD)
	@echo '$(NAM_CORE)' | cmp -s - $@ || echo '$(NAM_CORE)' > $@

$(BUILD)/nam/%.o: $(NAM_CORE)/NAM/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(NAM_FLAGS) -c -o $@ $<

$(BUILD)/squatch-rig-live: hosts/jack/rig_live.cpp $(RIG_HEADERS) $(HOST_HEADERS) $(TUNER_HEADERS) $(BUILD)/libsquatch-dsp.a \
                           $(NAM_OBJ) $(BUILD)/rig.cfg | $(BUILD)
	$(CXX) $(HOST) $(RIG_NAM) -o $@ $< $(NAM_OBJ) $(BUILD)/libsquatch-dsp.a -ljack -lpthread

$(BUILD)/squatch-latency: hosts/jack/latency_probe.cpp $(HOST_HEADERS) | $(BUILD)
	$(CXX) $(HOST) -o $@ $< -ljack -lpthread

# The rig's chain over a recording, no JACK: any machine (squatch-rig-render IN.wav OUT.wav ...).
render: $(BUILD)/squatch-rig-render

$(BUILD)/squatch-rig-render: hosts/render/rig_render.cpp $(RIG_HEADERS) $(TUNER_HEADERS) $(BUILD)/libsquatch-dsp.a \
                             $(NAM_OBJ) $(BUILD)/rig.cfg | $(BUILD)
	$(CXX) $(HOST) $(RIG_NAM) -o $@ $< $(NAM_OBJ) $(BUILD)/libsquatch-dsp.a -lpthread

# The tuner over a recording as display-frame CSV rows (squatch-tuner-csv IN.wav > rows.csv), and
# the ESP32-S3 tuner probe's own code built for this machine (hosts/esp32/tuner-probe): any machine.
# The probe is development-tree only (tools/private-paths): without it only the CSV tool builds.
PROBE_HEADERS := $(wildcard hosts/esp32/tuner-probe/main/*.hpp)
PROBE := $(wildcard hosts/esp32/tuner-probe/main/probe_session.hpp)
tuner-csv: $(BUILD)/squatch-tuner-csv $(if $(PROBE),$(BUILD)/squatch-tuner-probe-host)

$(BUILD)/squatch-tuner-csv: hosts/render/tuner_csv.cpp $(HOST_HEADERS) $(TUNER_HEADERS) $(BUILD)/libsquatch-dsp.a | $(BUILD)
	$(CXX) $(HOST) -o $@ $< $(BUILD)/libsquatch-dsp.a

$(BUILD)/squatch-tuner-probe-host: hosts/esp32/tuner-probe/host/probe_host.cpp $(PROBE_HEADERS) $(HOST_HEADERS) \
                                   $(TUNER_HEADERS) $(BUILD)/libsquatch-dsp.a | $(BUILD)
	$(CXX) $(HOST) -o $@ $< $(BUILD)/libsquatch-dsp.a

# The display frames, the hold and the S3 probe's session and USB feed, with ASan and UBSan.
$(BUILD)/test_probe: hosts/tests/test_probe.cpp src/tuner.cpp $(PROBE_HEADERS) $(HOST_HEADERS) $(TUNER_HEADERS) | $(BUILD)
	$(CXX) $(HOST) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $< src/tuner.cpp

# The rig's host code that needs no JACK (WAV reading, resampling, options), with ASan and UBSan.
$(BUILD)/test_rig: hosts/tests/test_rig.cpp hosts/common/ir_file.hpp hosts/jack/rig_options.hpp hosts/jack/rig_stages.hpp | $(BUILD)
	$(CXX) $(HOST) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $<

# The browser demo (hosts/wasm): the tuner as WebAssembly in an AudioWorklet, a static folder
# for any HTTPS host. Needs Emscripten on PATH (source <emsdk>/emsdk_env.sh); not part of `all`.
# wasm-test scores the module in Node, then the page in headless Chrome against test tones.
DEMO := $(BUILD)/web-demo
wasm:
	python3 hosts/wasm/build.py $(DEMO)

wasm-test: wasm
	node hosts/web/hold.test.mjs
	node hosts/wasm/test/core.test.mjs $(DEMO)/tuner.wasm
	node hosts/wasm/test/browser.test.mjs --dir $(DEMO)

# The Pi bench's recording session page (hosts/pi/record-session: the standard tuner view with the
# session's controls) in headless Chrome, against a stand-in for the Pi's tuner. Needs node and Chrome.
record-session-test:
	node hosts/web/hold.test.mjs
	node hosts/pi/test/record-session.test.mjs

# Rewrites the demo screenshots in blocks/tuner/docs/screens/ (desktop and phone, light and dark).
wasm-shots: wasm
	node hosts/wasm/test/browser.test.mjs --dir $(DEMO) --only a2,a2-plus7 --shots blocks/tuner/docs/screens

# LV2 bundles for PiPedal (hosts/lv2): the macOS build and its smoke host (LV2 headers from a first
# `make lv2-linux`), and arm64 Linux in a Debian 12 container with its symbol, TTL and lv2lint checks.
lv2:
	$(MAKE) -C hosts/lv2 test

lv2-linux:
	hosts/lv2/build-linux.sh --lint

$(BUILD)/bench: bench/bench.cpp $(BUILD)/libsquatch-dsp.a $(HEADERS) | $(BUILD)
	$(CXX) $(RELEASE) -o $@ $< $(BUILD)/libsquatch-dsp.a

# Benches of their own (bench/bench_NAME.cpp): make build/bench_NAME
$(BUILD)/bench_%: bench/bench_%.cpp $(BUILD)/libsquatch-dsp.a $(HEADERS) | $(BUILD)
	$(CXX) $(RELEASE) -o $@ $< $(BUILD)/libsquatch-dsp.a

$(BUILD) $(BUILD)/obj:
	mkdir -p $@

test: $(BUILD)/tests $(BUILD)/test_queue_threads $(BUILD)/test_hosts $(BUILD)/test_host_audio $(BUILD)/test_rig $(if $(DAISY),$(BUILD)/test_daisy) $(if $(PROBE),$(BUILD)/test_probe)
	./$(BUILD)/tests
	./$(BUILD)/test_queue_threads
	./$(BUILD)/test_hosts
	./$(BUILD)/test_host_audio
	./$(BUILD)/test_rig
	$(if $(PROBE),./$(BUILD)/test_probe)
	$(if $(DAISY),./$(BUILD)/test_daisy)
	$(if $(DAISY),python3 hosts/daisy/tests/test_tools.py)
	$(MAKE) -C blocks/tuner test CXX=$(CXX)

# The tuner quality suite (blocks/tuner/bench/quality/README.md): every case through the display
# path the apps use (TunerBlock, 16- and 64-frame blocks, read 30 times a second, the page's hold),
# scored against its truth. Data stays outside the repository: name the DI corpus folder with
# TUNER_CORPUS=DIR, here or in an untracked local.mk; without it only the synthetic set runs.
-include local.mk
.PHONY: tuner-quality tuner-quality-test
QUALITY_DIR := blocks/tuner/bench/quality
QUALITY_HEADERS := $(wildcard $(QUALITY_DIR)/*.hpp) $(wildcard blocks/tuner/bench/*.hpp) hosts/common/display.hpp \
                   hosts/common/hold.hpp hosts/common/tuner_frames.hpp
TUNER_CORPUS ?=
QUALITY_OUT ?= $(BUILD)/tuner-quality
SCORECARD ?= $(QUALITY_OUT)/scorecard

$(BUILD)/tuner_quality: $(QUALITY_DIR)/quality.cpp $(QUALITY_HEADERS) $(TUNER_HEADERS) $(BUILD)/libsquatch-dsp.a | $(BUILD)
	$(CXX) $(HOST) -I. -o $@ $< $(BUILD)/libsquatch-dsp.a -lpthread

$(BUILD)/test_quality: $(QUALITY_DIR)/test_quality.cpp $(QUALITY_HEADERS) $(TUNER_HEADERS) src/tuner.cpp | $(BUILD)
	$(CXX) $(HOST) -I. -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $< src/tuner.cpp -lpthread

$(BUILD)/hold_parity: $(QUALITY_DIR)/hold_parity.cpp hosts/common/hold.hpp | $(BUILD)
	$(CXX) $(HOST) -I. -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ $<

# The suite's own tests (reference estimator, display model, truth, MIDI) and hold.hpp against
# hold.js (needs node).
tuner-quality-test: $(BUILD)/test_quality $(BUILD)/hold_parity
	./$(BUILD)/test_quality
	node $(QUALITY_DIR)/hold_parity.mjs $(BUILD)/hold_parity

tuner-quality: $(BUILD)/tuner_quality
	./$(BUILD)/tuner_quality --out $(QUALITY_OUT) $(if $(TUNER_CORPUS),--corpus $(TUNER_CORPUS))
	python3 $(QUALITY_DIR)/score.py scorecard --run $(QUALITY_OUT) --out $(SCORECARD)

# Rewrites tests/golden/*.inc from the current code. Only after a deliberate change of sound.
golden: $(BUILD)/tests
	SQUATCH_WRITE_GOLDEN=tests/golden ./$(BUILD)/tests Golden
	$(MAKE) test

bench: $(BUILD)/bench
	mkdir -p results
	./$(BUILD)/bench "$$(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)" \
	  "$$($(CXX) --version | head -1)" > $(BENCH_OUT)
	cat $(BENCH_OUT)

# Each public header compiles on its own, with the core's flags.
headers:
	@for h in $(HEADERS); do \
	  printf '#include <%s>\n' "$${h#include/}" | $(CXX) $(CORE) -x c++ -fsyntax-only - || exit 1; \
	done; echo "headers: $(words $(HEADERS)) compile standalone"

# Complexity limits of the Squatch project kit: CCN 10, 60 lines, 5 parameters per function.
lint:
	@command -v lizard >/dev/null || { echo "lizard not found: this repo installs nothing"; exit 2; }
	lizard -C 10 -L 60 -a 5 -w -x 'hosts/clap/generated/*' -x 'hosts/daisy/vendor/*' -x 'tests/reference/*_faust.h' include src tests bench hosts tools/build-contracts blocks/tuner/include blocks/tuner/bench blocks/tuner/tests

ci: lint headers lib test contracts-check release-scan-test

# Control contracts (contracts/*.params.json) -> ParamDesc tables and the plugin list. `contracts`
# rewrites the generated files; `contracts-check` fails if they are stale.
contracts:
	tools/build-contracts --formats $(BUILD)/contracts

contracts-check:
	tools/build-contracts --check

# The desktop plugins (hosts/clap): CLAP, and VST3 and AUv2 through clap-wrapper, from the pinned
# dependencies in hosts/clap/deps.lock (fetched into build/deps; nothing downloads during a build).
PLUGIN_BUILD := $(BUILD)/plugins
plugins:
	tools/fetch-plugin-deps
	cmake -S hosts/clap -B $(PLUGIN_BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build $(PLUGIN_BUILD)

# Copies the bundles into $HOME/Library/Audio/Plug-Ins/{CLAP,VST3,Components} (macOS).
plugins-install: plugins
	for pair in clap:CLAP vst3:VST3 component:Components; do \
	  mkdir -p "$$HOME/Library/Audio/Plug-Ins/$${pair#*:}"; \
	  for b in $(PLUGIN_BUILD)/bundles/*."$${pair%%:*}"; do \
	    rm -rf "$$HOME/Library/Audio/Plug-Ins/$${pair#*:}/$$(basename "$$b")"; \
	    ditto "$$b" "$$HOME/Library/Audio/Plug-Ins/$${pair#*:}/$$(basename "$$b")"; \
	  done; \
	done
	-killall -9 AudioComponentRegistrar 2>/dev/null

# The whole plugin matrix: test host (linked and over the bundles) under ASan+UBSan, TSan and
# RTSan, clap-validator, the VST3 validator, pluginval and auval. See tools/plugins-check.
plugins-check:
	tools/plugins-check

# The development tree keeps some work out of the public export (tools/private-paths). This
# proves the export leaves it out and that the exported tree builds and passes its tests.
# The release scanner's own tests, each guard with its negative control.
release-scan-test:
	tools/test-release-scan
	tools/test-release-signing

export-check:
	@if [ -x tools/check-private-export ]; then tools/check-private-export; else echo "export-check: nothing private here"; fi

clean:
	rm -rf $(BUILD)
