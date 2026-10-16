#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The NAM adapter's parity test (tests/nam/test_parity_nam.cpp). It needs NAM Core, Eigen and
# nlohmann/json, which the core library never does, so it is not part of `make test`:
#
#   NAM_CORE=DIR NAM_DEPS=DIR tests/nam/run.sh [BUILD_DIR]
#
#   NAM_CORE  the NeuralAmpModelerCore sources (the NAM/ folder inside), the tag hosts/clap/deps.lock pins
#   NAM_DEPS  a directory holding eigen/ and nlohmann/
#
# The test reads NeuralAmpModelerCore's two example models, namcore-example-A1.nam and namcore-example-A2.nam,
# from tests/golden-parity/models/. The public repository does not carry .nam files: copy NAM Core's own
# example models there under those names (their MIT licence applies).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
nam="${NAM_CORE:?set NAM_CORE to the NAM Core sources}"
deps="${NAM_DEPS:?set NAM_DEPS to the directory holding eigen/ and nlohmann/}"
out="${1:-$root/build/nam-parity}"
mkdir -p "$out/obj"
cd "$root"
flags=(-std=c++17 -O2 -DNDEBUG -DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DEIGEN_DONT_PARALLELIZE
       -isystem "$nam" -isystem "$deps/eigen" -isystem "$deps/nlohmann")
objs=()
for f in "$nam"/NAM/*.cpp "$nam"/NAM/wavenet/*.cpp; do
  [ -e "$f" ] || continue
  o="$out/obj/$(echo "${f#"$nam"/NAM/}" | tr / _).o"
  [ "$o" -nt "$f" ] || ${CXX:-clang++} "${flags[@]}" -c -o "$o" "$f"
  objs+=("$o")
done
${CXX:-clang++} "${flags[@]}" -Wall -Wextra -Wpedantic -Wshadow -Werror -Iinclude -fsanitize=address,undefined \
  -o "$out/test_parity_nam" tests/nam/test_parity_nam.cpp hosts/common/nam_block.cpp tests/runner.cpp \
  src/biquad.cpp "${objs[@]}" -lpthread
"$out/test_parity_nam"
