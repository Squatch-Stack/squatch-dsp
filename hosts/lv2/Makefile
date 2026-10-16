# SPDX-License-Identifier: MIT
# The Squatch LV2 bundles and a smoke host that loads them. Plugins are found, not listed: each
# <name>_lv2.cpp is one plugin, built into the bundle squatch-<name with - for _>.lv2 next to its
# committed TTL, and each test_<name>_lv2.cpp adds that plugin's checks to the smoke host.
#
#   make -C hosts/lv2 LV2_INCLUDE=DIR        bundles into build/lv2-<os>-<arch>/ (DIR holds lv2/core/lv2.h),
#                                            objects and the smoke host into build/lv2-work-<os>-<arch>/
#   make -C hosts/lv2 test LV2_INCLUDE=DIR   ... and run the smoke host over them
#   hosts/lv2/build-linux.sh                 the same for arm64 Linux, in a Debian 12 container
#
# The TTL is generated from contracts/*.params.json by gen-ttl.mjs and committed; `make check`
# (Node) fails when it is stale. Linux builds link libstdc++ and libgcc statically and export one
# symbol, lv2_descriptor, so a bundle needs nothing on the target but glibc (2.36 or newer).
ROOT := ../..
UNAME := $(shell uname -s)
ARCH := $(subst aarch64,arm64,$(shell uname -m))
ifeq ($(UNAME),Darwin)
# build-linux.sh copies Debian's lv2-dev headers here, for the macOS build
LV2_INCLUDE ?= $(ROOT)/build/lv2-headers
OS := macos
EXT := dylib
SHARED := -dynamiclib -Wl,-exported_symbol,_lv2_descriptor
DL :=
else
OS := linux
EXT := so
SHARED := -shared -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-z,defs -Wl,--gc-sections -Wl,--as-needed
DL := -ldl
LV2_INCLUDE ?= /usr/include
endif
OUT ?= $(ROOT)/build/lv2-$(OS)-$(ARCH)
WORK ?= $(ROOT)/build/lv2-work-$(OS)-$(ARCH)

WARN := -Wall -Wextra -Wpedantic -Wshadow -Werror
# -isystem /usr/include would break libstdc++'s #include_next, and is on the path anyway.
INC := -I$(ROOT)/include -I$(ROOT)/blocks/tuner/include $(if $(filter /usr/include,$(LV2_INCLUDE)),,-isystem $(LV2_INCLUDE))
CXXFLAGS := -std=c++17 -O3 -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -ffunction-sections -fdata-sections \
            -fno-exceptions -fno-rtti $(WARN) $(INC)
HEADERS := $(shell find $(ROOT)/include $(ROOT)/blocks/tuner/include -name '*.hpp') $(wildcard *.hpp) ../common/display.hpp
LIB_SRC := $(wildcard $(ROOT)/src/*.cpp)
LIB_OBJ := $(LIB_SRC:$(ROOT)/src/%.cpp=$(WORK)/obj/%.o)
PLUGINS := $(patsubst %_lv2.cpp,%,$(filter-out test_%,$(wildcard *_lv2.cpp)))
bundle = squatch-$(subst _,-,$(1))
BUNDLES := $(foreach p,$(PLUGINS),$(call bundle,$(p)))
TEST_SRC := test_lv2.cpp $(wildcard test_*_lv2.cpp)

.PHONY: all test check clean
all: $(foreach b,$(BUNDLES),$(OUT)/$(b).lv2/$(b).$(EXT) $(OUT)/$(b).lv2/manifest.ttl $(OUT)/$(b).lv2/$(b).ttl)

# The core, position-independent, as an archive: each plugin pulls in only the objects it uses.
$(WORK)/obj/%.o: $(ROOT)/src/%.cpp $(HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(WORK)/libsquatch-dsp-pic.a: $(LIB_OBJ)
	ar rcs $@ $^

define plugin_rule
$(OUT)/$(call bundle,$(1)).lv2/$(call bundle,$(1)).$(EXT): $(1)_lv2.cpp $(WORK)/libsquatch-dsp-pic.a $(HEADERS)
	@mkdir -p $$(@D)
	$$(CXX) $$(CXXFLAGS) $$(SHARED) -o $$@ $$< $(WORK)/libsquatch-dsp-pic.a -lm
endef
$(foreach p,$(PLUGINS),$(eval $(call plugin_rule,$(p))))

# The committed TTL, with the binary's name for this platform.
$(OUT)/%.lv2/manifest.ttl: %.lv2/manifest.ttl
	@mkdir -p $(@D)
	sed 's/\.so>/.$(EXT)>/' $< > $@

define ttl_rule
$(OUT)/$(1).lv2/$(1).ttl: $(1).lv2/$(1).ttl
	@mkdir -p $$(@D)
	cp $$< $$@
endef
$(foreach b,$(BUNDLES),$(eval $(call ttl_rule,$(b))))

# The smoke host: plain C++ with the host's usual freedoms, sanitizers on where they exist.
TEST_SAN_Darwin := -fsanitize=address,undefined
TEST_SAN_Linux := -fsanitize=undefined
TEST_SAN := $(TEST_SAN_$(UNAME))
$(WORK)/test_lv2: $(TEST_SRC) $(WORK)/libsquatch-dsp-pic.a $(HEADERS)
	$(CXX) -std=c++17 -O1 -g $(WARN) $(INC) $(TEST_SAN) -o $@ $(TEST_SRC) $(WORK)/libsquatch-dsp-pic.a $(DL) -lm

test: all $(WORK)/test_lv2
	$(WORK)/test_lv2 $(OUT) $(EXT)

check:
	node gen-ttl.mjs --check

clean:
	rm -rf $(OUT) $(WORK)
