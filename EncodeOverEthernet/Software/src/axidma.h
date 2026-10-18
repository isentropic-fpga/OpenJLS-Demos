/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* axidma.h: Xilinx AXI DMA (PG021) Scatter/Gather driver
 *
 * Completion comes from the descriptor status written back to DRAM, not from
 * the IOC bit: with a unit interrupt threshold, IOC fires on the first filled
 * S2MM descriptor, long before the encoder's TLAST. */

#ifndef AXIDMA_H
#define AXIDMA_H

#include <stdint.h>

#include "udmabuf.h"
#include "uio.h"

/* Register offsets (PG021). MM2S at 0x00, S2MM at 0x30. In SG mode the engine
 * is steered by descriptor pointers rather than a direct address/length. */
#define AXIDMA_MM2S 0x00u
#define AXIDMA_S2MM 0x30u

#define AXIDMA_CR(chan)          ((chan) + 0x00u) /* control                   */
#define AXIDMA_SR(chan)          ((chan) + 0x04u) /* status                    */
#define AXIDMA_CURDESC(chan)     ((chan) + 0x08u) /* first descriptor, 64B-aln */
#define AXIDMA_CURDESC_MSB(chan) ((chan) + 0x0Cu) /* upper 32 bits             */
#define AXIDMA_TAILDESC(chan)    ((chan) + 0x10u) /* last descriptor; starts   */
#define AXIDMA_TAILDESC_MSB(chan)((chan) + 0x14u) /* upper 32 bits             */

/* Control bits */
#define AXIDMA_CR_RS       (1u << 0)
#define AXIDMA_CR_RESET    (1u << 2)
#define AXIDMA_CR_IOC_IRQ  (1u << 12)
#define AXIDMA_CR_ERR_IRQ  (1u << 14)

/* Status bits */
#define AXIDMA_SR_HALTED   (1u << 0)
#define AXIDMA_SR_IDLE     (1u << 1)
#define AXIDMA_SR_INT_ERR  (1u << 4)
#define AXIDMA_SR_SLV_ERR  (1u << 5)
#define AXIDMA_SR_DEC_ERR  (1u << 6)
#define AXIDMA_SR_IOC      (1u << 12)
#define AXIDMA_SR_ERR      (1u << 14)
#define AXIDMA_SR_ANY_ERR  (AXIDMA_SR_INT_ERR | AXIDMA_SR_SLV_ERR | AXIDMA_SR_DEC_ERR | AXIDMA_SR_ERR)

/* One 64-byte SG descriptor (PG021 §"SG Descriptors"). 13 words are defined;
 * the struct is padded to the 64-byte alignment the next-pointer requires. */
struct axidma_desc {
    uint32_t nxtdesc;      /* 0x00: next descriptor phys (64B-aligned)         */
    uint32_t nxtdesc_msb;  /* 0x04                                             */
    uint32_t buffer_addr;  /* 0x08: data buffer phys                           */
    uint32_t buffer_msb;   /* 0x0C                                             */
    uint32_t rsvd0;        /* 0x10                                             */
    uint32_t rsvd1;        /* 0x14                                             */
    uint32_t control;      /* 0x18: [25:0] len, bit26 EOF, bit27 SOF (MM2S)    */
    uint32_t status;       /* 0x1C: [25:0] transferred, b26 RXEOF, b31 Cmplt   */
    uint32_t app[5];       /* 0x20..0x30: unused (no status/control stream)    */
    uint32_t pad[3];       /* pad to 64 bytes                                  */
};

/* Descriptor control/status bit fields */
#define AXIDMA_DESC_LEN_MASK 0x03FFFFFFu /* 26-bit buffer length / xfer count  */
#define AXIDMA_DESC_EOF      (1u << 26)  /* control: TLAST here / status: RXEOF */
#define AXIDMA_DESC_SOF      (1u << 27)  /* control: TFIRST                     */
#define AXIDMA_DESC_CMPLT    (1u << 31)  /* status: descriptor completed        */
#define AXIDMA_DESC_ERR_MASK (7u << 28)  /* status: Int/Slv/Dec error bits      */

/* Largest byte count one descriptor can carry (26-bit length register). We
 * chunk buffers well under it on a clean boundary for burst alignment. */
#define AXIDMA_DESC_CHUNK    (32u * 1024u * 1024u) /* 32 MiB per descriptor     */

/* A descriptor ring built over a slice of a u-dma-buf. Self-describing so the
 * wait can invalidate exactly the descriptor bytes it scans. */
struct axidma_ring {
    const struct udmabuf *db;    /* buffer the descriptors live in            */
    size_t              off;     /* byte offset of desc[0] within db          */
    struct axidma_desc *desc;    /* CPU pointer to desc[0]                    */
    uint64_t            phys;    /* bus address of desc[0]                    */
    int                 count;   /* descriptors built                        */
};

struct axidma {
    struct uio uio;
};

/* Open the DMA's UIO device (name substring or /dev/uioN) and reset the
 * engine. Returns 0 or -1. */
int  axidma_open(struct axidma *d, const char *name_or_path);
void axidma_close(struct axidma *d);

/* Soft-reset the whole engine (both channels). Returns 0, or -1 if the reset
 * bit never self-cleared. */
int axidma_reset(struct axidma *d);

/* Build a descriptor ring in the u-dma-buf region [db + desc_off, +desc_cap)
 * that maps the contiguous data buffer [buf_phys, buf_phys + len) in chunks of
 * at most AXIDMA_DESC_CHUNK. `frame` != 0 sets SOF on the first descriptor and
 * EOF on the last (use it for MM2S so the encoder sees one TLAST-framed
 * packet; leave 0 for S2MM). Returns 0, or -1 if the region cannot hold the
 * required descriptors. Descriptors are written but not yet synced. */
int axidma_ring_build(struct axidma_ring *ring, const struct udmabuf *db,
                      size_t desc_off, size_t desc_cap,
                      uint64_t buf_phys, uint64_t len, int frame);

/* Start `chan` processing `ring` (reset assumed done): program CURDESC, raise
 * RS, then write TAILDESC to kick. Returns 0 or -1. */
int axidma_sg_start(struct axidma *d, uint32_t chan, const struct axidma_ring *ring);

/* Wait for `ring` on `chan` to finish. Completion is a descriptor marked EOF
 * (S2MM: the encoder's TLAST) or the last descriptor completing (MM2S). The
 * ring's descriptor bytes are invalidated and scanned each poll; the status
 * register is checked for errors. Returns 0 on completion, -1 on DMA error
 * (decoded to stderr), -2 on timeout. On return 0 the ring is CPU-coherent so
 * axidma_ring_bytes() can be read straight away. */
int axidma_wait(struct axidma *d, uint32_t chan, struct axidma_ring *ring, int timeout_ms);

/* Total bytes the DMA moved for `ring`: the sum of per-descriptor counts up to
 * and including the EOF descriptor. Valid after axidma_wait() returns 0. */
uint32_t axidma_ring_bytes(const struct axidma_ring *ring);

#endif /* AXIDMA_H */
