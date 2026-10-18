#!/usr/bin/env bash
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# board_reload.sh: load the bitstream for one depth and restart the server (as root)
#
# The server latches BITNESS at startup, so it must restart after a reload. The
# overlay and u-dma-buf are depth-independent and stay loaded.
#
# Usage: board_reload.sh <bitness> [server_dir] [fw_dir] [bs_dir]
set -euo pipefail

b="${1:?usage: board_reload.sh <bitness> [server_dir] [fw_dir] [bs_dir]}"
server_dir="${2:-$HOME/Software}"
fw_dir="${3:-/lib/firmware}"
bs_dir="${4:-$HOME/bitstreams}"

fw="encode_eth_openjls_b${b}.bit.bin"
src="$bs_dir/$fw"
[ -f "$src" ] || { echo "missing bitstream: $src" >&2; exit 1; }
[ -x "$server_dir/ojls_server" ] || { echo "missing server: $server_dir/ojls_server" >&2; exit 1; }

pkill -f '[o]jls_server' 2>/dev/null || true
sleep 0.3

cp -f "$src" "$fw_dir/$fw"
echo 0 > /sys/class/fpga_manager/fpga0/flags
echo "$fw" > /sys/class/fpga_manager/fpga0/firmware
state="$(cat /sys/class/fpga_manager/fpga0/state)"
echo "fpga state: $state"
[ "$state" = operating ] || { echo "fpga not operating after reload" >&2; exit 1; }

# setsid: a server holding the ssh channel would hang the caller.
# stdbuf -oL: the BITNESS banner checked below must not sit in a block buffer.
cd "$server_dir"
setsid stdbuf -oL -eL ./ojls_server \
  >/tmp/ojls_server.log 2>&1 </dev/null &

for _ in $(seq 1 25); do
  grep -q "BITNESS $b\b" /tmp/ojls_server.log 2>/dev/null && break
  sleep 0.2
done
if grep -q "BITNESS $b\b" /tmp/ojls_server.log 2>/dev/null; then
  echo "server up, BITNESS $b"
else
  echo "server did not report BITNESS $b — log:" >&2
  cat /tmp/ojls_server.log >&2 || true
  exit 1
fi
