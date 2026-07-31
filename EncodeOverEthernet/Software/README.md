# Software — `ojls_server` (board) and `ojls_client` (host)

> Reference for the two programs of the [EncodeOverEthernet](../README.md)
> demo. That README is the one to follow to actually run them.

Plain C, no dependencies, no vendor libraries. The server reaches the hardware
only through generic Linux interfaces — UIO for the register banks,
[u-dma-buf](https://github.com/ikwzm/udmabuf) for the DMA buffers — so nothing
here is board-specific: point it at your UIO/u-dma-buf names and it runs.

```
host                                  board
ojls_client ──── TCP :19020 ────► ojls_server ──► UIO regs + AXI DMA ──► PL
            ◄─── .jls bytes ─────             ◄── S2MM + IRQ ──────────
```

| Path | What |
|---|---|
| `host/ojls_client.c` | the client: reads a PGM, sends it, writes the `.jls` |
| `src/ojls_server.c` | the server: accepts, DMAs, replies |
| `src/uio.c`, `src/udmabuf.c`, `src/axidma.c` | the thin Linux-side layers: UIO mapping + IRQ wait, u-dma-buf discovery, AXI DMA Scatter/Gather ring |
| `src/ojls_regs.h` | `openjls_axis_regs` register map — must match `ThirdParty/OpenJLS/Sources/axi/openjls_axis_regs.vhd` |
| `common/ojls_proto.h` | the wire protocol, shared by both sides |

## Build

```sh
make                                          # native (on the board, or the host)
make CROSS_COMPILE=arm-linux-gnueabihf-       # cross, 32-bit ARM (Zynq-7000)
make CROSS_COMPILE=aarch64-linux-gnu-         # cross, 64-bit ARM (Zynq UltraScale+)
```

Builds both binaries; `make ojls_client` alone is the usual host build.

## Running

```
ojls_server [options]
  -p PORT          TCP port (default 19020)
  --regs NAME      UIO device of the openjls_axis_regs bank (default "openjls")
  --dma NAME       UIO device of the AXI DMA (default "dma")
  --tx-buf NAME    u-dma-buf for pixels in (default "udmabuf-ojls-tx")
  --rx-buf NAME    u-dma-buf for bitstream out (default "udmabuf-ojls-rx")
  --desc-buf NAME  u-dma-buf for the SG descriptor rings (default "udmabuf-ojls-desc")
  --timeout MS     per-image encode timeout (default 10000)
  --loopback       no hardware; echo payloads back (protocol test)
```

Serves one connection at a time — the encoder is a single physical resource.
`--loopback` lets you exercise the protocol, the client, and the network on any
Linux machine, with no FPGA involved.

```
ojls_client [options] HOST INPUT.pgm [OUTPUT.jls]
  -p PORT     TCP port (default 19020)
  -b BITS     override bitness (default: derived from the PGM maxval)
  -n COUNT    send the image COUNT times (throughput test, default 1)
```

Input is binary PGM (`P5`), grayscale, 8..16 bpp. Bitness comes from the maxval
(255→8, 4095→12, 65535→16) and **must match the bitstream loaded on the
board** — `BITNESS` is baked in at synthesis. A mismatch is refused cleanly with
`OJLS_ST_BAD_BITNESS` rather than silently producing garbage. Default output is
the input path with a `.jls` suffix.

## Wire protocol

`common/ojls_proto.h` is the normative definition; this is the shape of it.
A fixed 20-byte **little-endian** header — packed and parsed byte by byte, so
it's independent of host endianness and struct padding — then the payload:

| Offset | Field | Bytes | Notes |
|---|---|---|---|
| 0 | magic | 4 | `0x4F4A4C53` — `"OJLS"` |
| 4 | version | 2 | `1` |
| 6 | type | 1 | request `OJLS_MSG_ENCODE_REQ` / response `OJLS_MSG_ENCODE_RESP` |
| 7 | status | 1 | `OJLS_ST_OK` or an error; response only |
| 8 | width | 2 | pixels |
| 10 | height | 2 | pixels |
| 12 | bitness | 1 | 8..16 |
| 13 | reserved | 3 | zero |
| 16 | payload_len | 4 | raw pixels on the way in, `.jls` bytes on the way back |

Request payload is the raw pixel array, row-major: 1 byte per pixel at 8 bpp,
otherwise 2 bytes, right-justified little-endian. (PGM stores 16-bit samples
big-endian, so the client byte-swaps.) The response payload is the complete
JPEG-LS stream.
Errors come back as a header with `payload_len = 0` and a status naming the
cause: `OJLS_ST_BAD_MAGIC`, `OJLS_ST_BAD_VERSION`, `OJLS_ST_BAD_TYPE`,
`OJLS_ST_BAD_BITNESS`, `OJLS_ST_BAD_DIMS`, `OJLS_ST_TOO_LARGE`,
`OJLS_ST_SIZE_MISMATCH`, `OJLS_ST_HW_TIMEOUT`, `OJLS_ST_HW_ERROR`. The
connection stays open — one bad image doesn't end the session.

## Porting to another board

Nothing here needs to change. What has to be true on the target:

* UIO nodes for the register bank and the AXI DMA (`compatible = "generic-uio"`
  in the device tree, `uio_pdrv_genirq.of_id=generic-uio` on the kernel command
  line), passed to `--regs` / `--dma` by name.
* Three `u-dma-buf` regions, passed by name to `--tx-buf` / `--rx-buf` /
  `--desc-buf`. **Always pass them explicitly** — auto-discovery picks
  alphabetically and can swap the tx/rx roles.
* An image that fits: tx buffer ≥ the raw pixels, rx buffer ≥ the worst-case
  stream. Oversized requests are refused with `OJLS_ST_TOO_LARGE`.

The PYNQ-Z2 reference wiring of all of the above — addresses, sizes, and the
overlay that creates it — is in
[`../Hardware/pynq-z2/INTERNALS.md`](../Hardware/pynq-z2/INTERNALS.md).
