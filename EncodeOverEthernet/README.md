# EncodeOverEthernet

Stream raw images to an FPGA board over TCP, encode them to JPEG-LS **entirely in
hardware**, and stream the `.jls` files back. The CPU on the board only shuttles
bytes between the socket and an AXI DMA — the encode itself is 100% the OpenJLS
core in the PL.

```
┌─ host ───────┐            ┌─ board (PS) ─┐                     ┌─ board (PL) ──────┐
│              │─── TCP ───►│              │─── AXI DMA MM2S ───►│                   │
│ ojls_client  │            │ ojls_server  │◄─── AXI DMA S2MM ───│ openjls_axis_regs │
│              │◄─── TCP ───│              │◄──── AXI-Lite ─────►│                   │
│              │   (.jls)   │              │ dims, apply, status │                   │
└──────────────┘            └──────────────┘                     └───────────────────┘
```

The demo has two halves, and this README is the one place that sets up **both**
end to end. Each step links down to the reference doc that owns its detail:

* **Software** — two portable C programs from one `Software/` tree: `ojls_server`
  (board) and `ojls_client` (host). Board-agnostic; reference:
  [`Software/README.md`](Software/README.md).
* **Hardware** — the FPGA bitstream, device-tree overlay, and DMA module. Board-
  specific; a PYNQ-Z2 (Zynq-7020) build is provided under
  [`Hardware/pynq-z2/`](Hardware/pynq-z2/README.md), with design rationale and a
  porting guide in [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md).

## What you build, and where each piece runs

| Piece | Where it comes from | Runs on |
|---|---|---|
| `ojls_client` | you build it (`make`, host) | your host |
| `ojls_server` | you build it (cross-compiled on the host, or `make` on the board) | the board |
| bitstream `encode_eth_openjls_b<N>.bit.bin` (one per pixel depth `N`) | prebuilt, committed in the repo | board PL |
| `openjls.dtbo` overlay | prebuilt, committed in the repo | board (device tree) |
| `u-dma-buf.ko` | prebuilt, committed in the repo | board (kernel module) |

Only the two C programs are yours to build — the FPGA bitstreams and overlay are
committed pre-built, so **you don't need Vivado or the hardware toolchain to run
the demo**. (Rebuilding them is a reference task, not part of setup:
[`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md).)

## Setup, end to end

Setup starts at the software (the FPGA artifacts are already built, above). Each
step below says where it runs, and host and board commands are kept in separate
blocks. `<N>` is the encoder pixel depth (8..16) — start with **8**. The linked
reference docs carry the full options and troubleshooting.

## 1. Build the software

**On the host** — one Makefile builds both programs; the board runs `ojls_server`.

Cross-compile the server for the board (a PYNQ-Z2 is 32-bit ARM / Zynq-7000):

```sh
cd Software
make ojls_client
make CROSS_COMPILE=arm-linux-gnueabihf- ojls_server
```

Building the two targets separately keeps each binary for its own architecture — a
bare `make` builds *both* for whichever machine runs it. The cross compiler is
your distro's ARM hard-float toolchain package (`gcc-arm-linux-gnueabihf` on
Debian/Ubuntu, `arm-linux-gnueabihf-gcc` on Arch/AUR). For a 64-bit ARM board
use `aarch64-linux-gnu-`; to build the server natively on the board instead, run
`make ojls_server` there. Protocol, block-design requirements, and the porting
checklist: [`Software/README.md`](Software/README.md).

## 2. First-time board setup

**On the host and board** — once per board; persists across reboots, so
`board_setup.sh` doesn't redo it.

`setup_bootargs.sh` makes three persistent changes. It puts
`uio_pdrv_genirq.of_id=generic-uio` on the kernel command line in
`/boot/uEnv.txt` (binds the UIO register nodes) and pins `cma=128M`, seeding
from the running command line so `root=…` is preserved. It installs
`openjls.dtbo` plus a U-Boot `uenvcmd` that applies it to the device tree
**at boot** — that overlay carves out an exclusive reserved-memory region for
the DMA buffers, which the kernel must see at early boot (a runtime overlay
would be too late to reserve anything). And it caps U-Boot's fdt/initrd
relocation below the carveout so the patched DTB can't land inside it. It's
idempotent, and if the boot-time apply ever fails U-Boot falls through to a
stock boot — it can't brick the board. On the host, copy the
script, the overlay, and the `Software/` tree (with the `ojls_server` from
step 1) over:

```sh
BOARD=xilinx@192.168.2.99          # PYNQ's stock static IP
ssh-copy-id "$BOARD"               # once; the sweep in "Verifying it" needs key auth
scp Hardware/pynq-z2/setup_bootargs.sh Hardware/pynq-z2/openjls.dtbo "$BOARD:~/"
scp -r Software "$BOARD:~/"
```

(The stock PYNQ password is `xilinx` — `ssh-copy-id` asks for it once and every
later ssh/scp step is prompt-free.)

Then on the board, set the kernel command line and reboot for it to take effect:

```sh
sudo ./setup_bootargs.sh && sudo reboot
```

(`/boot/uEnv.txt` is the Xilinx/PYNQ U-Boot mechanism; another OS image may carry
kernel args elsewhere, e.g. `cmdline.txt` or an `extlinux.conf`. Note `/boot` is
the SD card's small FAT partition — `mmcblk0p1`, mounted at `/boot` on the board —
not part of the rootfs, so look for the file there if you mount the card on a PC.) The prebuilt
`u-dma-buf.ko` ships ready to copy (step 3) — no build, no internet on the
board — but it is built against the stock image's kernel; if `uname -r` on your
board differs, rebuild it first
([`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md)). Why a reserved-memory carveout rather than CMA, and how to size it for
larger images: [`INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md).

## 3. Flash and bring the board up

**On the host and board** — each boot.

On the host, copy the bitstream folder (`-r` ships every committed depth, b8..b16),
the DMA module, and the bring-up script:

```sh
BOARD=xilinx@192.168.2.99
scp -r Hardware/pynq-z2/bitstreams "$BOARD:~/"
scp Hardware/pynq-z2/u-dma-buf.ko "$BOARD:~/"
scp Verification/board_setup.sh "$BOARD:~/"
```

`board_setup.sh <N>` loads the depth-`<N>` bitstream, so `<N>` must match the
pixel depth of the images you'll send. On the board, as root:

```sh
sudo env HOME=$HOME ./board_setup.sh 8
```

It loads the PL, verifies the boot-time overlay and its DMA carveout are live
(pointing you back at step 2 if not), loads `u-dma-buf`, verifies the buffers,
and starts the server — idempotent, safe to re-run.

## 4. Encode

**On the host**

```sh
./Software/ojls_client <board-ip> image.pgm
```

Writes `image.jls` next to the input.

Any binary PGM (P5) works — its depth must match the loaded bitstream (8-bit
for BITNESS 8). No image handy? Generate a test gradient:

```sh
python3 -c 'w,h=640,480;open("image.pgm","wb").write(b"P5\n%d %d\n255\n"%(w,h)+bytes((x^y)&255 for y in range(h) for x in range(w)))'
```

## Verifying it

**On the host** — the sweep drives the board over ssh itself.

To prove the hardware output is byte-exact against a reference JPEG-LS encoder
(CharLS) across every pixel depth, run the hardware-in-the-loop sweep in
[`Verification/`](Verification/README.md).

The image corpus, the CharLS reference encoder, and the T.87 trust vectors live
in the `ThirdParty/OpenJLS` submodule — clone with `--recursive`, or initialize
it and build them once (from the repo root):

```sh
git submodule update --init --recursive
ThirdParty/OpenJLS/ThirdParty/fetch_third_party.sh charls          # reference encoder
"ThirdParty/OpenJLS/Verification/T87 conformance/fetch_reference_images.sh"
"ThirdParty/OpenJLS/Verification/Golden model/prepare_images.sh"   # image corpus (fetches + normalizes to grayscale PGM)
```

Then run the sweep (it targets `xilinx@192.168.2.99` by default; override with
`BOARD=`):

```sh
cd Verification
./run_hil_sweep.py --dry-run   # preflight only — board, ssh keys, corpus, CharLS
./run_hil_sweep.py             # full sweep, every committed depth
```

The `--dry-run` preflight checks all of the above and prints the exact
command for anything missing.

The sweep drives the board but does not bring it up: if it has been power-cycled
since you last ran [step 3](#3-flash-and-bring-the-board-up), run
`board_setup.sh` on it once more first. The per-depth `board_reload.sh` the sweep
runs itself only cycles the PL image and the server — it deliberately leaves
`u-dma-buf` alone. Preflight catches a board that needs it and says so.

The encoder IP is synthesized with a maximum image size (**65535 x 65535** in
the shipped block design); the server reads the exact limits from the `MAXDIM`
register and rejects anything larger, and the sweep skips such images rather
than reporting them as failures.

## Reference docs

| Doc | Covers |
|---|---|
| [`Software/README.md`](Software/README.md) | build details, wire protocol, block-design requirements, software porting checklist |
| [`Hardware/pynq-z2/README.md`](Hardware/pynq-z2/README.md) | scripted board build-and-flash quickstart |
| [`Hardware/pynq-z2/INTERNALS.md`](Hardware/pynq-z2/INTERNALS.md) | design rationale, manual bring-up, porting to another board |
| [`Verification/README.md`](Verification/README.md) | byte-exact-vs-CharLS hardware sweep |
