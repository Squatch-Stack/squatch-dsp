#!/bin/sh
# SPDX-License-Identifier: MIT
# Inside the build container: what each Linux bundle needs from the target and whether its TTL is
# sound. Fails on a libstdc++/libgcc dependency, a glibc symbol newer than 2.36, an exported
# symbol other than lv2_descriptor, invalid Turtle, or a plugin lilv cannot load.
#   sh hosts/lv2/inspect.sh build/lv2-linux-aarch64
set -eu
dir=$1
fail=0
for so in "$dir"/*.lv2/*.so; do
  name=$(basename "$so")
  needed=$(readelf -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
  glibc=$(objdump -T "$so" | grep -o 'GLIBC_[0-9.]*' | sort -t. -k2,2n -k3,3n -u | tail -1)
  exported=$(objdump -T "$so" | awk '$2 == "g" && $4 != "*UND*" {print $NF}' | tr '\n' ' ')
  echo "$name: $(wc -c < "$so") bytes; needs: $needed; newest glibc symbol: $glibc; exports: $exported"
  case "$needed" in *libstdc++*|*libgcc_s*) echo "  FAIL: links the C++ runtime dynamically"; fail=1 ;; esac
  newest=$(printf '%s\nGLIBC_2.36\n' "$glibc" | sort -t. -k2,2n -k3,3n | tail -1)
  [ "$newest" = "GLIBC_2.36" ] || { echo "  FAIL: needs $glibc, newer than Bookworm's 2.36"; fail=1; }
  [ "$(echo "$exported" | xargs)" = "lv2_descriptor" ] || { echo "  FAIL: exports more than lv2_descriptor"; fail=1; }
done
for ttl in "$dir"/*.lv2/*.ttl; do
  serdi -q -i turtle -o ntriples "$ttl" > /dev/null || { echo "FAIL: $ttl is not valid Turtle"; fail=1; }
done
spec=$(find /usr/lib/lv2 -name '*.ttl' | grep -E '/(schemas|core|units|port-props|state|urid)\.lv2/' | grep -v manifest.ttl)
# shellcheck disable=SC2086  # the spec file list is meant to split
if sord_validate $spec "$dir"/*.lv2/*.ttl; then echo "sord_validate: clean"; else echo "FAIL: sord_validate"; fail=1; fi
info=$(mktemp)
for manifest in "$dir"/*.lv2/manifest.ttl; do
  uri=$(sed -n 's/^<\(.*\)>$/\1/p' "$manifest" | head -1)
  LV2_PATH=$(cd "$dir" && pwd):/usr/lib/lv2 lv2info "$uri" > "$info" || { echo "FAIL: lilv cannot load $uri"; fail=1; }
  echo "lv2info $uri: $(grep -c 'Port [0-9]*:' "$info") ports, class $(sed -n 's/^[[:space:]]*Class:[[:space:]]*//p' "$info" | head -1)"
done
rm -f "$info"
exit $fail
