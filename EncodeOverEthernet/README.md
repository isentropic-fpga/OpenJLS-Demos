# EncodeOverEthernet

Stream raw images to an FPGA board over TCP, encode them to JPEG-LS **entirely in
hardware**, and stream the `.jls` files back. The CPU on the board only shuttles
bytes between the socket and an AXI DMA — the encode itself is 100% the OpenJLS
core in the PL.

![Host PC running ojls_client exchanges image data and the compressed stream with a PYNQ-Z2 over TCP. On the board, ojls_server copies pixels into DDR, an AXI DMA moves them over AXI4 and streams them into the OpenJLS core in the FPGA over AXI4-Stream, and the server configures the core directly over AXI4-Lite.](../Docs/Images/EncodeOverEthernet_arch.png)

This page is the reproduction guide: follow it top to bottom and you go from a
clean clone to a board that encodes images. It covers the PYNQ-Z2 build that
ships in this repo. Design rationale, manual bring-up, and porting to another
board are deliberately kept out of the steps — they live in
[`Hardware/pynq-z2/INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md).

## What you need

**Hardware**

* A PYNQ-Z2 board (Zynq-7020) with the stock PYNQ SD image. The prebuilt
  `u-dma-buf.ko` in this repo is compiled for that image's kernel,
  `6.6.10-xilinx-v2024.1-g3c0eca68c652`; check yours with `uname -r` on the
  board and see [Troubleshooting](#troubleshooting) if it differs.
* An Ethernet cable between the board and your host (a switch works too).

**Host** — Linux, with:

| Tool | For | Needed |
|---|---|---|
| `gcc`, `make` | building the client | always |
| `arm-linux-gnueabihf-gcc` | cross-building the server for the board | unless you build on the board |
| `ssh`, `scp` | copying files to the board | always |
| `python3` | the verification sweep | only for [verification](#verify-it-byte-exact-vs-charls) |
| `vivado`, `bootgen`, `dtc` | rebuilding the bitstreams | **not needed** — they ship prebuilt |

The cross compiler is your distribution's ARM hard-float toolchain:
`gcc-arm-linux-gnueabihf` on Debian/Ubuntu, `arm-linux-gnueabihf-gcc` on
Arch/AUR. If you would rather compile on the board, a PYNQ image has `gcc` —
run `make ojls_server` there instead.

**What gets built, and what is already built**

| Piece | Where it comes from | Runs on |
|---|---|---|
| `ojls_client` | you build it | your host |
| `ojls_server` | you build it (cross-compiled, or natively on the board) | the board |
| `encode_eth_openjls_b<N>.bit.bin`, one bitstream per pixel depth `N` | prebuilt, committed | board PL |
| `openjls.dtbo` device-tree overlay | prebuilt, committed | board |
| `u-dma-buf.ko` kernel module | prebuilt, committed | board |

Only the two C programs are built during setup, so **no FPGA toolchain is
required to run this demo**. Rebuilding the bitstreams is a separate, optional
task: [`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md).

**Time** — a few minutes of work, plus one board reboot. Steps 1–4 below are
about 10 minutes end to end.

## Reaching the board

The stock PYNQ image asks for DHCP on `eth0` and falls back to the static
address **192.168.2.99** when there is no lease — which is what a direct
host↔board cable gives you. Every command below uses that address; if your
board is on a DHCP network, substitute the address it got.

Give your host an address on the same subnet:

```sh
sudo ip link set <iface> up
sudo ip addr add 192.168.2.1/24 dev <iface>
ping -c1 192.168.2.99
```

The stock login is `xilinx` / `xilinx`. Install an ssh key once — it saves
typing that password on every copy below, and the verification sweep requires
key-based auth:

```sh
ssh-copy-id xilinx@192.168.2.99
```

## Step 0 — Get the repo

The verification corpus lives in a submodule, so clone recursively:

```sh
git clone --recursive https://github.com/isentropic-fpga/OpenJLS-Demos
cd OpenJLS-Demos/EncodeOverEthernet
```

Already cloned without `--recursive`? Run `git submodule update --init
--recursive` from the repo root. All paths below are relative to
`EncodeOverEthernet/`.

## Step 1 — Build the software

**Runs on: host.** One Makefile builds both programs; build each for the
machine it runs on.

```sh
cd Software
make ojls_client                                    # for this host
make CROSS_COMPILE=arm-linux-gnueabihf- ojls_server # for the board
cd ..
```

Build the two targets separately — a bare `make` builds *both* for whichever
machine runs it. For a 64-bit ARM board use `CROSS_COMPILE=aarch64-linux-gnu-`.

Wire protocol, block-design requirements, and the software porting checklist:
[`Software/README.md`](Software/README.md).

## Step 2 — First-time board setup

**Runs on: host, then board.** Once per board — it persists across reboots.

This installs the device-tree overlay that reserves memory for the DMA buffers
and puts `uio_pdrv_genirq.of_id=generic-uio` on the kernel command line. The
overlay must be applied by U-Boot *at boot*, because the kernel only honors a
reserved-memory carveout it sees at early boot; `setup_bootargs.sh` arranges
that in `/boot/uEnv.txt`. It is idempotent, and if the boot-time apply ever
fails U-Boot falls through to a stock boot — it cannot brick the board.

Copy the script, the overlay, and the `Software/` tree (with the `ojls_server`
you just built) to the board:

```sh
BOARD=xilinx@192.168.2.99
scp Hardware/pynq-z2/setup_bootargs.sh Hardware/pynq-z2/openjls.dtbo "$BOARD:~/"
scp -r Software "$BOARD:~/"
```

Then, on the board:

```sh
sudo ./setup_bootargs.sh && sudo reboot
```

The reboot is required — the changes only take effect on the next boot.

> `/boot/uEnv.txt` is the Xilinx/PYNQ U-Boot mechanism, and `/boot` is the SD
> card's small FAT partition (`mmcblk0p1`), not part of the rootfs. Another OS
> image may carry kernel arguments elsewhere (`cmdline.txt`, `extlinux.conf`).
> Why a carveout rather than CMA, and how to size it for larger images:
> [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md).

## Step 3 — Bring the board up

**Runs on: host, then board.** Once per boot.

Copy the bitstreams (`-r` ships every committed depth, b8..b16), the DMA kernel
module, and the bring-up script:

```sh
BOARD=xilinx@192.168.2.99
scp -r Hardware/pynq-z2/bitstreams "$BOARD:~/"
scp Hardware/pynq-z2/u-dma-buf.ko "$BOARD:~/"
scp Verification/board_setup.sh "$BOARD:~/"
```

`board_setup.sh <N>` loads the depth-`<N>` bitstream, where `<N>` is the encoder
pixel depth (8..16) and **must match the pixel depth of the images you will
send**. Start with 8. On the board, as root:

```sh
sudo env HOME=$HOME ./board_setup.sh 8
```

(`env HOME=$HOME` matters: the script looks for its artifacts under `$HOME`, and
plain `sudo` would point it at root's.)

It loads the PL, verifies the boot-time overlay and its DMA carveout are live,
loads `u-dma-buf`, verifies the buffers, and starts the server. It is
idempotent — safe to re-run at any time. Expect six numbered stages and this
last line:

```
=== 5. verify all three buffers allocated ===
  udmabuf-ojls-tx        128 MiB
  udmabuf-ojls-rx        128 MiB
  udmabuf-ojls-desc      256 KiB
=== 6. start server (detached; confirm it latched BITNESS 8) ===

BOARD READY — BITNESS 8, buffers up, server listening on :19020
```

Anything else means a step failed; the script says which and what to do. The
server logs to `/tmp/ojls_server.log` on the board.

## Step 4 — Encode an image

**Runs on: host.**

Any binary PGM (P5) works, as long as its depth matches the loaded bitstream.
No image handy? Generate a test gradient:

```sh
python3 -c 'w,h=640,480;open("image.pgm","wb").write(b"P5\n%d %d\n255\n"%(w,h)+bytes((x^y)&255 for y in range(h) for x in range(w)))'
```

Encode it:

```sh
./Software/ojls_client 192.168.2.99 image.pgm
```

It reports the image it read, the encoded size and compression ratio, and the
end-to-end rate:

```
image.pgm: 640x480, 8 bpp, 307200 bytes raw
image.jls: ..... bytes (....:1)
1 round trip in ..... ms — .... MB/s of pixels end-to-end
```

The `.jls` is written next to the input and is a standard JPEG-LS file — any
decoder reads it. `-n 100` repeats the round trip for a throughput figure.

That's the demo. What follows proves the output is correct.

## Verify it: byte-exact vs CharLS

**Runs on: host** — the sweep drives the board over ssh itself.

The sweep encodes the whole OpenJLS golden corpus on the board, one pixel depth
at a time, and byte-compares every result against the CharLS reference encoder.
It needs the corpus and CharLS, which live in the OpenJLS submodule; build them
once, from the repo root (one level up — the submodule sits beside
`EncodeOverEthernet/`, not inside it):

```sh
cd ..                                                              # repo root
ThirdParty/OpenJLS/ThirdParty/fetch_third_party.sh charls          # reference encoder
"ThirdParty/OpenJLS/Verification/T87 conformance/fetch_reference_images.sh"
"ThirdParty/OpenJLS/Verification/Golden model/prepare_images.sh"   # ~600 MB of PGMs
```

Then run it (it targets `xilinx@192.168.2.99` by default; override with
`BOARD=`):

```sh
cd EncodeOverEthernet/Verification
./run_hil_sweep.py --dry-run   # preflight only: board, ssh keys, corpus, CharLS
./run_hil_sweep.py             # full sweep, every committed depth
```

`--dry-run` checks every prerequisite and prints the exact command for anything
missing — start there. The full sweep reloads the PL nine times and encodes 287
images, so it is not quick; `--bitness 8 --limit 5` is a fast smoke test.

```
==================== summary ====================
  b8 :  225 pass     0 fail     0 skip   [OK]
  ...
  all: 287 pass, 0 fail, 0 skip
```

Exit status is non-zero if anything mismatched. Per-image results land in
`Verification/out/results.csv`. Details:
[`Verification/README.md`](Verification/README.md).

Two things the sweep will not do for you: it does not bring the board up (run
`board_setup.sh` once after each power cycle — preflight catches this and says
so), and it skips images larger than the hardware's synthesized limits
(**65535 x 65535** in the shipped design, read from the `MAXDIM` register)
rather than reporting them as failures.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cannot connect to 192.168.2.99:19020` | server not running | Re-run step 3 on the board |
| `server rejected the image: bitness does not match hardware` | loaded bitstream depth ≠ image depth | `board_setup.sh <N>` with `<N>` matching the image (PGM maxval 255→8, 4095→12, 65535→16) |
| `server rejected the image: dimensions outside hardware range` | image exceeds the synthesized `MAXDIM` | Use a smaller image, or rebuild the IP with a larger maximum |
| `server rejected the image: image exceeds board DMA buffers` | image larger than the 128 MiB tx buffer | Resize the carveout — [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) |
| `!! boot device tree is missing: …` from `board_setup.sh` | step 2 never ran, or its boot-time apply fell back | Re-run `setup_bootargs.sh` as root, then reboot |
| `insmod: … Invalid module format` | your kernel differs from the one `u-dma-buf.ko` was built for | Rebuild the module — [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) |
| `udmabuf-ojls-rx MISSING` in stage 5 | a buffer size does not match its carveout | [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md), "Large images and the DMA carveout" |
| The sweep prompts for an ssh password | key-based auth not set up | `ssh-copy-id xilinx@192.168.2.99` |

## Reference docs

| Doc | Covers |
|---|---|
| [`Software/README.md`](Software/README.md) | build details, wire protocol, block-design requirements, software porting checklist |
| [`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md) | rebuilding the bitstreams and overlay from source |
| [`Hardware/pynq-z2/INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) | design rationale, manual bring-up, porting to another board |
| [`Verification/README.md`](Verification/README.md) | the byte-exact-vs-CharLS hardware sweep |
