# SPDX-License-Identifier: MIT
# Seed3 firmware: libDaisy's Make build (not its CMake one: libDaisy #712/#714), our own flags.
#
#   hosts/daisy/fetch-libdaisy     once: libDaisy at the pinned commit, into vendor/ (gitignored)
#   make -C hosts/daisy            libDaisy, every variant below, sizes and the heap check
#   make -C hosts/daisy sizes      just the table (build/daisy/SIZES.md)
#
# The compiler is arm-none-eabi-gcc from PATH, else ARM_GCC_DIR (default: .local/bin in your home directory). The
# flags follow libDaisy's core/Makefile at the pin, except: C++17, -O3 like the squatch-dsp core,
# warnings as errors on our code (libDaisy's headers are -isystem), and no -fshort-enums (the
# library itself is built without it).
#
# A variant is PROJECT-OPTION-OPTION...: bNN is the block size, hot and burnNN are seed3-chain's
# libDaisy #656 checks (seed3-chain/main.cpp), laNN is the limiter's look-ahead in samples (default 72), gen replaces the
# input by a built-in hot test signal (seed3-chain), lim and chain put the limiter, or gate + drive + limiter, in the
# round trip of seed3-latency. Output: build/daisy/bin/seed3-<variant>.bin.
VARIANTS := passthrough-b32 passthrough-b48 passthrough-b64 passthrough-crash-b32 \
            chain-b32 chain-b48 chain-b64 chain-hot-b32 chain-hot-b48 chain-hot-burn60-b32 \
            latency-b32 latency-b48 latency-b64 latency-b16 \
            chain-b16 chain-la16-b32 chain-la32-b32 chain-la16-b16 \
            chain-limonly-gen-b32 chain-limonly-gen-la32-b32 chain-limonly-gen-la16-b32 chain-limonly-t12-gen-b32 chain-limonly-t12-gen-la32-b32 chain-limonly-t12-gen-la16-b32 \
            chain-gen-b32 chain-gen-la32-b32 chain-gen-la16-b32 chain-hot-gen-b32 chain-hot-gen-la32-b32 chain-hot-gen-la16-b32 \
            notegen-b32 nam-b32 nam-parity-b32 nam-chain-b32 latency-lim-b32 latency-lim-la16-b32 latency-lim-la32-b32 latency-chain-b32 latency-chain-la16-b32 latency-chain-la32-b32 \
            latency-lim-la16-b16 latency-chain-la16-b16 latency-chain-b16 latency-chain-la32-b16 latency-lim-b16

ROOT := ../..
LIBDAISY := vendor/libDaisy
OUT := $(ROOT)/build/daisy
LIBDAISY_REV := $(shell sed -n 's/^LIBDAISY_COMMIT=//p' fetch-libdaisy | cut -c1-8)

ARM_GCC_DIR ?= $(HOME)/.local/bin
TOOL = $(if $(shell command -v arm-none-eabi-$(1) 2>/dev/null),arm-none-eabi-$(1),$(ARM_GCC_DIR)/arm-none-eabi-$(1))
CC := $(call TOOL,gcc)
CXX := $(call TOOL,g++)
OBJCOPY := $(call TOOL,objcopy)
OBJDUMP := $(call TOOL,objdump)
NM := $(call TOOL,nm)
GCC_DIR := $(dir $(shell command -v $(CC) 2>/dev/null))

MCU := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
DEFS := -DUSE_HAL_DRIVER -DSTM32H750xx -DSTM32H750IB -DHSE_VALUE=16000000 -DCORE_CM7 -DARM_MATH_CM7 \
        -DUSE_FULL_LL_DRIVER -DFILEIO_ENABLE_FATFS_READER -DSQUATCH_LIBDAISY='"$(LIBDAISY_REV)"'
LIBDAISY_INC := $(addprefix -isystem $(LIBDAISY)/,src src/sys src/usbd src/usbh Drivers/CMSIS_5/CMSIS/Core/Include \
        Drivers/CMSIS-Device/ST/STM32H7xx/Include Drivers/STM32H7xx_HAL_Driver/Inc \
        Middlewares/ST/STM32_USB_Device_Library/Core/Inc Middlewares/ST/STM32_USB_Host_Library/Core/Inc \
        Middlewares/ST/STM32_USB_Host_Library/Class/MSC/Inc Middlewares/ST/STM32_USB_Host_Library/Class/MIDI/Inc \
        Middlewares/Third_Party/FatFs/src) -isystem $(LIBDAISY)
OPT ?= -O3
COMMON := $(MCU) $(DEFS) $(OPT) -g -ffunction-sections -fdata-sections -Wall -Wextra -Werror
# libDaisy's startup file is its code, built with its own warning set, not ours.
STARTUP_CFLAGS := $(MCU) $(DEFS) -DHardFault_Handler=SquatchHardFault_Handler -O3 -g -ffunction-sections -fdata-sections -std=gnu11 $(LIBDAISY_INC) \
                  -Wall -Wno-attributes -Wno-missing-attributes
CXXFLAGS := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -fno-unwind-tables -fno-threadsafe-statics \
            -Wshadow -I$(ROOT)/include $(LIBDAISY_INC)
LDSCRIPT := $(LIBDAISY)/core/STM32H750IB_flash.lds
LDFLAGS := $(MCU) --specs=nano.specs --specs=nosys.specs -T$(LDSCRIPT) -L$(LIBDAISY)/build \
           -Wl,--gc-sections -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

# gate + drive + limiter in the latency image (the round trip with the full chain in the path) is 4.6 KB over the
# 128 KB flash at -O3: those variants are built -Os. Their CPU figures are not comparable with the -O3 chain images.
OPT_latency-chain-b32 := -Os
OPT_latency-chain-la16-b32 := -Os
OPT_latency-chain-la32-b32 := -Os
OPT_latency-chain-b16 := -Os
OPT_latency-chain-la32-b16 := -Os
OPT_latency-chain-la16-b16 := -Os

# The A2-Lite images do not fit the 128 KB flash with libDaisy built -O2 (nam-b32 is 8 KB over, nam-chain 15 KB): they link a
# libDaisy built -Os in vendor/libDaisy/build-os (about 20 KB smaller), at some cost to its audio conversion.
LDB_nam-b32 := build-os
LDB_nam-parity-b32 := build-os
LDB_nam-chain-b32 := build-os
# The kernel options the shipping A2-Lite images use: the best exact combination measured by tools/nam_bench.py (variant
# "fused+dtcm+slideh" (hosts/daisy/nam/a2lite.hpp says what each does). The images' bit-for-bit output equals the plain kernel's.
NAMBEST := -DNAMOPT_FUSED=1 -DNAMOPT_DTCM=1 -DNAMOPT_SLIDEH=1
OPT_nam-b32 := $(NAMBEST)
OPT_nam-parity-b32 := $(NAMBEST)
OPT_nam-chain-b32 := $(NAMBEST)
ldb = $(or $(LDB_$(1)),$(if $(filter nam,$(call project,$(1))),build-os,build))

# What each project compiles besides its main.cpp.
EXTRA_passthrough :=
EXTRA_latency := $(ROOT)/src/gate.cpp $(ROOT)/src/limiter.cpp
EXTRA_nam := $(ROOT)/src/gate.cpp $(ROOT)/src/limiter.cpp
EXTRA_notegen :=
EXTRA_chain := $(ROOT)/src/gate.cpp $(ROOT)/src/limiter.cpp
HEADERS := $(wildcard nam/*.hpp common/*.hpp faust/*.hpp $(ROOT)/include/squatch/dsp/*.hpp $(ROOT)/include/squatch/dsp/blocks/*.hpp)

opt_def = $(if $(filter b8 b16 b32 b48 b64 b96 b128 b256,$(1)),-DSQUATCH_BLOCK=$(1:b%=%))$(if \
          $(filter hot,$(1)),-DSQUATCH_HOT=1)$(if $(filter burn%,$(1)),-DSQUATCH_BURN_PCT=$(1:burn%=%))$(if \
          $(filter la%,$(1)),-DSQUATCH_LOOKAHEAD=$(1:la%=%))$(if $(filter gen,$(1)),-DSQUATCH_GEN=1)$(if $(filter crash,$(1)),-DSQUATCH_CRASH=1)$(if $(filter limonly,$(1)),-DSQUATCH_LIMONLY=1)$(if $(filter t12,$(1)),-DSQUATCH_TRIM_DB=12)$(if \
          $(filter lim,$(1)),-DSQUATCH_PATH=1)$(if $(filter chain,$(1)),-DSQUATCH_PATH=2 -DSQUATCH_NAM_CHAIN=1)
# nam-bench-... (tools/nam_bench.py): bench runs the model synchronously with no audio, parity adds the recording, and
# xNAME is a kernel option (-DNAMOPT_NAME=1, hosts/daisy/nam/a2lite.hpp). xo2/xo3/xos set the optimisation level of the
# image's own code, xfast adds -ffast-math, xlto adds -flto (compile and link).
opt_def += $(if $(filter bench,$(1)),-DSQUATCH_NAM_BENCH=1) $(if $(filter parity,$(1)),-DSQUATCH_NAM_PARITY=1) $(if \
           $(filter x%,$(1)),-DNAMOPT_$(shell echo '$(1:x%=%)' | tr a-z A-Z)=1) $(if $(filter xo2,$(1)),-O2) $(if \
           $(filter xo3,$(1)),-O3) $(if $(filter xos,$(1)),-Os) $(if $(filter xfast,$(1)),-ffast-math) $(if \
           $(filter xlto,$(1)),-flto)
project = $(firstword $(subst -, ,$(1)))
has = $(filter $(2),$(subst -, ,$(1)))

.PHONY: all libdaisy sizes heapcheck heapcheck-control clean
all: sizes heapcheck-control heapcheck

$(LIBDAISY)/.squatch-pin:
	@echo "libDaisy is missing: run hosts/daisy/fetch-libdaisy first"; exit 2

libdaisy: $(LIBDAISY)/build/libdaisy.a $(LIBDAISY)/build-os/libdaisy.a
$(LIBDAISY)/build/libdaisy.a: $(LIBDAISY)/.squatch-pin
	./fetch-libdaisy --verify
	$(MAKE) -C $(LIBDAISY) GCC_PATH=$(GCC_DIR:%/=%)

$(LIBDAISY)/build-os/libdaisy.a: $(LIBDAISY)/.squatch-pin
	./fetch-libdaisy --verify
	$(MAKE) -C $(LIBDAISY) GCC_PATH=$(GCC_DIR:%/=%) OPT=-Os BUILD_DIR=build-os

# One variant: $(1) is its name.
define VARIANT
$(OUT)/$(1)/main.o: seed3-$(call project,$(1))/main.cpp $(HEADERS) | $(LIBDAISY)/$(call ldb,$(1))/libdaisy.a
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) -DSQUATCH_FIRMWARE='"seed3-$(1)"' $(OPT_$(1)) $(foreach o,$(wordlist 2,99,$(subst -, ,$(1))),$(call opt_def,$(o))) -c -o $$@ $$<
$(OUT)/$(1)/%.o: $(ROOT)/src/%.cpp $(HEADERS)
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) $(OPT_$(1)) -c -o $$@ $$<
$(OUT)/$(1)/startup.o: $(LIBDAISY)/core/startup_stm32h750xx.c | $(LIBDAISY)/$(call ldb,$(1))/libdaisy.a
	@mkdir -p $$(@D)
	$(CC) $(STARTUP_CFLAGS) -c -o $$@ $$<
$(OUT)/bin/seed3-$(1).elf: $(OUT)/$(1)/main.o $(OUT)/$(1)/startup.o \
                           $(patsubst $(ROOT)/src/%.cpp,$(OUT)/$(1)/%.o,$(EXTRA_$(call project,$(1)))) $(LIBDAISY)/$(call ldb,$(1))/libdaisy.a
	@mkdir -p $$(@D)
	$(CXX) -o $$@ $$(filter %.o,$$^) -L$(LIBDAISY)/$(call ldb,$(1)) $(LDFLAGS) $(if $(filter xlto,$(subst -, ,$(1))),$(OPT_$(1)) -flto) -Wl,-Map=$(OUT)/$(1)/seed3-$(1).map -ldaisy -lc -lm -lnosys > $(OUT)/$(1)/memory.txt
	@cat $(OUT)/$(1)/memory.txt
endef
VARIANTS += $(EXTRA_VARIANTS)
$(foreach v,$(VARIANTS),$(eval $(call VARIANT,$(v))))

ELFS := $(VARIANTS:%=$(OUT)/bin/seed3-%.elf)
BINS := $(ELFS:.elf=.bin)
$(OUT)/bin/%.bin: $(OUT)/bin/%.elf
	$(OBJCOPY) -O binary -S $< $@

sizes: $(BINS)
	python3 tools/size-report --out $(OUT)/SIZES.md $(ELFS)

heapcheck: $(ELFS)
	python3 tools/heapcheck --objdump $(OBJDUMP) --nm $(NM) $(ELFS)

# The check must fail an image whose callback allocates behind a virtual call, or it is blind.
$(OUT)/control/heapcheck-control.elf: tools/heapcheck_control.cpp $(HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(MCU) -O2 -std=gnu++17 -fno-exceptions -fno-rtti -I$(ROOT)/include --specs=nano.specs --specs=nosys.specs \
	  -Wl,--no-warn-rwx-segments -o $@ $< 2>/dev/null

heapcheck-control: $(OUT)/control/heapcheck-control.elf
	@if python3 tools/heapcheck --objdump $(OBJDUMP) --nm $(NM) $<; then \
	  echo "heapcheck-control: FAIL - heapcheck passed an image that allocates in its callback"; exit 1; \
	else echo "heapcheck-control: ok - heapcheck caught the allocating callback"; fi

clean:
	rm -rf $(OUT)
