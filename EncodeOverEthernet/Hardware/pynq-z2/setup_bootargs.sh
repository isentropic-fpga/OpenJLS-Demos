#!/bin/sh
# Copyright (C) 2026 Vitor Mendes Camilo
# SPDX-License-Identifier: GPL-3.0-only
#
# This file is part of OpenJLS-Demos. Available under GPLv3 or a
# commercial license. See LICENSE.md and README.md for details.
#
# setup_bootargs.sh: one-time PYNQ-Z2 boot setup (as root, idempotent, then reboot)
#
# Edits /boot/uEnv.txt and installs openjls.dtbo in /boot:
#   1. bootargs: adds uio_pdrv_genirq.of_id=generic-uio and pins cma=128M. The
#      bootargs line replaces the whole kernel command line, so it is seeded
#      from /proc/cmdline. CMA must stay small beside the ~256 MiB carveout.
#   2. fdt_high/initrd_high = 0x0ffc0000: otherwise U-Boot relocates the DTB
#      into the ojls-rx carveout and the kernel drops that region.
#   3. uenvcmd: extracts the DTB from image.ub, applies openjls.dtbo, boots.
#      The carveout only works if the kernel sees it at early boot. Any failed
#      step falls through to the stock boot.
#
# Overrides: UENV=/boot/uEnv.txt BOOTDIR=/boot CMA=128M DTBO=<path>
set -eu

UENV=${UENV:-/boot/uEnv.txt}
BOOTDIR=${BOOTDIR:-/boot}
CMA=${CMA:-128M}
DTBO=${DTBO:-$(dirname "$0")/openjls.dtbo}

if [ "$(id -u)" -ne 0 ]; then
	echo "setup_bootargs.sh: must run as root (writes $UENV)" >&2
	exit 1
fi
if [ ! -f "$DTBO" ]; then
	echo "setup_bootargs.sh: $DTBO not found (copy openjls.dtbo next to this script, or set DTBO=)" >&2
	exit 1
fi

# 1. bootargs
args=$(sed -n 's/^bootargs=//p' "$UENV" 2>/dev/null || true)
[ -n "$args" ] || args=$(cat /proc/cmdline)

case "$args" in *uio_pdrv_genirq.of_id=*) ;; *) args="$args uio_pdrv_genirq.of_id=generic-uio" ;; esac
case "$args" in
	*cma=*) args=$(printf '%s' "$args" | sed -E "s/cma=[0-9]+[KkMmGg]?/cma=$CMA/") ;;
	*)      args="$args cma=$CMA" ;;
esac

cp -f "$DTBO" "$BOOTDIR/openjls.dtbo"

# 2. below the lowest carveout (ojls-desc@ffc0000) and above the kernel image
fdt_high='fdt_high=0x0ffc0000'
initrd_high='initrd_high=0x0ffc0000'

# 3. single-quoted: ${devtype} etc. are expanded by U-Boot.
# DTB at 0x08000000, dtbo at 0x08100000: clear of the kernel and the FIT.
uenvcmd='uenvcmd=echo openjls: applying openjls.dtbo to the FIT device tree && fatload ${devtype} ${devnum}:${distro_bootpart} 0x10000000 image.ub && fatload ${devtype} ${devnum}:${distro_bootpart} 0x08100000 openjls.dtbo && imxtract 0x10000000 fdt-0 0x08000000 && fdt addr 0x08000000 && fdt resize 0x10000 && fdt apply 0x08100000 && bootm 0x10000000:kernel-0 - 0x08000000'

tmp="$UENV.tmp"
{
	[ -f "$UENV" ] && grep -v -e '^bootargs=' -e '^uenvcmd=' -e '^fdt_high=' -e '^initrd_high=' "$UENV" || true
	echo "bootargs=$args"
	echo "$fdt_high"
	echo "$initrd_high"
	echo "$uenvcmd"
} > "$tmp"
mv -f "$tmp" "$UENV"

echo "setup_bootargs.sh: wrote $UENV"
echo "  bootargs=$args"
echo "  $fdt_high $initrd_high  (keeps U-Boot's relocated DTB out of the carveouts)"
echo "  uenvcmd=(boot-time overlay apply, falls back to stock boot on failure)"
echo "setup_bootargs.sh: installed $BOOTDIR/openjls.dtbo"
echo "setup_bootargs.sh: reboot for the new command line + carveout to take effect."
