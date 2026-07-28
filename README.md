# OpenJLS-Demos

Demo projects for [OpenJLS](https://github.com/isentropic-fpga/OpenJLS), the
open-source JPEG-LS (ITU-T T.87) hardware encoder. Each demo lives in its own
directory with a self-contained, end-to-end README — software, hardware
bring-up, and verification; the encoder core is shared by all of them as a
submodule pinned at the verified commit.

Core specs, benchmarks and licensing: **[isentropic.com.br/openjls](https://isentropic.com.br/openjls)**

## Projects

| Path | Project |
|---|---|
| [`EncodeOverEthernet/`](EncodeOverEthernet/README.md) | Stream raw images to a board over Ethernet, encode them 100% in the FPGA, stream the `.jls` files back. A PYNQ-Z2 build is provided; other boards get sibling directories. |
| `ThirdParty/OpenJLS` | The encoder core (submodule), used by every project. |

## Encode over Ethernet
### Results

Verified in hardware on a PYNQ-Z2: across the full verification corpus —
**287 images spanning every supported pixel depth (8–16 bits)** — the FPGA
output is **byte-exact against the CharLS reference encoder**, with zero
mismatches. Losslessly compressed, the 581 MB corpus comes back as 356 MB
(1.63:1 overall; ratio is content-dependent).

The hardware-in-the-loop sweep that produces these numbers is fully
reproducible — see
[`EncodeOverEthernet/README.md`](EncodeOverEthernet/README.md), which walks
from a clean clone to a verified board.

## About

OpenJLS and these demos are developed and maintained by
[Isentropic](https://isentropic.com.br), an FPGA engineering company.
For commercial licensing, technical questions, or collaboration inquiries:
[isentropic.com.br/contact](https://isentropic.com.br/contact) or
contact@isentropic.com.br
