/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* udmabuf.h: u-dma-buf (https://github.com/ikwzm/udmabuf) buffer access */

#ifndef UDMABUF_H
#define UDMABUF_H

#include <stddef.h>
#include <stdint.h>

struct udmabuf {
    int      fd;
    uint8_t *mem;
    size_t   size;
    uint64_t phys;
    char     name[64];
};

/* Open by exact device name. Returns 0 or -1 (message on stderr). */
int udmabuf_open(struct udmabuf *b, const char *name);

/* Open the index-th device (alphabetical) in /sys/class/u-dma-buf. */
int udmabuf_open_nth(struct udmabuf *b, int index);

/* Number of u-dma-buf devices present. */
int udmabuf_count(void);

void udmabuf_close(struct udmabuf *b);

/* Cache maintenance before/after DMA. offset/len select the region.
 * _for_device: CPU wrote the data, DMA will read it (flush).
 * _for_cpu:    DMA wrote the data, CPU will read it (invalidate).
 * Return 0 or -1. */
int udmabuf_sync_for_device(const struct udmabuf *b, size_t offset, size_t len);
int udmabuf_sync_for_cpu(const struct udmabuf *b, size_t offset, size_t len);

#endif /* UDMABUF_H */
