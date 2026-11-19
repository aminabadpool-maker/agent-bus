#include "ab_frame.h"
#include <string.h>

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void write_u64_le(uint8_t *p, uint64_t v) {
    for (size_t i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xffu);
    }
}

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8u; i++) {
        v |= ((uint64_t)p[i]) << (i * 8u);
    }
    return v;
}

int ab_frame_encode(uint8_t *out, size_t out_cap, size_t *out_written,
                  uint16_t type, uint32_t flags, uint64_t agent_id,
                  const uint8_t *payload, uint32_t payload_len) {
    if (!out || !out_written || payload_len > AB_MAX_PAYLOAD) {
        return AB_PARSE_ERR_ARGS;
    }
    if (payload_len > 0u && !payload) {
        return AB_PARSE_ERR_ARGS;
    }

    size_t total = (size_t)AB_HDR_SIZE + (size_t)payload_len;
    if (total < (size_t)AB_HDR_SIZE || out_cap < total) {
        return AB_PARSE_ERR_LEN;
    }

    write_u32_le(out + 0, AB_MAGIC);
    write_u16_le(out + 4, AB_VERSION);
    write_u16_le(out + 6, type);
    write_u32_le(out + 8, flags);
    write_u64_le(out + 12, agent_id);
    write_u32_le(out + 20, payload_len);

    if (payload_len > 0u) {
        memcpy(out + AB_HDR_SIZE, payload, payload_len);
    }

    *out_written = total;
    return AB_PARSE_OK;
}

int ab_frame_decode(const uint8_t *buf, size_t buf_len,
                  ab_header_t *hdr_out, const uint8_t **payload_out,
                  size_t *consumed_out) {
    if (!buf || !hdr_out || !payload_out || !consumed_out) {
        return AB_PARSE_ERR_ARGS;
    }
    if (buf_len < AB_HDR_SIZE) {
        return AB_PARSE_NEED_MORE;
    }

    ab_header_t hdr;
    hdr.magic = read_u32_le(buf + 0);
    hdr.version = read_u16_le(buf + 4);
    hdr.type = read_u16_le(buf + 6);
    hdr.flags = read_u32_le(buf + 8);
    hdr.agent_id = read_u64_le(buf + 12);
    hdr.payload_len = read_u32_le(buf + 20);

    if (hdr.magic != AB_MAGIC) {
        return AB_PARSE_ERR_MAGIC;
    }
    if (hdr.version != AB_VERSION) {
        return AB_PARSE_ERR_VERSION;
    }
    if (hdr.payload_len > AB_MAX_PAYLOAD) {
        return AB_PARSE_ERR_LEN;
    }

    size_t total = (size_t)AB_HDR_SIZE + (size_t)hdr.payload_len;
    if (buf_len < total) {
        return AB_PARSE_NEED_MORE;
    }

    *hdr_out = hdr;
    *payload_out = (hdr.payload_len > 0u) ? (buf + AB_HDR_SIZE) : NULL;
    *consumed_out = total;
    return AB_PARSE_OK;
}
