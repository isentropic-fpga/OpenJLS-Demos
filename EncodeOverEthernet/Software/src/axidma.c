/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* axidma.c: Xilinx AXI DMA (PG021) Scatter/Gather driver */

#include "axidma.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void msleep_short(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 }; /* 100 us */

    nanosleep(&ts, NULL);
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int axidma_open(struct axidma *d, const char *name_or_path)
{
    memset(d, 0, sizeof(*d));

    if (uio_open(&d->uio, name_or_path) != 0)
        return -1;

    if (axidma_reset(d) != 0) {
        uio_close(&d->uio);
        return -1;
    }

    if (uio_irq_enable(&d->uio) == 0)
        fprintf(stderr, "axidma: %s: interrupt available, waits will sleep on it\n", d->uio.name);
    else
        fprintf(stderr, "axidma: %s: no interrupt wired, waits will poll\n", d->uio.name);

    return 0;
}

void axidma_close(struct axidma *d)
{
    uio_close(&d->uio);
}

int axidma_reset(struct axidma *d)
{
    const int64_t deadline = now_ms() + 100;

    /* Either channel's reset bit resets the whole engine (PG021). */
    uio_wr(&d->uio, AXIDMA_CR(AXIDMA_MM2S), AXIDMA_CR_RESET);
    uio_wr(&d->uio, AXIDMA_CR(AXIDMA_S2MM), AXIDMA_CR_RESET);

    while ((uio_rd(&d->uio, AXIDMA_CR(AXIDMA_MM2S)) & AXIDMA_CR_RESET) != 0 ||
           (uio_rd(&d->uio, AXIDMA_CR(AXIDMA_S2MM)) & AXIDMA_CR_RESET) != 0) {
        if (now_ms() > deadline) {
            fprintf(stderr, "axidma: reset did not complete — check that the DMA clock is running\n");
            return -1;
        }
        msleep_short();
    }

    return 0;
}

int axidma_ring_build(struct axidma_ring *ring, const struct udmabuf *db,
                      size_t desc_off, size_t desc_cap,
                      uint64_t buf_phys, uint64_t len, int frame)
{
    uint64_t nchunks, remaining, addr;

    if (len == 0)
        return -1;
    if ((desc_off & 0x3Fu) != 0 || ((db->phys + desc_off) & 0x3Fu) != 0) {
        fprintf(stderr, "axidma: descriptor region not 64-byte aligned\n");
        return -1;
    }

    nchunks = (len + AXIDMA_DESC_CHUNK - 1) / AXIDMA_DESC_CHUNK;
    if (nchunks * sizeof(struct axidma_desc) > desc_cap) {
        fprintf(stderr, "axidma: descriptor region too small for %" PRIu64 " descriptors\n", nchunks);
        return -1;
    }

    ring->db    = db;
    ring->off   = desc_off;
    ring->desc  = (struct axidma_desc *)(db->mem + desc_off);
    ring->phys  = db->phys + desc_off;
    ring->count = (int)nchunks;

    remaining = len;
    addr      = buf_phys;
    for (int i = 0; i < ring->count; i++) {
        struct axidma_desc *dsc  = &ring->desc[i];
        const uint32_t      chunk = (remaining > AXIDMA_DESC_CHUNK)
                                        ? AXIDMA_DESC_CHUNK : (uint32_t)remaining;
        /* Non-cyclic: the engine stops at TAILDESC regardless, so the last
         * next-pointer wrapping back to desc[0] is harmless. */
        const uint64_t next = (i == ring->count - 1)
                                  ? ring->phys
                                  : ring->phys + (uint64_t)(i + 1) * sizeof(struct axidma_desc);
        uint32_t ctrl = chunk & AXIDMA_DESC_LEN_MASK;

        memset(dsc, 0, sizeof(*dsc));
        dsc->nxtdesc     = (uint32_t)next;
        dsc->nxtdesc_msb = (uint32_t)(next >> 32);
        dsc->buffer_addr = (uint32_t)addr;
        dsc->buffer_msb  = (uint32_t)(addr >> 32);
        if (frame) {
            if (i == 0)
                ctrl |= AXIDMA_DESC_SOF;
            if (i == ring->count - 1)
                ctrl |= AXIDMA_DESC_EOF;
        }
        dsc->control = ctrl;

        addr      += chunk;
        remaining -= chunk;
    }

    return 0;
}

int axidma_sg_start(struct axidma *d, uint32_t chan, const struct axidma_ring *ring)
{
    uint64_t first, tail;

    if (ring->count <= 0)
        return -1;

    first = ring->phys;
    tail  = ring->phys + (uint64_t)(ring->count - 1) * sizeof(struct axidma_desc);

    /* CURDESC is latched while the engine is halted (caller has reset it),
     * then RS starts it and the TAILDESC write kicks off descriptor fetch. */
    uio_wr(&d->uio, AXIDMA_CURDESC(chan), (uint32_t)first);
    uio_wr(&d->uio, AXIDMA_CURDESC_MSB(chan), (uint32_t)(first >> 32));

    uio_wr(&d->uio, AXIDMA_CR(chan), AXIDMA_CR_RS | AXIDMA_CR_IOC_IRQ | AXIDMA_CR_ERR_IRQ);
    uio_wr(&d->uio, AXIDMA_SR(chan), AXIDMA_SR_IOC | AXIDMA_SR_ERR);

    uio_wr(&d->uio, AXIDMA_TAILDESC_MSB(chan), (uint32_t)(tail >> 32));
    uio_wr(&d->uio, AXIDMA_TAILDESC(chan), (uint32_t)tail);

    return 0;
}

int axidma_wait(struct axidma *d, uint32_t chan, struct axidma_ring *ring, int timeout_ms)
{
    const int64_t deadline = now_ms() + timeout_ms;
    const size_t  nbytes   = (size_t)ring->count * sizeof(struct axidma_desc);
    const char   *cname    = (chan == AXIDMA_MM2S) ? "MM2S" : "S2MM";

    for (;;) {
        const uint32_t sr = uio_rd(&d->uio, AXIDMA_SR(chan));
        int            eof = 0;
        uint32_t       last;

        if ((sr & AXIDMA_SR_ANY_ERR) != 0) {
            fprintf(stderr, "axidma: %s error on %s: SR=0x%08x%s%s%s\n",
                    d->uio.name, cname, sr,
                    (sr & AXIDMA_SR_INT_ERR) ? " [internal: TLAST missing / early?]" : "",
                    (sr & AXIDMA_SR_SLV_ERR) ? " [slave]" : "",
                    (sr & AXIDMA_SR_DEC_ERR) ? " [decode: bad descriptor/buffer address?]" : "");
            return -1;
        }

        if (udmabuf_sync_for_cpu(ring->db, ring->off, nbytes) != 0)
            return -1;

        for (int i = 0; i < ring->count; i++) {
            const uint32_t st = ring->desc[i].status;

            if ((st & AXIDMA_DESC_ERR_MASK) != 0) {
                fprintf(stderr, "axidma: %s descriptor %d error: status=0x%08x\n", cname, i, st);
                return -1;
            }
            if ((st & AXIDMA_DESC_EOF) != 0) { /* RXEOF: encoder raised TLAST here */
                eof = 1;
                break;
            }
        }

        last = ring->desc[ring->count - 1].status;
        if (chan == AXIDMA_S2MM) {
            if (eof)
                return 0;
            /* All descriptors done without TLAST: rx buffer overrun. */
            if ((last & AXIDMA_DESC_CMPLT) != 0) {
                fprintf(stderr, "axidma: S2MM filled every descriptor without TLAST — "
                        "encoded output exceeded the rx buffer\n");
                return -1;
            }
        } else {
            if ((last & AXIDMA_DESC_CMPLT) != 0)
                return 0;
        }

        if (now_ms() > deadline)
            return -2;

        if (d->uio.irq_ok)
            uio_irq_wait(&d->uio, 10);
        else
            msleep_short();
    }
}

uint32_t axidma_ring_bytes(const struct axidma_ring *ring)
{
    uint32_t total = 0;

    for (int i = 0; i < ring->count; i++) {
        const uint32_t st = ring->desc[i].status;

        total += st & AXIDMA_DESC_LEN_MASK;
        if ((st & AXIDMA_DESC_EOF) != 0)
            break;
    }
    return total;
}
