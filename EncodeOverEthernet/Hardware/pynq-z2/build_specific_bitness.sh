#!/usr/bin/env bash
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# build_specific_bitness.sh: bitstreams/encode_eth_openjls_b<N>.bit.bin for one BITNESS, plus openjls.dtbo
#
# Usage: ./build_specific_bitness.sh [--no-bitstream] [N]   (default 8)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
b=8 bit=1
for arg in "$@"; do
  case "$arg" in
    -n|--no-bitstream) bit=0 ;;
    8|9|1[0-6]) b=$arg ;;
    *) echo "usage: $0 [--no-bitstream] [8..16]" >&2; exit 2 ;;
  esac
done

if [ $bit = 1 ]; then
  wrapper="$HERE/build/encode_ethernet.runs/impl_1/design_encode_ethernet_wrapper.bit"
  rm -f "$wrapper"
  # build.tcl exits nonzero on negative WNS, even though the .bit is written.
  vivado -mode batch -source "$HERE/build.tcl" -tclargs --bitstream --bitness "$b"
  # The Linux FPGA manager takes the raw .bin, not .bit.
  tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
  cp "$wrapper" "$tmp/system.bit"
  echo 'all: { system.bit }' > "$tmp/system.bif"
  ( cd "$tmp" && bootgen -image system.bif -arch zynq -process_bitstream bin -w )
  mkdir -p "$HERE/bitstreams"
  mv "$tmp/system.bit.bin" "$HERE/bitstreams/encode_eth_openjls_b$b.bit.bin"
fi
dtc -@ -O dtb -o "$HERE/openjls.dtbo" "$HERE/openjls.dtso"
