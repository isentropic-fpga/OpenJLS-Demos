#!/usr/bin/env bash
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# build_all_bitness.sh: bitstreams/encode_eth_openjls_b<N>.bit.bin per BITNESS, plus openjls.dtbo
#
# Depths build concurrently, JOBS at a time (default: MemAvailable / 5 GB, max 6).
# Each uses --jobs 1: -jobs N spawns N ~2.5 GB OOC runs per depth and OOM-kills.
#
# Usage: [JOBS=n] ./build_all_bitness.sh [N ...]   (default 8..16)
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
mkdir -p "$HERE/bitstreams" "$BUILD/logs"

DEPTHS=("$@"); [ $# -gt 0 ] || DEPTHS=(8 9 10 11 12 13 14 15 16)
if [ -z "${JOBS:-}" ]; then
  JOBS=$(( $(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo) / 5 ))
  [ "$JOBS" -gt 6 ] && JOBS=6
  [ "$JOBS" -lt 1 ] && JOBS=1
fi

build_one() {
  local b=$1 dir="$BUILD/b$b" log="$BUILD/logs/b$b.log" tmp
  rm -rf "$dir"; mkdir -p "$dir"
  # cd: Vivado's .Xil and the PS7 IP's NA/ directory would otherwise race.
  ( cd "$dir" && vivado -mode batch -source "$HERE/build.tcl" \
      -tclargs --bitstream --bitness "$b" --build-dir "$dir" --jobs 1 ) > "$log" 2>&1 || return 1
  # The Linux FPGA manager takes the raw .bin, not .bit.
  tmp="$(mktemp -d)"
  cp "$dir"/encode_ethernet.runs/impl_1/design_encode_ethernet_wrapper.bit "$tmp/system.bit"
  echo 'all: { system.bit }' > "$tmp/system.bif"
  ( cd "$tmp" && bootgen -image system.bif -arch zynq -process_bitstream bin -w ) >> "$log" 2>&1 \
    && mv "$tmp/system.bit.bin" "$HERE/bitstreams/encode_eth_openjls_b$b.bit.bin" || { rm -rf "$tmp"; return 1; }
  rm -rf "$tmp" "$dir"
}

echo "${#DEPTHS[@]} depths, $JOBS at a time"
rm -f "$BUILD"/rc-b*
for b in "${DEPTHS[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n; done
  { build_one "$b"; echo $? > "$BUILD/rc-b$b"; } &
done
wait

rc=0
for b in "${DEPTHS[@]}"; do
  if [ "$(cat "$BUILD/rc-b$b" 2>/dev/null)" = 0 ]; then
    echo "  b$b ok, $(grep -o 'WNS = [-0-9.]* ns' "$BUILD/logs/b$b.log" | tail -1)"
  else
    echo "  b$b FAILED: see build/logs/b$b.log, project kept in build/b$b"; rc=1
  fi
done
dtc -@ -O dtb -o "$HERE/openjls.dtbo" "$HERE/openjls.dtso" || { echo "  dtc FAILED"; rc=1; }
exit $rc
