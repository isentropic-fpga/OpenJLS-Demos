#!/usr/bin/env bash
# Build one PYNQ-Z2 bitstream per encoder precision (BITNESS 8..16) and stage
# each as bitstreams/encode_eth_openjls_b<N>.bit.bin — the committed artifacts
# the HIL sweep (../../Verification/run_hil_sweep.py) reloads on the board. Then
# compile the shared, precision-independent overlay (openjls.dtbo) once, so this
# script is self-contained: same outputs as build_specific_bitness.sh, every
# depth. For a single precision, use build_specific_bitness.sh instead.
#
# Each depth is a full synth+impl run (this design only closes timing with the
# Congestion_SpreadLogic_high strategy build.tcl sets), so the whole sweep is
# hours. The depths are independent, so they run concurrently: each gets its own
# project directory (build/b<N>, via build.tcl --build-dir) because two Vivado
# runs sharing one project would clobber each other's runs/impl_1.
#
# Each depth is given --jobs 1, so it is one ~3 GB vivado process at a time and
# concurrency is the only multiplier. Do not raise that: -jobs N lets Vivado
# launch the block design's ~8 out-of-context IP synth runs as N separate
# processes, so depths x jobs is the real process count. Five depths at -jobs 4
# is ~20 vivado processes and OOM-kills a 30 GB machine (it took the session's
# systemd and dbus with it), which is why the two knobs are split.
#
# Concurrency defaults to MemAvailable / 5 GB, capped at MAX_JOBS and at the
# number of depths. Override with JOBS=<n>. Memory is the binding constraint,
# not cores: a swapping router is slower than a serial sweep.
#
# A depth that fails to fit or close timing is reported and skipped, not fatal —
# you still get every depth that built. Higher depths use more PL and may be the
# ones that don't close on the -1 speed grade. Project directories are deleted
# as they succeed (~1 GB each); a failed depth keeps its whole project for
# inspection, and every depth leaves build/logs/b<N>.log either way.
#
# Usage:
#   ./build_all_bitness.sh                # all depths 8..16
#   ./build_all_bitness.sh 8 12 16        # only these depths
#   JOBS=2 ./build_all_bitness.sh         # cap concurrency by hand
#
# Requires vivado + bootgen + dtc on PATH (host shims into the vivado_box
# distrobox provide vivado + bootgen).
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$HERE/bitstreams"
BUILD_DIR="$HERE/build"
LOG_DIR="$BUILD_DIR/logs"

mkdir -p "$OUT_DIR" "$LOG_DIR"

DEPTHS=("$@")
[ ${#DEPTHS[@]} -eq 0 ] && DEPTHS=(8 9 10 11 12 13 14 15 16)

# Ceiling on concurrent depths, whatever the machine reports free. Empirical:
# the sweep is memory-bound long before it is core-bound.
MAX_JOBS=6

# Budget per depth. Two vivado processes are alive at once even at --jobs 1:
# the parent holds the elaborated design (~2.5 GB) while wait_on_run blocks,
# and the run itself is another ~2.5 GB through place and route. Sizing this at
# the parent alone is what made the first attempt OOM.
#
# Measured on the v1.2 sweep (9 depths, pool of 4): ~20 GB committed and ~12 GB
# actually in use, peaking at 14.6 GiB of summed RSS -- and summed RSS
# overcounts, since the forked children share pages with the parent. 5 GB/depth
# is the honest number. On a ~27 GB-free machine that yields a pool of 5, so
# nine depths run 5 then 4 instead of 4/4/1 with a lone straggler.
GB_PER_DEPTH=5

# Never more runs than depths, never more than MAX_JOBS, never more than fits.
if [ -z "${JOBS:-}" ]; then
  by_mem=$(( $(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo) / GB_PER_DEPTH ))
  JOBS=${#DEPTHS[@]}
  [ "$by_mem" -lt "$JOBS" ] && JOBS=$by_mem
  [ "$MAX_JOBS" -lt "$JOBS" ] && JOBS=$MAX_JOBS
  [ "$JOBS" -lt 1 ] && JOBS=1
fi

# One depth, end to end: synth+impl, then bootgen to the .bit.bin the FPGA
# manager accepts. Runs in the background, so the outcome goes to a status file
# rather than a variable the parent shell could never see.
build_one() {
  local b="$1"
  local dir="$BUILD_DIR/b$b"
  local status="$BUILD_DIR/status-b$b"
  local log="$LOG_DIR/b$b.log"
  local wrapper_bit="$dir/encode_ethernet.runs/impl_1/design_encode_ethernet_wrapper.bit"

  rm -rf "$dir"; mkdir -p "$dir"
  # cd so Vivado's own log/journal/.Xil — and the PS7 IP's literal NA/ directory
  # — land in this depth's directory instead of racing in the shared one.
  if ! ( cd "$dir" && vivado -mode batch -source "$HERE/build.tcl" \
           -tclargs --bitstream --bitness "$b" --build-dir "$dir" --jobs 1 ) > "$log" 2>&1; then
    echo "fail: vivado exited nonzero (project kept at build/b$b/)" > "$status"; return
  fi
  # build.tcl gates on post-route WNS, so a .bit here is already timing-clean.
  if [ ! -f "$wrapper_bit" ]; then
    echo "fail: no bitstream produced, timing/fit (project kept at build/b$b/)" > "$status"; return
  fi

  # bootgen: emit the raw .bin (no boot header) the Linux FPGA manager accepts.
  local tmp; tmp="$(mktemp -d)"
  cp "$wrapper_bit" "$tmp/system.bit"
  echo 'all: { system.bit }' > "$tmp/system.bif"
  if ! ( cd "$tmp" && bootgen -image system.bif -arch zynq -process_bitstream bin -w ) \
         >> "$log" 2>&1; then
    echo "fail: bootgen (project kept at build/b$b/)" > "$status"; rm -rf "$tmp"; return
  fi
  mv "$tmp/system.bit.bin" "$OUT_DIR/encode_eth_openjls_b${b}.bit.bin"
  rm -rf "$tmp"

  # Artifact is staged and the log is outside the project, so drop the project.
  rm -rf "$dir"
  echo "ok: $(grep -o 'post-route WNS = [-0-9.]* ns' "$log" | tail -1)" > "$status"
}

echo "==== ${#DEPTHS[@]} depths (${DEPTHS[*]}), $JOBS at a time ===="
rm -f "$BUILD_DIR"/status-b*
started=$(date +%s)
running=0
for b in "${DEPTHS[@]}"; do
  while [ "$running" -ge "$JOBS" ]; do wait -n; running=$((running - 1)); done
  echo "-- BITNESS $b: started ($(date +%H:%M:%S))"
  build_one "$b" &
  running=$((running + 1))
done
wait

# The overlay is precision-independent (UIO nodes + DMA buffers), so build it
# once here rather than per depth. Independent of the sweep above, so it runs
# even if some depths failed to close timing.
echo "================ overlay (dtc) ================"
if dtc -@ -O dtb -o "$HERE/openjls.dtbo" "$HERE/openjls.dtso"; then
  echo "== overlay -> openjls.dtbo"; overlay_ok=1
else
  echo "!! overlay: dtc failed"; overlay_ok=0
fi

echo
echo "==================== summary ===================="
printf 'elapsed: %s\n\n' "$(date -u -d "@$(( $(date +%s) - started ))" +%H:%M:%S)"
ok=(); fail=()
for b in "${DEPTHS[@]}"; do
  line="$(cat "$BUILD_DIR/status-b$b" 2>/dev/null || echo 'fail: no status (killed?)')"
  case "$line" in
    ok*) ok+=("$b"); printf '   b%-2s %s\n' "$b" "${line#ok: }" ;;
    *)   fail+=("$b"); printf '!! b%-2s %s\n' "$b" "${line#fail: }" ;;
  esac
done
echo
echo "built:   ${ok[*]:-(none)} -> bitstreams/encode_eth_openjls_b<N>.bit.bin"
echo "failed:  ${fail[*]:-(none)}"
echo "overlay: $([ "${overlay_ok:-0}" = 1 ] && echo openjls.dtbo || echo FAILED)"
echo "logs:    build/logs/b<N>.log"
[ ${#fail[@]} -eq 0 ] && [ "${overlay_ok:-0}" = 1 ]
