# EncodeOverEthernet

Stream raw images to an FPGA board over TCP, encode them to JPEG-LS **entirely in
hardware**, and stream the `.jls` files back. The board's CPU only moves bytes
between the socket and an AXI DMA; the encode is the OpenJLS core in the PL.

![Host PC running ojls_client exchanges image data and the compressed stream with a PYNQ-Z2 over TCP. On the board, ojls_server copies pixels into DDR, an AXI DMA moves them over AXI4 and streams them into the OpenJLS core in the FPGA over AXI4-Stream, and the server configures the core directly over AXI4-Lite.](../Docs/Images/EncodeOverEthernet_arch.png)

This is the reproduction guide for the PYNQ-Z2 build: top to bottom, from a
clean clone to a board that encodes images. Design rationale, manual bring-up
and porting are in [`Hardware/pynq-z2/INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md).

## What you need

**Hardware**

* A PYNQ-Z2 (Zynq-7020) with the stock PYNQ SD image. The prebuilt
  `u-dma-buf.ko` targets its kernel, `6.6.10-xilinx-v2024.1-g3c0eca68c652`;
  if `uname -r` on the board differs, see [Troubleshooting](#troubleshooting).
* An Ethernet cable to your host (a switch works too).

**Host** — Linux, with:

| Tool | For | Needed |
|---|---|---|
| `gcc`, `make` | building the client | always |
| `arm-linux-gnueabihf-gcc` | cross-building the server | unless you build on the board |
| `ssh`, `scp` | copying files to the board | always |
| `python3` | the verification sweep | only for [verification](#verify-it-byte-exact-vs-charls) |
| `vivado`, `bootgen`, `dtc` | rebuilding the bitstreams | no — they ship prebuilt |

The cross compiler is `gcc-arm-linux-gnueabihf` on Debian/Ubuntu,
`arm-linux-gnueabihf-gcc` on Arch/AUR. To compile on the board instead, run
`make ojls_server` there.

**What gets built**

| Piece | Source | Runs on |
|---|---|---|
| `ojls_client` | you build it | host |
| `ojls_server` | you build it (cross, or on the board) | board |
| `encode_eth_openjls_b<N>.bit.bin`, one per pixel depth `N` | prebuilt | board PL |
| `openjls.dtbo` device-tree overlay | prebuilt | board |
| `u-dma-buf.ko` kernel module | prebuilt | board |

Rebuilding the bitstreams is optional:
[`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md).

**Time** — steps 1–4 take about 10 minutes, plus one board reboot.

## Reaching the board

The stock PYNQ image tries DHCP on `eth0` and falls back to **192.168.2.99**,
which is what a direct cable gives you. The commands below use that address;
substitute yours if the board got a DHCP lease.

```sh
sudo ip link set <iface> up
sudo ip addr add 192.168.2.1/24 dev <iface>
ping -c1 192.168.2.99
```

The login is `xilinx` / `xilinx`. Install an ssh key once; the verification
sweep requires key-based auth:

```sh
ssh-copy-id xilinx@192.168.2.99
```

## Step 0 — Get the repo

The core and the verification corpus are in a submodule, so clone recursively:

```sh
git clone --recursive https://github.com/isentropic-fpga/OpenJLS-Demos
cd OpenJLS-Demos/EncodeOverEthernet
```

Already cloned? `git submodule update --init --recursive` from the repo root.
Paths below are relative to `EncodeOverEthernet/`.

## Step 1 — Build the software

**Runs on: host.**

```sh
cd Software
make ojls_client                                    # for this host
make CROSS_COMPILE=arm-linux-gnueabihf- ojls_server # for the board
cd ..
```

Build the targets separately: a bare `make` builds both for the current
machine. For a 64-bit ARM board use `CROSS_COMPILE=aarch64-linux-gnu-`.
Wire protocol and porting checklist: [`Software/README.md`](Software/README.md).

## Step 2 — First-time board setup

**Runs on: host, then board.** Once per board; it persists across reboots.

This installs the device-tree overlay that reserves memory for the DMA buffers
and adds `uio_pdrv_genirq.of_id=generic-uio` to the kernel command line. The
kernel only honors a reserved-memory carveout it sees at early boot, so U-Boot
must apply the overlay; `setup_bootargs.sh` arranges that in `/boot/uEnv.txt`.
It is idempotent, and if the boot-time apply fails U-Boot falls back to a stock
boot, so it cannot brick the board.

```sh
BOARD=xilinx@192.168.2.99
scp Hardware/pynq-z2/setup_bootargs.sh Hardware/pynq-z2/openjls.dtbo "$BOARD:~/"
scp -r Software "$BOARD:~/"
```

On the board:

```sh
sudo ./setup_bootargs.sh && sudo reboot
```

> `/boot` is the SD card's FAT partition (`mmcblk0p1`). Other OS images may keep
> kernel arguments elsewhere (`cmdline.txt`, `extlinux.conf`). Why a carveout
> rather than CMA, and how to size it:
> [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md).

## Step 3 — Bring the board up

**Runs on: host, then board.** Once per boot.

```sh
BOARD=xilinx@192.168.2.99
scp -r Hardware/pynq-z2/bitstreams "$BOARD:~/"
scp Hardware/pynq-z2/u-dma-buf.ko Verification/board_setup.sh "$BOARD:~/"
```

`board_setup.sh <N>` loads the depth-`<N>` bitstream (8..16); `<N>` **must match
the pixel depth of the images you send**. Start with 8. On the board:

```sh
sudo env HOME=$HOME ./board_setup.sh 8
```

(`env HOME=$HOME` keeps `$HOME` pointing at your artifacts, not root's.)

It loads the PL, checks the boot-time overlay and DMA carveout, loads
`u-dma-buf`, checks the buffers and starts the server. It is idempotent.
Expect six numbered stages ending with:

```
=== 5. verify all three buffers allocated ===
  udmabuf-ojls-tx        128 MiB
  udmabuf-ojls-rx        128 MiB
  udmabuf-ojls-desc      256 KiB
=== 6. start server (detached; confirm it latched BITNESS 8) ===

BOARD READY — BITNESS 8, buffers up, server listening on :19020
```

On failure the script names the step and the fix. The server logs to
`/tmp/ojls_server.log` on the board.

## Step 4 — Encode an image

**Runs on: host.**

Any binary PGM (P5) whose depth matches the loaded bitstream. A test gradient:

```sh
python3 -c 'w,h=640,480;open("image.pgm","wb").write(b"P5\n%d %d\n255\n"%(w,h)+bytes((x^y)&255 for y in range(h) for x in range(w)))'
./Software/ojls_client 192.168.2.99 image.pgm
```

```
image.pgm: 640x480, 8 bpp, 307200 bytes raw
image.jls: ..... bytes (....:1)
1 round trip in ..... ms — .... MB/s of pixels end-to-end
```

The `.jls` lands next to the input and is a standard JPEG-LS file. `-n 100`
repeats the round trip for a throughput figure.

## Verify it: byte-exact vs CharLS

**Runs on: host**; the sweep drives the board over ssh.

The sweep encodes the OpenJLS golden corpus on the board, one pixel depth at a
time, and byte-compares every result against CharLS. Build the corpus and
CharLS once, from the repo root:

```sh
cd ..
ThirdParty/OpenJLS/ThirdParty/fetch_third_party.sh charls
"ThirdParty/OpenJLS/Verification/T87 conformance/fetch_reference_images.sh"
"ThirdParty/OpenJLS/Verification/Golden model/prepare_images.sh"   # ~1.2 GB
```

Then run it (target defaults to `xilinx@192.168.2.99`; override with `BOARD=`):

```sh
cd EncodeOverEthernet/Verification
./run_hil_sweep.py --dry-run   # preflight: board, ssh key, corpus, CharLS
./run_hil_sweep.py             # every depth, 5639 images
```

`--dry-run` prints the exact command for anything missing. The full sweep
reloads the PL nine times and takes a while; `--bitness 8 --limit 5` is a quick
smoke test.

```
==================== summary ====================
  b8 :  732 pass     0 fail     0 skip   [OK]
  ...
  all: 5639 pass, 0 fail, 0 skip
```

Exit status is non-zero on any mismatch; per-image results go to
`Verification/out/results.csv`. The sweep does not bring the board up (run
`board_setup.sh` after each power cycle; preflight checks it) and skips, rather
than fails, images above the synthesized `MAXDIM` (65535 × 65535 here).
Details: [`Verification/README.md`](Verification/README.md).

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cannot connect to 192.168.2.99:19020` | server not running | Re-run step 3 |
| `server rejected the image: bitness does not match hardware` | bitstream depth ≠ image depth | `board_setup.sh <N>` with `<N>` matching the image (maxval 255→8, 4095→12, 65535→16) |
| `server rejected the image: dimensions outside hardware range` | image exceeds `MAXDIM` | Smaller image, or rebuild the IP with a larger maximum |
| `server rejected the image: image exceeds board DMA buffers` | image over the 128 MiB tx buffer | Resize the carveout — [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) |
| `!! boot device tree is missing: …` from `board_setup.sh` | step 2 skipped, or its boot-time apply fell back | Re-run `setup_bootargs.sh` as root, reboot |
| `insmod: … Invalid module format` | kernel differs from the one `u-dma-buf.ko` targets | Rebuild the module — [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) |
| `udmabuf-ojls-rx MISSING` in stage 5 | buffer size does not match its carveout | [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md), "Large images and the DMA carveout" |
| The sweep prompts for an ssh password | no ssh key | `ssh-copy-id xilinx@192.168.2.99` |

## Reference docs

| Doc | Covers |
|---|---|
| [`Software/README.md`](Software/README.md) | build, wire protocol, block-design requirements, porting |
| [`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md) | rebuilding the bitstreams and overlay |
| [`Hardware/pynq-z2/INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) | design rationale, manual bring-up, porting |
| [`Verification/README.md`](Verification/README.md) | the hardware sweep |
