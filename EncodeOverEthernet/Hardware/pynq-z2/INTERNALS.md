# PYNQ-Z2 internals, manual bring-up, and porting notes

The [README](README.md) gets you a running board with the pre-built scripts.
This document is the reference behind those scripts: what each one automates
step by step, *why* the design is the way it is, and what to change to port it
to another board. Read this if a script fails, if you're modifying the block
design, or if you're bringing the demo up on different hardware.

## The design, and why it's shaped this way

PS7 GEM handles Ethernet; `openjls_axis_regs` (the OpenJLS encoder wrapped with
an AXI-Lite register bank, BITNESS 8, max 65535×65535) and an AXI DMA sit in the
PL on a **50 MHz** fabric clock.

**Why 50 MHz and not the ~83 MHz the PS can supply.** The OpenJLS `byte_stuffer`
has a deep, high-fanout combinational path that will not close timing faster on
this `-1` speed grade. It is a core-logic limit, not a fit or congestion one —
the part is only ~25% full. `build.tcl` refuses to stage a bitstream whose
post-route WNS is negative, so this limit can't be shipped by accident; if you
raise the clock, the build fails rather than producing an unreliable bitstream.

**Why Scatter/Gather.** SG lets the DMA chain descriptors, so one transfer is no
longer capped at the 64 MiB a 26-bit length register allows. An image is bounded
only by the `u-dma-buf` sizes, and thus by how much DRAM the board reserves (see
[Large images and the DMA carveout](#large-images-and-the-dma-carveout)).

**Why the S2MM status stream is disabled** (`c_sg_include_stscntrl_strm 0`).
This is the hard-won one: with the AXI-Ethernet-style SG control/status
side-channel enabled, S2MM withholds descriptor completion until the source
drives a per-packet status word on `s_axis_s2mm_sts` — but the BD ties that
slave stream off. So S2MM receives all the encoded data and the encoder's TLAST
correctly (bytes are byte-perfect in DRAM) but never writes RXEOF, stays
RUNNING, and the server times out. MM2S is unaffected because its counterpart
`m_axis_mm2s_cntrl` is a master output, harmless when ignored — which is why
only S2MM stalled. **Do not re-enable the status/control stream** unless you
also drive `s_axis_s2mm_sts`.

**Why a bitstream per precision.** BITNESS is a synth-time generic. The
encoder's `s_axis_pixel_tdata` is `8 * ceil(BITNESS/8)` bits wide — 8 bits for
BITNESS 8, 16 bits for 9..16 (a 9-bit sample sits losslessly in a 16-bit
container) — so the DMA MM2S stream width has to move with it. `build.tcl` sets
both together from `--bitness`. An AXIS data-width converter does *not* buy
build-once robustness here: only its slave side auto-propagates width; the
master TDATA width is a fixed IP parameter you'd have to size per precision
anyway, recreating the problem. `m_axis_jls` (and thus S2MM) is a fixed 64 bits
regardless of BITNESS. See [`../../Software/README.md`](../../Software/README.md)
for the full list of requirements the design satisfies.

## Files

* `openjls.dtso` — device tree overlay: `generic-uio` nodes for
  `openjls_axis_regs` (0x4000_0000) and the AXI DMA (0x4040_0000), plus three
  `u-dma-buf` buffers (`udmabuf-ojls-tx` pixels in, `udmabuf-ojls-rx` bitstream
  out, `udmabuf-ojls-desc` the SG descriptor rings).
* `design_encode_ethernet.tcl` — the block design (`write_bd_tcl` export, pinned
  to Vivado 2025.2). Source of truth; the generated project is never committed.
* `build.tcl` — recreates the Vivado project from it in `./build/` (git-ignored):
  adds the core sources from `ThirdParty/OpenJLS`, sources the block design,
  retargets it to `--bitness`, generates the HDL wrapper, and (with
  `--bitstream`) implements and gates on timing.
* `build_specific_bitness.sh` — one precision, sources → the two board files
  (`.bit.bin` + `.dtbo`). Wraps `build.tcl` + `bootgen` + `dtc`. `--no-bitstream`
  builds only the overlay (for when the `.bit.bin` already exists).
* `build_all_bitness.sh` — the same, looped over BITNESS 8..16, staging every
  `bitstreams/encode_eth_openjls_b<N>.bit.bin` the HIL sweep reloads, plus the
  shared overlay once at the end.

## Manual bring-up (what the scripts automate)

### On the development machine

`build_specific_bitness.sh N` does all of this for one precision (and
`build_all_bitness.sh` for every precision); the steps below are what they run.

1. **Bitstream** — build the project with the bitstream:

   ```sh
   vivado -mode batch -source build.tcl -tclargs --bitstream --bitness 8
   ```

   The wrapper is the top, so the file is named after it:

   ```
   build/encode_ethernet.runs/impl_1/design_encode_ethernet_wrapper.bit
   ```

2. **`.bit` → `.bin`** — the Linux FPGA manager on current kernels rejects raw
   `.bit` files (only the legacy `/dev/xdevcfg` accepted those; PYNQ's Python
   `Overlay` class converts them in software), so convert with bootgen, which
   ships with Vivado:

   ```sh
   cd build/encode_ethernet.runs/impl_1
   cp design_encode_ethernet_wrapper.bit system.bit    # short, stable name
   echo 'all: { system.bit }' > system.bif             # bootgen input list
   # -process_bitstream bin : raw .bin (no boot header)   -arch zynq : Zynq-7000
   # -w : overwrite                                       (zynqmp for UltraScale+)
   bootgen -image system.bif -arch zynq -process_bitstream bin -w
   # -> system.bit.bin
   ```

3. **Overlay** — compile the device tree overlay:

   ```sh
   dtc -@ -O dtb -o openjls.dtbo openjls.dtso
   ```

4. Copy the artifacts and the `Software/` directory to the board over `scp`
   (default PYNQ login `xilinx` / `xilinx`). The board is `192.168.3.1` over the
   USB gadget, or the `pynq` hostname / its DHCP address over Ethernet:

   ```sh
   BOARD=xilinx@192.168.3.1   # or xilinx@pynq over Ethernet
   scp bitstreams/encode_eth_openjls_b8.bit.bin "$BOARD:~/bitstreams/"
   scp openjls.dtbo u-dma-buf.ko "$BOARD:~/"
   scp -r ../../Software "$BOARD:~/"
   ```

   (Paths are relative to `EncodeOverEthernet/Hardware/pynq-z2/`.) Alternatively
   `git clone --recursive` the repo on the board and build there.

### On the board (as root)

`board_setup.sh` ([`../../Verification/board_setup.sh`](../../Verification/board_setup.sh))
does all of this in one idempotent command. The steps below are what it runs,
in the order that matters.

1. **See what already owns the PL address windows.** Do **not** trust
   `grep 4000 /proc/iomem` — `generic-uio` maps registers without reserving
   them, so UIO squatters never appear there. Check the driver instead:

   ```sh
   cat /sys/class/uio/uio*/name    # what UIO devices exist right now
   ```

   The stock PYNQ image ships a whole-PL UIO node, `fabric` at 0x4000_0000,
   which overlaps the `openjls` register bank. It is **harmless** — two UIO devices can
   map the same window, and the server picks its device by name — so no need to
   remove it. (To remove it anyway:
   `echo 40000000.fabric > /sys/bus/platform/drivers/uio_pdrv_genirq/unbind`.)
   Just don't also have PYNQ's full Python overlay (`base.bit`) driving the PL.

2. **The overlay is already live** — U-Boot applied `openjls.dtbo` at boot
   (`setup_bootargs.sh` installed it in `/boot` with a `uenvcmd` that patches
   it into the FIT's device tree before the kernel starts). It has to happen
   there: the DMA buffers live in `reserved-memory` carveouts, and the kernel
   only honors those from the device tree it *boots* with — a runtime
   (configfs) overlay would reserve nothing. Verify, then load the bitstream:

   ```sh
   ls /proc/device-tree/reserved-memory/       # expect ojls-desc@…, ojls-tx@…, ojls-rx@…

   cp system.bit.bin /lib/firmware/
   echo 0 > /sys/class/fpga_manager/fpga0/flags
   echo system.bit.bin > /sys/class/fpga_manager/fpga0/firmware
   cat /sys/class/fpga_manager/fpga0/state     # expect "operating"
   ```

   If the `ojls-*` nodes are missing, the boot hook didn't run (or fell back
   to a stock boot): re-run `setup_bootargs.sh` as root and reboot. The
   overlay's device nodes are inert until the PL is loaded — `generic-uio`
   and `u-dma-buf` don't touch the fabric at probe — so boot-time apply with
   the PL still unconfigured is fine.

3. **UIO**: the PYNQ image normally boots with
   `uio_pdrv_genirq.of_id=generic-uio` already on the kernel command line —
   check `grep generic-uio /proc/cmdline`. If it's missing, `setup_bootargs.sh`
   adds it to the bootargs,
   or `modprobe uio_pdrv_genirq of_id=generic-uio` when it's a module.
   Then:

   ```sh
   ls -l /dev/uio*
   cat /sys/class/uio/uio*/name    # among them, expect "openjls" and "dma"
   ```

4. **DMA buffers come from exclusive carveouts** — not CMA. The overlay's
   `reserved-memory` pools (`ojls-{tx,rx,desc}`, ~256 MiB total, `no-map`)
   are claimed at early boot before anything can touch or fragment them, so
   `u-dma-buf`'s allocations are deterministic at any uptime — there is
   nothing to free, unbind, or compact first. (Earlier revisions allocated
   from an enlarged CMA pool and fought the stock image for it each boot;
   see [Large images and the DMA carveout](#large-images-and-the-dma-carveout)
   for that history and the sizing rules.)

5. **u-dma-buf** — the DMA-buffer kernel module. The quickstart doesn't build
   this on the board (the board may have no internet, and a `.ko` is tied to one
   exact kernel anyway): a pre-built `u-dma-buf.ko` is committed next to this
   file and `board_setup.sh` just loads it. It was built against the PYNQ image's
   kernel, `6.6.10-xilinx-v2024.1-g3c0eca68c652` (its `vermagic`; check yours
   with `uname -r`). `insmod` refuses a module whose `vermagic` doesn't match the
   running kernel, so **rebuild it if you change the kernel or port to another
   board** — that's the only time you need this:

   ```sh
   # on a machine with this board's kernel headers (out-of-tree module)
   git clone https://github.com/ikwzm/udmabuf
   cd udmabuf && make          # -> u-dma-buf.ko ; commit/scp it in place
   ```

   Confirm it loaded: `ls /sys/class/u-dma-buf/` should list
   `udmabuf-ojls-{tx,rx,desc}`. If an entry is missing, its carveout wasn't in
   the boot device tree (step 2), or a node's `size` doesn't match its pool —
   see below.

6. **Run**:

   ```sh
   cd Software && make
   ./ojls_server
   ```

   The server verifies the `"OJLS"` ID register before opening the socket, so a
   successful start already proves the AXI-Lite path.

### From the host

```sh
./ojls_client <board-ip> image.pgm    # writes image.jls
```

The strongest end-to-end check: encode an image from the core's golden suite
(`ThirdParty/OpenJLS/Verification/Golden model`) and `cmp` the result against
its CharLS-generated `.jls` reference — OpenJLS is byte-exact against CharLS, so
the files must be identical. The [HIL sweep](../../Verification/README.md)
automates this across every precision.

### Undo / reload

```sh
# reload a new bitstream via fpga0/firmware at any time — the boot-time
# overlay is precision-independent and stays as is. To remove the overlay
# permanently, delete the uenvcmd line from /boot/uEnv.txt (and the dtbo
# from /boot) and reboot.
```

For swapping precisions on an already-set-up board (`u-dma-buf` stays
loaded, only the PL image and server cycle), the HIL sweep uses the lighter
`board_reload.sh` instead of a full `board_setup.sh`.

## Large images and the DMA carveout

With Scatter/Gather the DMA is no longer the limit, so the largest image the
board can encode is set by the two DMA buffers, which `u-dma-buf` takes from
dedicated **reserved-memory carveouts** — not from CMA. The admission rule the
server enforces is:

```
input  <= udmabuf-ojls-tx.size
output <= udmabuf-ojls-rx.size   (worst case ~ input + 25% + slack)
```

The overlay declares three `no-map` pools (`ojls-{tx,rx,desc}`) of
128 MiB (tx) + 128 MiB (rx) + 256 KiB (desc) ≈ **256 MiB**, so the encoder
accepts raw frames up to ~100 MiB (the rx output guard, worst case
raw + 25% + slack, binds before tx's 128 MiB) — comfortably past every image in
the golden corpus. Because the overlay is merged into the device tree by U-Boot
(`uenvcmd` in `uEnv.txt`) *before* the kernel starts, the spans are reserved
before any allocator runs: they are contiguous by construction, can never be
fragmented by zocl-drm or anything else, and allocate identically at any
uptime. No `cma=` tuning is needed — the stock `cma=128M` is untouched and
stays available to the rest of the system.

Confirm the reservation and the buffers after boot:

```sh
ls /proc/device-tree/reserved-memory/    # ojls-tx / ojls-rx / ojls-desc nodes
ls /sys/class/u-dma-buf/                 # udmabuf-ojls-{tx,rx,desc}
```

Each `u-dma-buf` node's `size` must exactly match its pool's `reg` length —
on a mismatch the probe fails and that `/sys/class/u-dma-buf` entry is simply
absent (check `dmesg | grep u-dma-buf`).

A second, sneakier failure mode: **U-Boot's own relocations**. By default
U-Boot relocates the (patched) DTB and any initrd to the *top of DDR* — i.e.
straight into `ojls-rx`, which ends at the 512 MiB top. The kernel then finds
live data overlapping the region at early boot, refuses to create it
(`of_reserved_mem_device_init failed. return=-22`), and only `udmabuf-ojls-rx`
is missing while tx/desc probe fine. The overlay itself is *present* under
`/proc/device-tree/reserved-memory/` — only the rx pool silently never became
a real reservation (visible as top-of-DDR entries in
`/sys/kernel/debug/memblock/reserved`). `setup_bootargs.sh` prevents this by
writing `fdt_high=0x0ffc0000` and `initrd_high=0x0ffc0000` into
`/boot/uEnv.txt` (`boot.scr` does a full `env import`, so they take effect
before `uenvcmd` runs), capping U-Boot's top-down relocation just below the
lowest carveout. If rx alone is missing after a boot-flow change, check those
two variables first.

**History.** Earlier revisions allocated these buffers from an enlarged CMA
pool (`cma=320M`) and had to unbind zocl-drm and compact memory each boot to
scrape together a contiguous 176 MiB run. The carveout design replaces all of
that; if you see `cma=320M` on an old SD card it can be reverted to stock.

**Pushing higher.** The ceiling is `DRAM − (Linux working set)`, split ~1 : 1.25
between tx and rx (the output guard). Each extra 100 MiB of image needs ~225 MiB
more carveout, taken directly out of Linux's 512 MiB. On a fully slimmed
userspace (~80–100 MiB) the board can reach roughly a **180–190 MiB** image
(tx 192 / rx 248 MiB). To change it: edit each pool's `reg` (base **and**
length — keep the pools adjacent and below the kernel's view of DDR) and the
matching `size` in the `u-dma-buf` nodes in `openjls.dtso`, recompile
(`dtc -@ -O dtb -o openjls.dtbo openjls.dtso`), copy the dtbo to `/boot`, and
reboot. Measure the real Linux floor with `free -m` under load first —
over-reserving will OOM the board at boot, not just fail an alloc.

## Porting to another board

The software is board-agnostic (see
[`../../Software/README.md`](../../Software/README.md) for its checklist); this
directory is what's PYNQ-Z2-specific. To bring the demo up on a different Zynq /
Zynq-MPSoC board, give it a sibling directory under `Hardware/` and adapt:

* **Part and board preset** — `build.tcl` sets `-part xc7z020clg400-1` and the
  `tul.com.tw:pynq-z2` board part; `design_encode_ethernet.tcl` bakes the PS7
  configuration (DDR, GEM, clocks) from that preset. Regenerate the PS block for
  your board's preset rather than hand-editing PCW values.
* **Fabric clock** — 50 MHz is this `-1` part's ceiling for the `byte_stuffer`
  path. A faster speed grade may close higher; a slower one may need less.
  `build.tcl` gates on WNS, so raising
  `PCW_*FPGA0_PERIPHERAL_FREQMHZ` and rebuilding will *tell you* if it doesn't
  close — trust that gate rather than shipping a violating bitstream.
* **DRAM and the carveouts** — the 128/128 MiB buffer sizes assume 512 MiB of
  DDR. Scale each pool's `reg` (base and length) and the matching `u-dma-buf`
  `size` fields in `openjls.dtso` to your board's DRAM and target image size
  (see the section above). More DDR means the pools can simply grow; there is
  no CMA sizing to coordinate.
* **Boot-time overlay application is the load-bearing part** — the carveouts
  only work because U-Boot merges the dtbo *before* the kernel boots
  (`uenvcmd` in `uEnv.txt`). A different board's boot flow (extlinux,
  distro-boot scripts, FIT images) needs an equivalent hook; applying the
  overlay at runtime via configfs is too late for `reserved-memory` and will
  quietly leave the pools unreserved. The boot loader's fdt/initrd
  relocations must also be kept out of the carveouts (`fdt_high` /
  `initrd_high`, see above) — especially for pools that end at the top of
  DDR, U-Boot's favourite relocation target.
* **UIO on the command line** — `uio_pdrv_genirq.of_id=generic-uio` must be in
  bootargs (or modprobed). Different images vary in whether it's built in.
* **Precision** — nothing precision-specific is board-specific; the per-BITNESS
  bitstream story (above) carries over unchanged.

What does *not* change: the block-design topology (encoder + AXI DMA in SG mode,
status stream off), the overlay's node structure, and the whole `Software/`
tree.
