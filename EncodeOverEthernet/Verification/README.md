# Hardware-in-the-loop verification

> Bring the board up with the [demo README](../README.md) (through step 3)
> first, or run `./run_hil_sweep.py --dry-run` to see what is missing.

Encodes the OpenJLS golden corpus on the PYNQ-Z2, one pixel depth at a time,
and byte-compares every `.jls` against CharLS, the same reference the core's
golden-model simulation uses. A pass means the silicon matches CharLS exactly
at `BITNESS` 8..16, including the full-size originals too large to simulate.

The corpus, CharLS and the T.87 reference images belong to the
`ThirdParty/OpenJLS` submodule; this harness only drives the board and
compares. Its report, [`out/report.md`](out/report.md), is what the
hardware-in-the-loop row of the core's
[verification report](https://isentropic-fpga.github.io/OpenJLS/) links to, next
to the OSVVM, golden-model and post-synthesis suites run in simulation.

## How it works

Each image's depth comes from its PGM maxval (255→8, 4095→12, 65535→16), as in
the client. First, CharLS must reproduce the official T.87 file `T16E0.JLS`
byte-exact. Then, per depth `N`:

1. `board_reload.sh` (over ssh, as root) loads
   `bitstreams/encode_eth_openjls_b<N>.bit.bin` and restarts the server;
2. every image of depth `N` goes through `ojls_client`;
3. each result is `cmp`'d against its CharLS golden.

The restart matters: the server reads `BITNESS` from the CAPS register once at
startup and rejects other depths. The overlay and `u-dma-buf` are
depth-independent and stay loaded.

## Files

| File | Runs on | What |
|---|---|---|
| `run_hil_sweep.py` | host | preflight, CharLS check, per-depth reload + encode + compare, report |
| `board_setup.sh` | board | bring-up after a power cycle: PL, overlay check, `u-dma-buf`, server |
| `board_reload.sh` | board | per-depth bitstream reload + server restart (staged by the driver) |

## Prerequisites

Preflight checks each and prints the command for anything missing.

- **Corpus** — `ThirdParty/OpenJLS/Verification/Golden model/prepare_images.sh`
- **CharLS** — `ThirdParty/OpenJLS/ThirdParty/fetch_third_party.sh charls`
- **T.87 reference images** — `ThirdParty/OpenJLS/Verification/T87 conformance/fetch_reference_images.sh`
- **Client** — `make -C ../Software ojls_client`
- **Board** — through step 3 of the [demo README](../README.md), plus an ssh
  key (`ssh-copy-id xilinx@192.168.2.99`). The driver stages the bitstreams
  and `board_reload.sh` itself.

## Running

```sh
./run_hil_sweep.py --dry-run               # preflight + depth histogram, no board needed
./run_hil_sweep.py                         # full sweep
./run_hil_sweep.py --bitness 8 12 --limit 5  # smoke test
```

Exit status is non-zero on any mismatch or error. The sweep writes
`out/results.csv` and `out/report.md` (per image: name, depth, bytes in and
out, pass/fail, plus a total row). Both are committed; the encoded outputs and
cached goldens are not.

Configuration is by environment variable, documented in the header of
`run_hil_sweep.py`. The usual ones: `BOARD` (default `xilinx@192.168.2.99`),
`SUDO_PASS` (default `xilinx`), `SSH_KEY`, path overrides (`IMAGES_DIR`,
`FULL_IMAGES_DIR`, `CHARLS`, `CLIENT`, `BITSTREAM_DIR`) and `TX_BYTES` (images
above the board tx buffer are skipped, not failed).
