/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* ojls_client.c: host client; sends a PGM to the board, writes the .jls */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ojls_proto.h"

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int recv_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t   got = 0;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);

        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static int send_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t         sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Skip whitespace and '#' comments, then parse one unsigned decimal. */
static int pgm_token(FILE *f, unsigned long *out)
{
    int c;

    for (;;) {
        c = fgetc(f);
        if (c == '#') {
            do {
                c = fgetc(f);
            } while (c != '\n' && c != EOF);
        } else if (!isspace(c)) {
            break;
        }
    }
    if (c == EOF || !isdigit(c))
        return -1;

    *out = 0;
    do {
        *out = *out * 10 + (unsigned long)(c - '0');
        c = fgetc(f);
    } while (isdigit(c));
    /* The terminating whitespace is consumed, as PGM requires after maxval. */
    return 0;
}

struct image {
    uint16_t width;
    uint16_t height;
    uint8_t  bitness;
    uint32_t bytes;   /* payload size on the wire */
    uint8_t *pixels;  /* wire format: LE, 1 or 2 bytes/pixel */
};

static int pgm_load(const char *path, struct image *img)
{
    FILE         *f = fopen(path, "rb");
    unsigned long w, h, maxval;
    size_t        npix;
    int           c1, c2;

    if (f == NULL) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }

    c1 = fgetc(f);
    c2 = fgetc(f);
    if (c1 != 'P' || c2 != '5') {
        fprintf(stderr, "%s: not a binary PGM (P5)\n", path);
        fclose(f);
        return -1;
    }

    if (pgm_token(f, &w) != 0 || pgm_token(f, &h) != 0 || pgm_token(f, &maxval) != 0 ||
        w == 0 || h == 0 || w > 65535 || h > 65535 || maxval == 0 || maxval > 65535) {
        fprintf(stderr, "%s: malformed PGM header\n", path);
        fclose(f);
        return -1;
    }

    img->width  = (uint16_t)w;
    img->height = (uint16_t)h;
    img->bitness = 8; /* hardware minimum — low maxvals still encode as 8-bit */
    while (((1ul << img->bitness) - 1) < maxval)
        img->bitness++;

    npix = (size_t)w * h;
    img->bytes  = (uint32_t)(npix * ((maxval > 255) ? 2 : 1));
    img->pixels = malloc(img->bytes);
    if (img->pixels == NULL) {
        fclose(f);
        return -1;
    }

    if (fread(img->pixels, 1, img->bytes, f) != img->bytes) {
        fprintf(stderr, "%s: truncated pixel data\n", path);
        free(img->pixels);
        fclose(f);
        return -1;
    }
    fclose(f);

    /* PGM is big-endian, the wire little-endian. */
    if (maxval > 255) {
        for (size_t i = 0; i < npix; i++) {
            const uint8_t hi = img->pixels[2 * i];

            img->pixels[2 * i]     = img->pixels[2 * i + 1];
            img->pixels[2 * i + 1] = hi;
        }
    }

    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options] HOST INPUT.pgm [OUTPUT.jls]\n"
            "  -p PORT     TCP port (default %d)\n"
            "  -b BITS     override bitness (default: derived from PGM maxval)\n"
            "  -n COUNT    send the image COUNT times (throughput test, default 1)\n"
            "Default OUTPUT is INPUT with a .jls suffix.\n",
            argv0, OJLS_PROTO_PORT);
}

int main(int argc, char **argv)
{
    const char     *host = NULL, *in_path = NULL, *out_path = NULL;
    int             port = OJLS_PROTO_PORT, bitness_ovr = 0, count = 1;
    char            out_buf[4096];
    struct image    img;
    struct addrinfo hints, *ai = NULL;
    char            port_str[16];
    int             fd = -1, yes = 1, rc = 1;
    uint8_t        *resp_payload = NULL;
    uint32_t        resp_len = 0;
    int64_t         t0, t_total;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            bitness_ovr = atoi(argv[++i]);
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            count = atoi(argv[++i]);
        else if (argv[i][0] == '-') {
            usage(argv[0]);
            return (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) ? 0 : 1;
        } else if (host == NULL)
            host = argv[i];
        else if (in_path == NULL)
            in_path = argv[i];
        else
            out_path = argv[i];
    }
    if (host == NULL || in_path == NULL || count < 1) {
        usage(argv[0]);
        return 1;
    }
    if (out_path == NULL) {
        const char *dot = strrchr(in_path, '.');
        const size_t stem = (dot != NULL) ? (size_t)(dot - in_path) : strlen(in_path);

        snprintf(out_buf, sizeof(out_buf), "%.*s.jls", (int)stem, in_path);
        out_path = out_buf;
    }

    if (pgm_load(in_path, &img) != 0)
        return 1;
    if (bitness_ovr != 0) {
        if (bitness_ovr < img.bitness || bitness_ovr > 16 ||
            (img.bytes / ((uint32_t)img.width * img.height) == 1 && bitness_ovr > 8)) {
            fprintf(stderr, "-b %d incompatible with %u-bit samples in %s\n",
                    bitness_ovr, img.bitness, in_path);
            free(img.pixels);
            return 1;
        }
        img.bitness = (uint8_t)bitness_ovr;
    }
    printf("%s: %ux%u, %u bpp, %u bytes raw\n",
           in_path, img.width, img.height, img.bitness, img.bytes);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &ai) != 0 || ai == NULL) {
        fprintf(stderr, "cannot resolve %s\n", host);
        goto out;
    }

    for (const struct addrinfo *a = ai; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s:%d: %s\n", host, port, strerror(errno));
        goto out;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    t_total = 0;
    for (int i = 0; i < count; i++) {
        struct ojls_hdr req = {
            .magic       = OJLS_PROTO_MAGIC,
            .version     = OJLS_PROTO_VERSION,
            .type        = OJLS_MSG_ENCODE_REQ,
            .status      = 0,
            .width       = img.width,
            .height      = img.height,
            .bitness     = img.bitness,
            .payload_len = img.bytes,
        };
        struct ojls_hdr resp;
        uint8_t         hdr_raw[OJLS_PROTO_HDR_LEN];

        ojls_hdr_pack(hdr_raw, &req);

        t0 = now_us();
        if (send_full(fd, hdr_raw, sizeof(hdr_raw)) != 0 ||
            send_full(fd, img.pixels, img.bytes) != 0 ||
            recv_full(fd, hdr_raw, sizeof(hdr_raw)) != 0) {
            fprintf(stderr, "connection lost\n");
            goto out;
        }
        ojls_hdr_unpack(&resp, hdr_raw);

        if (resp.magic != OJLS_PROTO_MAGIC || resp.type != OJLS_MSG_ENCODE_RESP) {
            fprintf(stderr, "malformed response\n");
            goto out;
        }
        if (resp.status != OJLS_ST_OK) {
            fprintf(stderr, "server rejected the image: %s\n", ojls_status_str(resp.status));
            goto out;
        }

        if (resp_payload == NULL || resp_len < resp.payload_len) {
            free(resp_payload);
            resp_payload = malloc(resp.payload_len);
            if (resp_payload == NULL)
                goto out;
        }
        resp_len = resp.payload_len;
        if (recv_full(fd, resp_payload, resp_len) != 0) {
            fprintf(stderr, "connection lost mid-payload\n");
            goto out;
        }
        t_total += now_us() - t0;
    }

    {
        FILE *f = fopen(out_path, "wb");

        if (f == NULL || fwrite(resp_payload, 1, resp_len, f) != resp_len) {
            fprintf(stderr, "%s: %s\n", out_path, strerror(errno));
            if (f != NULL)
                fclose(f);
            goto out;
        }
        fclose(f);
    }

    printf("%s: %u bytes (%.2f:1)\n", out_path, resp_len,
           (resp_len > 0) ? (double)img.bytes / resp_len : 0.0);
    printf("%d round trip%s in %.2f ms — %.1f MB/s of pixels end-to-end\n",
           count, (count == 1) ? "" : "s", t_total / 1e3,
           (t_total > 0) ? ((double)img.bytes * count) / t_total : 0.0);
    rc = 0;

out:
    if (ai != NULL)
        freeaddrinfo(ai);
    if (fd >= 0)
        close(fd);
    free(resp_payload);
    free(img.pixels);
    return rc;
}
