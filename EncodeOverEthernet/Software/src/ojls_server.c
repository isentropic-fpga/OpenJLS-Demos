/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* ojls_server.c: board-side server; encodes TCP image requests on the PL
 *
 * Hardware is reached only through UIO (register banks) and u-dma-buf (pixels,
 * bitstream, SG descriptors), so any board exposing those runs it. */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ojls_proto.h"
#include "ojls_regs.h"
#include "axidma.h"
#include "udmabuf.h"
#include "uio.h"

struct dmabuf_region {
    struct udmabuf *b;
    size_t          off;
    size_t          size;
};

static uint8_t *region_mem(const struct dmabuf_region *r)   { return r->b->mem + r->off; }
static uint64_t region_phys(const struct dmabuf_region *r)  { return r->b->phys + r->off; }

struct hw {
    struct uio           regs;
    struct axidma        dma;
    struct udmabuf       tx_buf;    /* pixels in                              */
    struct udmabuf       rx_buf;    /* bitstream out                          */
    struct udmabuf       desc_buf;  /* SG descriptor rings                    */
    struct dmabuf_region tx;
    struct dmabuf_region rx;
    uint32_t             bitness;
    uint32_t             max_width;
    uint32_t             max_height;
};

struct options {
    int         port;
    const char *regs_name;
    const char *dma_name;
    const char *tx_buf;   /* NULL = default name */
    const char *rx_buf;   /* NULL = default name */
    const char *desc_buf; /* NULL = default name */
    int         timeout_ms;
    int         loopback;
};

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Read exactly len bytes. Returns 0 on success, 1 on orderly EOF before any
 * byte, -1 on error/truncation. */
static int recv_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t   got = 0;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);

        if (n == 0)
            return (got == 0) ? 1 : -1;
        if (n < 0) {
            if (errno == EINTR)
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

/* Discard len payload bytes so the request stream stays in sync after a
 * rejected header. */
static int drain(int fd, size_t len)
{
    uint8_t scratch[4096];

    while (len > 0) {
        const size_t chunk = (len < sizeof(scratch)) ? len : sizeof(scratch);

        if (recv_full(fd, scratch, chunk) != 0)
            return -1;
        len -= chunk;
    }
    return 0;
}

static int send_error(int fd, const struct ojls_hdr *req, uint8_t status)
{
    struct ojls_hdr resp = *req;
    uint8_t         buf[OJLS_PROTO_HDR_LEN];

    resp.magic       = OJLS_PROTO_MAGIC;
    resp.version     = OJLS_PROTO_VERSION;
    resp.type        = OJLS_MSG_ENCODE_RESP;
    resp.status      = status;
    resp.payload_len = 0;
    ojls_hdr_pack(buf, &resp);
    fprintf(stderr, "request rejected: %s\n", ojls_status_str(status));
    return send_full(fd, buf, sizeof(buf));
}

static int hw_open(struct hw *hw, const struct options *opt)
{
    uint32_t id, version, caps, maxdim;

    memset(hw, 0, sizeof(*hw));

    if (uio_open(&hw->regs, opt->regs_name) != 0)
        return -1;

    id = uio_rd(&hw->regs, OJLS_REG_ID);
    if (id != OJLS_ID_VALUE) {
        fprintf(stderr, "openjls: ID register reads 0x%08x, expected 0x%08x (\"OJLS\") — "
                "wrong UIO device or address map?\n", id, OJLS_ID_VALUE);
        return -1;
    }
    version = uio_rd(&hw->regs, OJLS_REG_VERSION);
    caps    = uio_rd(&hw->regs, OJLS_REG_CAPS);
    maxdim  = uio_rd(&hw->regs, OJLS_REG_MAXDIM);

    hw->bitness    = OJLS_CAPS_BITNESS(caps);
    hw->max_width  = OJLS_MAXDIM_WIDTH(maxdim);
    hw->max_height = OJLS_MAXDIM_HEIGHT(maxdim);

    printf("openjls: v%u.%u.%u on %s — BITNESS %u, %u bytes/beat out, max %ux%u\n",
           (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF,
           hw->regs.name, hw->bitness, OJLS_CAPS_OUT_BYTES(caps),
           hw->max_width, hw->max_height);

    if (axidma_open(&hw->dma, opt->dma_name) != 0)
        return -1;

    /* By name, not index: u-dma-buf enumerates alphabetically, so "-rx" would
     * come before "-tx" and swap the roles. */
    {
        const char *tx_name   = (opt->tx_buf   != NULL) ? opt->tx_buf   : "udmabuf-ojls-tx";
        const char *rx_name   = (opt->rx_buf   != NULL) ? opt->rx_buf   : "udmabuf-ojls-rx";
        const char *desc_name = (opt->desc_buf != NULL) ? opt->desc_buf : "udmabuf-ojls-desc";

        if (udmabuf_open(&hw->tx_buf, tx_name) != 0 ||
            udmabuf_open(&hw->rx_buf, rx_name) != 0 ||
            udmabuf_open(&hw->desc_buf, desc_name) != 0)
            return -1;
    }

    hw->tx = (struct dmabuf_region){ .b = &hw->tx_buf, .off = 0, .size = hw->tx_buf.size };
    hw->rx = (struct dmabuf_region){ .b = &hw->rx_buf, .off = 0, .size = hw->rx_buf.size };

    printf("buffers: tx \"%s\" %zu KiB @ 0x%09" PRIx64 ", rx \"%s\" %zu KiB @ 0x%09" PRIx64
           ", desc \"%s\" %zu KiB\n",
           hw->tx.b->name, hw->tx.size / 1024, region_phys(&hw->tx),
           hw->rx.b->name, hw->rx.size / 1024, region_phys(&hw->rx),
           hw->desc_buf.name, hw->desc_buf.size / 1024);

    return 0;
}

static void hw_close(struct hw *hw)
{
    udmabuf_close(&hw->desc_buf);
    udmabuf_close(&hw->rx_buf);
    udmabuf_close(&hw->tx_buf);
    axidma_close(&hw->dma);
    uio_close(&hw->regs);
}

/* Write dimensions and pulse CTRL.APPLY; the core samples them during the
 * reset pulse. Returns 0 once STATUS.BUSY clears. */
static int hw_configure(struct hw *hw, uint16_t width, uint16_t height)
{
    const int64_t deadline = now_us() + 100000;

    uio_wr(&hw->regs, OJLS_REG_WIDTH, width);
    uio_wr(&hw->regs, OJLS_REG_HEIGHT, height);

    if (uio_rd(&hw->regs, OJLS_REG_WIDTH) != width ||
        uio_rd(&hw->regs, OJLS_REG_HEIGHT) != height)
        return -1; /* clamped: out of the hardware's range */

    uio_wr(&hw->regs, OJLS_REG_CTRL, OJLS_CTRL_APPLY);

    while ((uio_rd(&hw->regs, OJLS_REG_STATUS) & OJLS_STATUS_BUSY) != 0) {
        if (now_us() > deadline) {
            fprintf(stderr, "openjls: STATUS.BUSY stuck after APPLY\n");
            return -1;
        }
    }
    return 0;
}

/* After a DMA error or timeout: reset the DMA engine, then pulse the core
 * reset so any half-encoded image is dropped. */
static void hw_recover(struct hw *hw, uint16_t width, uint16_t height)
{
    fprintf(stderr, "recovering: DMA + core reset\n");
    axidma_reset(&hw->dma);
    hw_configure(hw, width, height);
}

/* Payload already in the tx buffer. Returns 0 once replied, a status code for
 * the caller to send, or -1 on socket error. */
static int encode_and_reply(int fd, struct hw *hw, const struct ojls_hdr *req, int timeout_ms)
{
    const uint32_t in_bytes  = req->payload_len;
    const size_t   desc_half = hw->desc_buf.size / 2;
    struct axidma_ring mm2s, s2mm;
    struct ojls_hdr resp;
    uint8_t         hdr_buf[OJLS_PROTO_HDR_LEN];
    uint32_t        out_bytes;
    int64_t         t0, t1;
    int             rc;

    if (hw_configure(hw, req->width, req->height) != 0)
        return OJLS_ST_HW_ERROR;

    if (udmabuf_sync_for_device(hw->tx.b, hw->tx.off, in_bytes) != 0)
        return OJLS_ST_HW_ERROR;

    /* S2MM spans the whole rx buffer and stops on the encoder's TLAST; MM2S is
     * framed so the pixels form one packet across its descriptors. */
    if (axidma_reset(&hw->dma) != 0)
        return OJLS_ST_HW_ERROR;

    if (axidma_ring_build(&s2mm, &hw->desc_buf, 0, desc_half,
                          region_phys(&hw->rx), hw->rx.size, 0) != 0 ||
        axidma_ring_build(&mm2s, &hw->desc_buf, desc_half, hw->desc_buf.size - desc_half,
                          region_phys(&hw->tx), in_bytes, 1) != 0)
        return OJLS_ST_HW_ERROR;

    if (udmabuf_sync_for_device(&hw->desc_buf, 0, hw->desc_buf.size) != 0)
        return OJLS_ST_HW_ERROR;

    t0 = now_us();

    if (axidma_sg_start(&hw->dma, AXIDMA_S2MM, &s2mm) != 0 ||
        axidma_sg_start(&hw->dma, AXIDMA_MM2S, &mm2s) != 0)
        return OJLS_ST_HW_ERROR;

    rc = axidma_wait(&hw->dma, AXIDMA_S2MM, &s2mm, timeout_ms);
    if (rc == -2) {
        hw_recover(hw, req->width, req->height);
        return OJLS_ST_HW_TIMEOUT;
    }
    if (rc != 0) {
        hw_recover(hw, req->width, req->height);
        return OJLS_ST_HW_ERROR;
    }

    t1 = now_us();

    out_bytes = axidma_ring_bytes(&s2mm);

    if (udmabuf_sync_for_cpu(hw->rx.b, hw->rx.off, out_bytes) != 0)
        return OJLS_ST_HW_ERROR;

    resp.magic       = OJLS_PROTO_MAGIC;
    resp.version     = OJLS_PROTO_VERSION;
    resp.type        = OJLS_MSG_ENCODE_RESP;
    resp.status      = OJLS_ST_OK;
    resp.width       = req->width;
    resp.height      = req->height;
    resp.bitness     = req->bitness;
    resp.payload_len = out_bytes;
    ojls_hdr_pack(hdr_buf, &resp);

    if (send_full(fd, hdr_buf, sizeof(hdr_buf)) != 0 ||
        send_full(fd, region_mem(&hw->rx), out_bytes) != 0)
        return -1;

    printf("%ux%u @ %u bpp: %u -> %u bytes (%.2f:1) in %.2f ms, %.1f MB/s\n",
           req->width, req->height, req->bitness, in_bytes, out_bytes,
           (out_bytes > 0) ? (double)in_bytes / out_bytes : 0.0,
           (t1 - t0) / 1e3, (t1 > t0) ? in_bytes / (double)(t1 - t0) : 0.0);

    return 0;
}

static int loopback_reply(int fd, const struct ojls_hdr *req, const uint8_t *payload)
{
    struct ojls_hdr resp = *req;
    uint8_t         hdr_buf[OJLS_PROTO_HDR_LEN];

    resp.type   = OJLS_MSG_ENCODE_RESP;
    resp.status = OJLS_ST_OK;
    ojls_hdr_pack(hdr_buf, &resp);

    if (send_full(fd, hdr_buf, sizeof(hdr_buf)) != 0 ||
        send_full(fd, payload, req->payload_len) != 0)
        return -1;

    printf("loopback: echoed %ux%u @ %u bpp, %u bytes\n",
           req->width, req->height, req->bitness, req->payload_len);
    return 0;
}

/* Serve requests on one connection until EOF. Returns 0 on orderly close. */
static int serve_connection(int fd, struct hw *hw, const struct options *opt)
{
    uint8_t *loop_buf = NULL;
    size_t   loop_cap = 0;
    int      rc = 0;

    for (;;) {
        uint8_t         hdr_raw[OJLS_PROTO_HDR_LEN];
        struct ojls_hdr req;
        uint32_t        expect;
        int             st;

        rc = recv_full(fd, hdr_raw, sizeof(hdr_raw));
        if (rc == 1) { /* orderly EOF between requests */
            rc = 0;
            break;
        }
        if (rc != 0)
            break;
        ojls_hdr_unpack(&req, hdr_raw);

        /* Bad magic: not our stream, so drop the connection. */
        if (req.magic != OJLS_PROTO_MAGIC) {
            send_error(fd, &req, OJLS_ST_BAD_MAGIC);
            rc = -1;
            break;
        }
        if (req.version != OJLS_PROTO_VERSION)
            st = OJLS_ST_BAD_VERSION;
        else if (req.type != OJLS_MSG_ENCODE_REQ)
            st = OJLS_ST_BAD_TYPE;
        else
            st = OJLS_ST_OK;

        if (st == OJLS_ST_OK) {
            expect = (uint32_t)req.width * req.height * ojls_bytes_per_pixel(req.bitness);
            if (req.bitness < 8 || req.bitness > 16)
                st = OJLS_ST_BAD_BITNESS;
            else if (req.payload_len != expect)
                st = OJLS_ST_SIZE_MISMATCH;
        }

        if (st == OJLS_ST_OK && !opt->loopback) {
            if (req.bitness != hw->bitness)
                st = OJLS_ST_BAD_BITNESS;
            else if (req.width < OJLS_MIN_WIDTH || req.width > hw->max_width ||
                     req.height < OJLS_MIN_HEIGHT || req.height > hw->max_height)
                st = OJLS_ST_BAD_DIMS;
            else if (req.payload_len > hw->tx.size ||
                     /* Headroom for incompressible input; an overrun is still
                      * caught as a missing TLAST, not corrupted. */
                     (uint64_t)req.payload_len + req.payload_len / 4 + 4096 > hw->rx.size)
                st = OJLS_ST_TOO_LARGE;
        }

        if (st != OJLS_ST_OK) {
            if (drain(fd, req.payload_len) != 0 || send_error(fd, &req, (uint8_t)st) != 0) {
                rc = -1;
                break;
            }
            continue;
        }

        if (opt->loopback) {
            if (loop_cap < req.payload_len) {
                free(loop_buf);
                loop_buf = malloc(req.payload_len);
                loop_cap = (loop_buf != NULL) ? req.payload_len : 0;
                if (loop_buf == NULL) {
                    rc = -1;
                    break;
                }
            }
            if (recv_full(fd, loop_buf, req.payload_len) != 0 ||
                loopback_reply(fd, &req, loop_buf) != 0) {
                rc = -1;
                break;
            }
            continue;
        }

        if (recv_full(fd, region_mem(&hw->tx), req.payload_len) != 0) {
            rc = -1;
            break;
        }

        st = encode_and_reply(fd, hw, &req, opt->timeout_ms);
        if (st < 0) {
            rc = -1;
            break;
        }
        if (st != 0 && send_error(fd, &req, (uint8_t)st) != 0) {
            rc = -1;
            break;
        }
    }

    free(loop_buf);
    return rc;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -p PORT        TCP port (default %d)\n"
            "  --regs NAME    UIO device of the openjls_axis_regs register bank (default \"openjls\")\n"
            "  --dma NAME     UIO device of the AXI DMA (default \"dma\")\n"
            "  --tx-buf NAME  u-dma-buf for pixels in (default \"udmabuf-ojls-tx\")\n"
            "  --rx-buf NAME  u-dma-buf for bitstream out (default \"udmabuf-ojls-rx\")\n"
            "  --desc-buf NAME u-dma-buf for SG descriptor rings (default \"udmabuf-ojls-desc\")\n"
            "  --timeout MS   per-image encode timeout (default 10000)\n"
            "  --loopback     no hardware; echo payloads back (protocol test)\n",
            argv0, OJLS_PROTO_PORT);
}

int main(int argc, char **argv)
{
    struct options opt = {
        .port       = OJLS_PROTO_PORT,
        .regs_name  = "openjls",
        .dma_name   = "dma",
        .timeout_ms = 10000,
    };
    struct hw          hw;
    struct sockaddr_in addr;
    struct sigaction   sa = { .sa_handler = on_sigint };
    int                listen_fd;
    int                yes = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            opt.port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--regs") == 0 && i + 1 < argc)
            opt.regs_name = argv[++i];
        else if (strcmp(argv[i], "--dma") == 0 && i + 1 < argc)
            opt.dma_name = argv[++i];
        else if (strcmp(argv[i], "--tx-buf") == 0 && i + 1 < argc)
            opt.tx_buf = argv[++i];
        else if (strcmp(argv[i], "--rx-buf") == 0 && i + 1 < argc)
            opt.rx_buf = argv[++i];
        else if (strcmp(argv[i], "--desc-buf") == 0 && i + 1 < argc)
            opt.desc_buf = argv[++i];
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            opt.timeout_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--loopback") == 0)
            opt.loopback = 1;
        else {
            usage(argv[0]);
            return (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) ? 0 : 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (opt.loopback)
        printf("loopback mode: no hardware will be touched\n");
    else if (hw_open(&hw, &opt) != 0)
        return 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)opt.port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 1) != 0) {
        perror("bind/listen");
        return 1;
    }
    printf("listening on port %d\n", opt.port);

    while (!g_stop) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        char               peer_str[INET_ADDRSTRLEN] = "?";
        int                fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        inet_ntop(AF_INET, &peer.sin_addr, peer_str, sizeof(peer_str));
        printf("client %s connected\n", peer_str);

        serve_connection(fd, opt.loopback ? NULL : &hw, &opt);

        close(fd);
        printf("client %s disconnected\n", peer_str);
    }

    close(listen_fd);
    if (!opt.loopback)
        hw_close(&hw);
    printf("bye\n");
    return 0;
}
