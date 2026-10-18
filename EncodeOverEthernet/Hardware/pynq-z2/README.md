# PYNQ-Z2 (Zynq-7020) — board files

> To run the demo, start at the [EncodeOverEthernet README](../../README.md);
> these files ship prebuilt. This page covers *rebuilding* them.

PS7 GEM handles Ethernet; the OpenJLS encoder (`openjls_axis_regs`) and an AXI
DMA in Scatter/Gather mode sit in the PL on a 70 MHz fabric clock.
[`INTERNALS.md`](INTERNALS.md) has the rationale, the manual bring-up and
porting notes.

## What ships here, prebuilt

| File | What | Rebuild when |
|---|---|---|
| `bitstreams/encode_eth_openjls_b<N>.bit.bin` | PL image, one per pixel depth `N` (8..16) | you change the block design or the core |
| `openjls.dtbo` | device-tree overlay: UIO nodes + the DMA reserved-memory carveout | you change addresses or buffer sizes |
| `u-dma-buf.ko` | the [u-dma-buf](https://github.com/ikwzm/udmabuf) kernel module | your board's kernel differs from the PYNQ image's `6.6.10-xilinx-v2024.1-g3c0eca68c652` (see [`INTERNALS.md`](INTERNALS.md)) |

`build.tcl` and `design_encode_ethernet.tcl` recreate the Vivado project and
block design (the Tcl is the source of truth; the project is not committed).
`openjls.dtso` is the overlay source.

## Rebuilding

Requires `vivado`, `bootgen`, and `dtc` on the machine you build on. `<N>` is
the encoder pixel depth (8..16).

```sh
./build_specific_bitness.sh 8      # one depth
./build_all_bitness.sh             # every depth, 8..16
```

Both write `bitstreams/encode_eth_openjls_b<N>.bit.bin` and the shared
`openjls.dtbo`; `build_specific_bitness.sh --no-bitstream` rebuilds only the
overlay. `BITNESS` is fixed at synthesis, hence one bitstream per depth and a PL
reload per depth in the [verification sweep](../../Verification/README.md).
After a rebuild, rerun that sweep before committing the bitstreams.

`setup_bootargs.sh` (once per board) and
[`board_setup.sh`](../../Verification/board_setup.sh) (once per boot) are steps
2 and 3 of the [demo README](../../README.md).
