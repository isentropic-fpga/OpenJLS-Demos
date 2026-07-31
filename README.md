# OpenJLS-Demos

Demo projects for [OpenJLS](https://github.com/isentropic-fpga/OpenJLS), the
open-source JPEG-LS (ITU-T T.87) hardware encoder. Each demo lives in its own
directory and carries a step-by-step reproduction guide; the encoder core is
shared by all of them as a submodule, pinned at the verified commit.

Core specs, benchmarks and licensing: **[isentropic.com.br/openjls](https://isentropic.com.br/openjls)**

## Encode over Ethernet

[`EncodeOverEthernet/`](EncodeOverEthernet/README.md) — a complete image
compression appliance on an FPGA board. A host sends raw images over TCP; the
board encodes them to JPEG-LS **entirely in the fabric** and sends the `.jls`
files back. The CPU on the board never touches a pixel: it only moves bytes
between the socket and an AXI DMA, and the encode is 100% the OpenJLS core.

```
┌─ host ───────┐            ┌─ board (PS) ─┐                     ┌─ board (PL) ──────┐
│              │─── TCP ───►│              │─── AXI DMA MM2S ───►│                   │
│ ojls_client  │            │ ojls_server  │◄─── AXI DMA S2MM ───│ openjls_axis_regs │
│              │◄─── TCP ───│              │◄──── AXI-Lite ─────►│                   │
│              │   (.jls)   │              │ dims, apply, status │                   │
└──────────────┘            └──────────────┘                     └───────────────────┘
```

A PYNQ-Z2 (Zynq-7020) build ships prebuilt — bitstreams for every supported
pixel depth, the device-tree overlay, and the DMA kernel module — so the demo
runs without Vivado or any FPGA toolchain. The board-side software reaches the
hardware only through generic Linux interfaces (UIO and u-dma-buf), so porting
to another board is a block-design and device-tree job, not a software one.

### Outcome

Verified in hardware on a PYNQ-Z2. Across the full verification corpus —
**287 images spanning every supported pixel depth (8–16 bits)** — the FPGA
output is **byte-exact against the CharLS reference encoder**, with zero
mismatches. Losslessly compressed, the 581 MB corpus comes back as 356 MB
(1.63:1 overall; the ratio is content-dependent).

The hardware-in-the-loop sweep that produces these numbers is part of the demo
and is fully reproducible:
[`EncodeOverEthernet/README.md`](EncodeOverEthernet/README.md) walks from a
clean clone to a verified board.

## Repository layout

| Path | What |
|---|---|
| [`EncodeOverEthernet/`](EncodeOverEthernet/README.md) | The demo above: software, PYNQ-Z2 hardware build, and verification. Other boards get sibling directories under its `Hardware/`. |
| `ThirdParty/OpenJLS` | The encoder core, as a submodule — shared by every demo, and the owner of the image corpus and reference encoder the verification uses. |

## About

OpenJLS and these demos are developed and maintained by
[Isentropic](https://isentropic.com.br), an FPGA engineering company.
For commercial licensing, technical questions, or collaboration inquiries:
[isentropic.com.br/contact](https://isentropic.com.br/contact) or
contact@isentropic.com.br
