/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* uio.c: minimal UIO access */

#include "uio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Read the first line of a small sysfs attribute into buf (NUL-terminated,
 * trailing newline stripped). Returns 0 or -1. */
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

/* Find /dev/uioN whose sysfs name contains `name`. */
static int uio_find(const char *name, char *dev, size_t dev_len, char *sysname, size_t sysname_len)
{
    DIR *dir = opendir("/sys/class/uio");
    const struct dirent *ent;
    int found = -1;

    if (dir == NULL) {
        fprintf(stderr, "uio: cannot open /sys/class/uio: %s (is the UIO driver loaded?)\n",
                strerror(errno));
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        char path[300];
        char attr[128];

        if (strncmp(ent->d_name, "uio", 3) != 0)
            continue;
        snprintf(path, sizeof(path), "/sys/class/uio/%s/name", ent->d_name);
        if (sysfs_read_line(path, attr, sizeof(attr)) != 0)
            continue;
        if (strstr(attr, name) != NULL) {
            if (snprintf(dev, dev_len, "/dev/%s", ent->d_name) >= (int)dev_len ||
                snprintf(sysname, sysname_len, "%s", attr) >= (int)sysname_len)
                continue; /* absurdly long name — not one of ours */
            found = 0;
            break;
        }
    }
    closedir(dir);

    if (found != 0)
        fprintf(stderr, "uio: no device matching \"%s\" in /sys/class/uio\n", name);
    return found;
}

int uio_open(struct uio *u, const char *name_or_path)
{
    char size_path[300];
    char attr[64];

    memset(u, 0, sizeof(*u));
    u->fd = -1;

    if (strncmp(name_or_path, "/dev/", 5) == 0) {
        snprintf(u->dev, sizeof(u->dev), "%s", name_or_path);
        snprintf(u->name, sizeof(u->name), "%s", name_or_path + 5);
    } else if (uio_find(name_or_path, u->dev, sizeof(u->dev), u->name, sizeof(u->name)) != 0) {
        return -1;
    }

    snprintf(size_path, sizeof(size_path), "/sys/class/uio/%s/maps/map0/size", u->dev + 5);
    if (sysfs_read_line(size_path, attr, sizeof(attr)) != 0) {
        fprintf(stderr, "uio: cannot read %s: %s\n", size_path, strerror(errno));
        return -1;
    }
    u->map_size = (size_t)strtoul(attr, NULL, 0);

    u->fd = open(u->dev, O_RDWR | O_SYNC);
    if (u->fd < 0) {
        fprintf(stderr, "uio: cannot open %s: %s\n", u->dev, strerror(errno));
        return -1;
    }

    /* Map N of a UIO device sits at offset N * page_size. Map 0 -> offset 0. */
    u->regs = mmap(NULL, u->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, u->fd, 0);
    if (u->regs == MAP_FAILED) {
        fprintf(stderr, "uio: mmap of %s failed: %s\n", u->dev, strerror(errno));
        close(u->fd);
        u->fd = -1;
        return -1;
    }

    return 0;
}

void uio_close(struct uio *u)
{
    if (u->regs != NULL && u->regs != MAP_FAILED)
        munmap((void *)u->regs, u->map_size);
    if (u->fd >= 0)
        close(u->fd);
    u->regs = NULL;
    u->fd   = -1;
}

int uio_irq_enable(struct uio *u)
{
    const uint32_t one = 1;

    if (write(u->fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
        return -1;
    u->irq_ok = 1;
    return 0;
}

int uio_irq_wait(struct uio *u, int timeout_ms)
{
    struct pollfd pfd = { .fd = u->fd, .events = POLLIN };
    uint32_t count;
    int rc;

    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0)
        return -1;
    if (rc == 0)
        return 0;

    if (read(u->fd, &count, sizeof(count)) != (ssize_t)sizeof(count))
        return -1;
    /* Level-triggered sources need re-enabling after each event. */
    uio_irq_enable(u);
    return 1;
}
