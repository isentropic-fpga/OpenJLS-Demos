/*
 * Copyright (C) 2026 Vitor Mendes Camilo
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This file is part of OpenJLS-Demos. Available under GPLv3 or a
 * commercial license. See LICENSE.md and README.md for details.
 */

/* ojls_proto.h: demo wire protocol, shared by server and client
 *
 * A 20-byte little-endian header, packed byte by byte (no struct padding or
 * host endianness), then the payload: raw pixels in ENCODE_REQ (row-major,
 * 1 byte/pixel at bitness 8, else 2 bytes LE), the .jls stream in
 * ENCODE_RESP (empty when status != 0). */

#ifndef OJLS_PROTO_H
#define OJLS_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define OJLS_PROTO_MAGIC   0x4F4A4C53u /* "OJLS" */
#define OJLS_PROTO_VERSION 1u
#define OJLS_PROTO_HDR_LEN 20u
#define OJLS_PROTO_PORT    19020 /* default TCP port: 0x4A4C, "JL" */

/* Message types */
#define OJLS_MSG_ENCODE_REQ  1u
#define OJLS_MSG_ENCODE_RESP 2u

/* Response status codes */
#define OJLS_ST_OK            0u
#define OJLS_ST_BAD_MAGIC     1u
#define OJLS_ST_BAD_VERSION   2u
#define OJLS_ST_BAD_TYPE      3u
#define OJLS_ST_BAD_DIMS      4u /* outside the hardware's min..max range   */
#define OJLS_ST_BAD_BITNESS   5u /* != the hardware's synthesis-time BITNESS */
#define OJLS_ST_SIZE_MISMATCH 6u /* payload_len != width*height*bytes/pixel */
#define OJLS_ST_TOO_LARGE     7u /* exceeds the board's DMA buffer sizes    */
#define OJLS_ST_HW_ERROR      8u /* DMA error — see server log              */
#define OJLS_ST_HW_TIMEOUT    9u /* encode did not finish in time           */

struct ojls_hdr {
    uint32_t magic;
    uint16_t version;
    uint8_t  type;
    uint8_t  status;
    uint16_t width;
    uint16_t height;
    uint8_t  bitness;
    uint32_t payload_len;
};

static inline void ojls_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline void ojls_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t ojls_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ojls_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void ojls_hdr_pack(uint8_t buf[OJLS_PROTO_HDR_LEN], const struct ojls_hdr *h)
{
    ojls_put_u32(buf + 0, h->magic);
    ojls_put_u16(buf + 4, h->version);
    buf[6] = h->type;
    buf[7] = h->status;
    ojls_put_u16(buf + 8, h->width);
    ojls_put_u16(buf + 10, h->height);
    buf[12] = h->bitness;
    buf[13] = 0;
    buf[14] = 0;
    buf[15] = 0;
    ojls_put_u32(buf + 16, h->payload_len);
}

static inline void ojls_hdr_unpack(struct ojls_hdr *h, const uint8_t buf[OJLS_PROTO_HDR_LEN])
{
    h->magic       = ojls_get_u32(buf + 0);
    h->version     = ojls_get_u16(buf + 4);
    h->type        = buf[6];
    h->status      = buf[7];
    h->width       = ojls_get_u16(buf + 8);
    h->height      = ojls_get_u16(buf + 10);
    h->bitness     = buf[12];
    h->payload_len = ojls_get_u32(buf + 16);
}

static inline const char *ojls_status_str(uint8_t status)
{
    switch (status) {
    case OJLS_ST_OK:            return "OK";
    case OJLS_ST_BAD_MAGIC:     return "bad magic";
    case OJLS_ST_BAD_VERSION:   return "unsupported protocol version";
    case OJLS_ST_BAD_TYPE:      return "unsupported message type";
    case OJLS_ST_BAD_DIMS:      return "dimensions outside hardware range";
    case OJLS_ST_BAD_BITNESS:   return "bitness does not match hardware";
    case OJLS_ST_SIZE_MISMATCH: return "payload length does not match dimensions";
    case OJLS_ST_TOO_LARGE:     return "image exceeds board DMA buffers";
    case OJLS_ST_HW_ERROR:      return "hardware/DMA error";
    case OJLS_ST_HW_TIMEOUT:    return "hardware timeout";
    default:                    return "unknown status";
    }
}

/* Bytes per pixel on the wire and in the hardware pixel stream. */
static inline uint32_t ojls_bytes_per_pixel(uint8_t bitness)
{
    return (bitness > 8) ? 2u : 1u;
}

#endif /* OJLS_PROTO_H */
