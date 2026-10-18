/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* udmabuf.c: u-dma-buf buffer access */

#include "udmabuf.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define UDMABUF_CLASS "/sys/class/u-dma-buf"

static int sysfs_read_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");

    if (f == NULL)
        return -1;
    if (fgets(buf, (int)len, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int attr_write(const char *name, const char *attr, unsigned long value)
{
    char path[300];
    FILE *f;
    int rc;

    snprintf(path, sizeof(path), UDMABUF_CLASS "/%s/%s", name, attr);
    f = fopen(path, "w");
    if (f == NULL) {
        fprintf(stderr, "udmabuf: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    rc = (fprintf(f, "%lu", value) > 0) ? 0 : -1;
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

static int name_filter(const struct dirent *ent)
{
    return ent->d_name[0] != '.';
}

int udmabuf_count(void)
{
    struct dirent **list;
    int n = scandir(UDMABUF_CLASS, &list, name_filter, alphasort);

    if (n < 0)
        return 0;
    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
    return n;
}

int udmabuf_open_nth(struct udmabuf *b, int index)
{
    struct dirent **list;
    int n = scandir(UDMABUF_CLASS, &list, name_filter, alphasort);
    int rc = -1;

    if (n < 0) {
        fprintf(stderr, "udmabuf: cannot scan " UDMABUF_CLASS ": %s (is u-dma-buf loaded?)\n",
                strerror(errno));
        return -1;
    }
    if (index < n)
        rc = udmabuf_open(b, list[index]->d_name);
    else
        fprintf(stderr, "udmabuf: device index %d out of range (%d present)\n", index, n);

    for (int i = 0; i < n; i++)
        free(list[i]);
    free(list);
    return rc;
}

int udmabuf_open(struct udmabuf *b, const char *name)
{
    char path[300];
    char attr[64];

    memset(b, 0, sizeof(*b));
    b->fd = -1;
    snprintf(b->name, sizeof(b->name), "%s", name);

    snprintf(path, sizeof(path), UDMABUF_CLASS "/%s/phys_addr", name);
    if (sysfs_read_line(path, attr, sizeof(attr)) != 0) {
        fprintf(stderr, "udmabuf: cannot read %s: %s (is u-dma-buf loaded?)\n",
                path, strerror(errno));
        return -1;
    }
    b->phys = strtoull(attr, NULL, 0);

    snprintf(path, sizeof(path), UDMABUF_CLASS "/%s/size", name);
    if (sysfs_read_line(path, attr, sizeof(attr)) != 0) {
        fprintf(stderr, "udmabuf: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    b->size = (size_t)strtoull(attr, NULL, 0);

    snprintf(path, sizeof(path), "/dev/%s", name);
    b->fd = open(path, O_RDWR);
    if (b->fd < 0) {
        fprintf(stderr, "udmabuf: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Cached mapping; ownership is handed over with the sync_* calls. */
    b->mem = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->mem == MAP_FAILED) {
        fprintf(stderr, "udmabuf: mmap of %s failed: %s\n", path, strerror(errno));
        close(b->fd);
        b->fd = -1;
        return -1;
    }

    return 0;
}

void udmabuf_close(struct udmabuf *b)
{
    if (b->mem != NULL && b->mem != MAP_FAILED)
        munmap(b->mem, b->size);
    if (b->fd >= 0)
        close(b->fd);
    b->mem = NULL;
    b->fd  = -1;
}

/* u-dma-buf sync protocol: stage offset/size/direction, then kick the sync.
 * Directions follow dma_data_direction: 1 = DMA_TO_DEVICE, 2 = DMA_FROM_DEVICE. */
static int udmabuf_sync(const struct udmabuf *b, size_t offset, size_t len, int dir, const char *kick)
{
    if (attr_write(b->name, "sync_offset", offset) != 0 ||
        attr_write(b->name, "sync_size", len) != 0 ||
        attr_write(b->name, "sync_direction", (unsigned long)dir) != 0 ||
        attr_write(b->name, kick, 1) != 0)
        return -1;
    return 0;
}

int udmabuf_sync_for_device(const struct udmabuf *b, size_t offset, size_t len)
{
    return udmabuf_sync(b, offset, len, 1, "sync_for_device");
}

int udmabuf_sync_for_cpu(const struct udmabuf *b, size_t offset, size_t len)
{
    return udmabuf_sync(b, offset, len, 2, "sync_for_cpu");
}
