# PYNQ-Z2 (Zynq-7020) — board files

> The hardware half of the [EncodeOverEthernet](../../README.md) demo.
> **To run the demo, start at that README** — the board files here ship
> prebuilt and it tells you what to copy where. This page is for *rebuilding*
> them from source.

The design: PS7 GEM handles Ethernet; the OpenJLS encoder
(`openjls_axis_regs`) and an AXI DMA in Scatter/Gather mode sit in the PL on a
50 MHz fabric clock. The CPU only shuttles bytes between the socket and the
DMA — the encode is 100% hardware. [`INTERNALS.md`](INTERNALS.md) explains why
each of those choices is what it is, walks the manual bring-up the scripts
automate, and covers porting to another board.

## What ships here, prebuilt

| File | What | Rebuild when |
|---|---|---|
| `bitstreams/encode_eth_openjls_b<N>.bit.bin` | PL image, one per pixel depth `N` (8..16) | you change the block design or the core |
| `openjls.dtbo` | device-tree overlay: UIO nodes + the DMA reserved-memory carveout | you change addresses or buffer sizes |
| `u-dma-buf.ko` | the [u-dma-buf](https://github.com/ikwzm/udmabuf) kernel module | your board's kernel differs from the PYNQ image's `6.6.10-xilinx-v2024.1-g3c0eca68c652` (see [`INTERNALS.md`](INTERNALS.md)) |

Sources for the first two: `build.tcl` and `design_encode_ethernet.tcl`
recreate the Vivado project and its block design — the Tcl is the source of
truth, the project itself is never committed — and `openjls.dtso` is the
overlay source.

## Rebuilding

Requires `vivado`, `bootgen`, and `dtc` on the machine you build on. `<N>` is
the encoder pixel depth (8..16).

```sh
./build_specific_bitness.sh 8      # one depth
./build_all_bitness.sh             # every depth, 8..16 (hours: a full synth+impl each)
```

Both produce `bitstreams/encode_eth_openjls_b<N>.bit.bin` plus the shared
`openjls.dtbo`, which is exactly what the demo README copies to the board.
`build_specific_bitness.sh --no-bitstream` rebuilds only the overlay.

BITNESS is baked in at synthesis, so each pixel depth is its own bitstream —
that is why there are nine of them and why the verification sweep reloads the
PL between depths. Why per-depth, and why the 50 MHz clock and SG mode:
[`INTERNALS.md`](INTERNALS.md).

## The board-side scripts

Both are documented as steps in the [demo README](../../README.md); this is
what they are and where they live.

* `setup_bootargs.sh` (here) — **once per board**, as root, with
  `openjls.dtbo` next to it, then reboot. Puts `generic-uio` on the kernel
  command line and installs `openjls.dtbo` plus a U-Boot `uenvcmd` in
  `/boot/uEnv.txt`, so the overlay — and the DMA carveout it reserves — is
  applied **at boot**, which is the only time the kernel will honor it.
  Idempotent.
* `board_setup.sh` ([in `../../Verification/`](../../Verification/board_setup.sh))
  — **once per boot**, as root. Loads the PL, verifies the boot-time overlay
  and its carveout are live, loads `u-dma-buf`, verifies the buffers, starts
  the server. Idempotent. `board_reload.sh` next to it is the lighter
  per-depth path the verification sweep drives.

## Where to go next

* Running the demo end to end: [`../../README.md`](../../README.md)
* Rationale, manual bring-up, porting: [`INTERNALS.md`](INTERNALS.md)
* Byte-exact-vs-CharLS sweep: [`../../Verification/README.md`](../../Verification/README.md)
