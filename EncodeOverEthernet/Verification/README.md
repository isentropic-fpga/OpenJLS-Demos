# Hardware-in-the-loop verification

> The verification half of the [EncodeOverEthernet](../README.md) demo. Set the
> board up by following that README first (through step 3), then come back —
> or just run `./run_hil_sweep.py --dry-run`, which tells you what is missing.

Drives the PYNQ-Z2 through the OpenJLS golden corpus, one encoder pixel depth at
a time, and byte-compares every hardware-encoded `.jls` against a CharLS
reference — the **same oracle** the simulation golden model uses. A pass proves
the silicon reproduces CharLS exactly, across `BITNESS` 8..16.

This is a *consumer* of the core's verification assets, not an owner of them.
The image corpus, the CharLS reference encoder, and the T.87 trust vectors all
live in the OpenJLS submodule and are produced by its scripts; this harness only
orchestrates the board, the transport, and the comparison. Nothing is
duplicated.

## How it works

The corpus is not organized by depth — each image carries its own via its
PGM maxval (255→8, 4095→12, 65535→16). The driver buckets images by that
(exactly as the client derives it) and, for each depth `N`:

1. reloads `bitstreams/encode_eth_openjls_b<N>.bit.bin` on the board and
   restarts the server (`board_reload.sh`, over ssh, as root);
2. sends every image in that bucket through `ojls_client`;
3. `cmp`s each result against its CharLS golden; tallies pass/fail/skip.

The server restart in step 1 is load-bearing: it reads the hardware `BITNESS`
from the CAPS register **once at startup** and rejects mismatched requests, so a
still-running server would reject the whole new bucket after a reload. The
device-tree overlay and `u-dma-buf` are depth-independent and stay loaded.

## Files

| file | runs on | what |
|---|---|---|
| `run_hil_sweep.py` | host | orchestrator: preflight, CharLS gate, bucket, per-depth reload + encode + compare, report |
| `board_setup.sh`   | board | **one-shot bring-up after a power cycle**: load PL, verify the boot-time overlay + DMA carveout, load `u-dma-buf`, verify buffers, start server |
| `board_reload.sh`  | board | lighter per-depth path: reload a depth's bitstream + restart the server (copied over by the driver) |

`board_setup.sh` is the whole board-side bring-up (documented step by step in
`../Hardware/pynq-z2/INTERNALS.md`) collapsed into one idempotent command — run
it once after each boot and the board is ready. (The device-tree overlay itself
is applied by U-Boot at boot — installed once per board by
`../Hardware/pynq-z2/setup_bootargs.sh`; `board_setup.sh` verifies it rather
than applying it.) `board_reload.sh` is what the sweep calls between depths,
when `u-dma-buf` is already up and only the PL image and server need to cycle.

## Prerequisites

Produced by the core / demo, checked by the driver's preflight (which prints the
exact command for anything missing). The first three live in the
`ThirdParty/OpenJLS` submodule — `git submodule update --init --recursive`
first if it's empty:

- **Corpus** — `ThirdParty/OpenJLS/Verification/Golden model/prepare_images.sh`
  (fetches the curated public datasets + synthesizes the depth/boundary probes).
- **CharLS** — `ThirdParty/OpenJLS/ThirdParty/fetch_third_party.sh charls`.
- **T.87 trust vectors** (for the gate) —
  `ThirdParty/OpenJLS/Verification/T87 conformance/fetch_reference_images.sh`.
- **Client** — `make -C ../Software ojls_client`.
- **Bitstreams** — `../Hardware/pynq-z2/build_all_bitness.sh`.
- **Board** — set up through step 3 of the [demo README](../README.md): server
  built on the board, `setup_bootargs.sh` done once, `board_setup.sh` run this
  boot. Plus **key-based** ssh auth, which the demo doesn't need but the sweep
  does — once: `ssh-copy-id xilinx@192.168.2.99` (stock password `xilinx`).
  The driver stages the bitstreams and the reload script itself.

## Running

```sh
# Preview the corpus depth histogram and check prerequisites — no board needed:
./run_hil_sweep.py --dry-run

# Full sweep (defaults to BOARD=xilinx@192.168.2.99, SUDO_PASS=xilinx):
./run_hil_sweep.py

# Subset of depths, few images each (smoke test):
./run_hil_sweep.py --bitness 8 12 --limit 5
```

Exit status is non-zero if any image mismatched or errored. Per-image results
land in `out/results.csv`; encoded outputs and cached goldens under `out/`
(git-ignored).

Configuration is entirely via environment — see the header of
`run_hil_sweep.py` for every variable and its default. Key ones: `BOARD` /
`SUDO_PASS` (default to the standard bench setup above; set a different target
or `BOARD=` empty for dry-run-only), `SSH_KEY`,
`BITSTREAM_DIR`/`IMAGES_DIR`/`CHARLS`/`CLIENT` overrides,
and `TX_BYTES` (images larger than the board tx buffer are skipped, not failed).
