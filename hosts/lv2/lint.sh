#!/bin/sh
# SPDX-License-Identifier: MIT
# lv2lint over the Linux bundles. lv2lint is not packaged by Debian, so it is built from source
# (github.com/swesterfeld/lv2lint, Artistic-2.0: a tool we run, never link) in a throwaway image.
#   sh hosts/lv2/lint.sh build/lv2-linux-aarch64
set -eu
dir=$(cd "$1" && pwd)
image=squatch-lv2lint:trixie
if ! docker image inspect "$image" >/dev/null 2>&1; then
  docker build -t "$image" - <<'IMG'
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends git ca-certificates meson ninja-build pkg-config \
    gcc g++ liblilv-dev lv2-dev libelf-dev libcurl4-openssl-dev && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/swesterfeld/lv2lint /opt/lv2lint && cd /opt/lv2lint \
 && meson setup build -Delf-tests=enabled -Donline-tests=disabled && ninja -C build && ninja -C build install
IMG
fi
status=0
for manifest in "$dir"/*.lv2/manifest.ttl; do
  bundle=$(basename "$(dirname "$manifest")" .lv2)
  uri=$(sed -n 's/^<\(.*\)>$/\1/p' "$manifest" | head -1)
  echo "== lv2lint $uri"
  docker run --rm --platform linux/arm64 -v "$dir:/lv2:ro" "$image" \
    lv2lint -I "/lv2/$bundle.lv2/" -M pack -S note "$uri" || status=1
done
exit $status
