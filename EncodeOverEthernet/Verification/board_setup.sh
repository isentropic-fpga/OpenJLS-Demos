#!/usr/bin/env bash
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# board_setup.sh: PYNQ-Z2 bring-up after a power cycle (as root, idempotent)
#
# Needs the boot-time overlay from setup_bootargs.sh; stops early if its DMA
# carveout is missing. For depth swaps on a set-up board, use board_reload.sh.
#
# Usage: sudo env HOME=$HOME board_setup.sh [bitness=8] [server_dir] [fw_dir] [bs_dir]
#   defaults: $HOME/Software, /lib/firmware, $HOME/bitstreams; module $HOME/u-dma-buf.ko
set -euo pipefail

b="${1:-8}"
server_dir="${2:-$HOME/Software}"
fw_dir="${3:-/lib/firmware}"
bs_dir="${4:-$HOME/bitstreams}"
module="${UDMABUF_KO:-$HOME/u-dma-buf.ko}"

fw="encode_eth_openjls_b${b}.bit.bin"
src="$bs_dir/$fw"
ov=/sys/kernel/config/device-tree/overlays/openjls   # legacy runtime overlay

[ -f "$src" ]                  || { echo "missing bitstream: $src" >&2; exit 1; }
[ -f "$module" ]              || { echo "missing module: $module" >&2; exit 1; }
[ -x "$server_dir/ojls_server" ] || { echo "missing server: $server_dir/ojls_server" >&2; exit 1; }

echo "=== 1. stop server + unload u-dma-buf (idempotent teardown) ==="
pkill -f '[o]jls_server' 2>/dev/null || true
sleep 0.3
rmmod u-dma-buf 2>/dev/null || true
# A stale configfs overlay's u-dma-buf nodes would shadow the boot-time ones.
[ -d "$ov" ] && { echo "(removing stale runtime overlay)"; rmdir "$ov" 2>/dev/null || true; }

echo "=== 2. load PL image ($fw) ==="
cp -f "$src" "$fw_dir/$fw"
echo 0 > /sys/class/fpga_manager/fpga0/flags
echo "$fw" > /sys/class/fpga_manager/fpga0/firmware
state="$(cat /sys/class/fpga_manager/fpga0/state)"
echo "fpga state: $state"
[ "$state" = operating ] || { echo "PL not operating after load" >&2; exit 1; }

echo "=== 3. verify the boot-time overlay (UIO + DMA carveout) is live ==="
dt=/proc/device-tree
missing=""
for n in reserved-memory/ojls-tx@10000000 reserved-memory/ojls-rx@18000000 \
         reserved-memory/ojls-desc@ffc0000 udmabuf-ojls-tx udmabuf-ojls-rx \
         udmabuf-ojls-desc openjls@40000000 dma@40400000; do
  [ -d "$dt/$n" ] || missing="$missing $n"
done
if [ -n "$missing" ]; then
  echo "!! boot device tree is missing:$missing" >&2
  echo "   openjls.dtbo was not applied at boot (first-time setup never ran," >&2
  echo "   or its uenvcmd fell back to a stock boot). Run the first-time setup:" >&2
  echo "   Hardware/pynq-z2/setup_bootargs.sh (as root), then reboot — it" >&2
  echo "   installs the uenvcmd that patches the DMA carveout into the FIT" >&2
  echo "   device tree. See Hardware/pynq-z2/INTERNALS.md." >&2
  exit 1
fi
echo "UIO devices: $(cat /sys/class/uio/uio*/name 2>/dev/null | sort -u | tr '\n' ' ')"

echo "=== 4. load u-dma-buf (buffers come from the exclusive carveout) ==="
insmod "$module"; sleep 0.5

echo "=== 5. verify all three buffers allocated ==="
ok=1
for name in tx rx desc; do
  d="/sys/class/u-dma-buf/udmabuf-ojls-$name"
  if [ -d "$d" ]; then
    size=$(cat "$d/size")
    if [ "$size" -ge 1048576 ]; then
      printf "  %-22s %d MiB\n" "udmabuf-ojls-$name" "$(( size / 1048576 ))"
    else
      printf "  %-22s %d KiB\n" "udmabuf-ojls-$name" "$(( size / 1024 ))"
    fi
  else
    echo "  udmabuf-ojls-$name  MISSING"; ok=0
  fi
done
if [ "$ok" != 1 ]; then
  echo "!! a buffer failed to allocate from its carveout — dmesg says:" >&2
  dmesg | grep -i -E 'reserved_mem|dma_alloc|u-dma-buf|udmabuf' | tail -8 >&2
  echo "   the carveout is exclusive, so this is not a memory-pressure race;" >&2
  echo "   check that openjls.dtso sizes match their pools (powers of two) —" >&2
  echo "   see Hardware/pynq-z2/INTERNALS.md ('Large images and the DMA carveout')." >&2
  exit 1
fi

echo "=== 6. start server (detached; confirm it latched BITNESS $b) ==="
cd "$server_dir"
setsid stdbuf -oL -eL ./ojls_server >/tmp/ojls_server.log 2>&1 </dev/null &
for _ in $(seq 1 25); do
  grep -q "BITNESS $b\b" /tmp/ojls_server.log 2>/dev/null && break
  sleep 0.2
done
if grep -q "BITNESS $b\b" /tmp/ojls_server.log 2>/dev/null; then
  echo
  echo "BOARD READY — BITNESS $b, buffers up, server listening on :19020"
else
  echo "server did not report BITNESS $b — log:" >&2
  cat /tmp/ojls_server.log >&2 || true
  exit 1
fi
