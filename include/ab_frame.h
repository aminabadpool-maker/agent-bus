#ifndef AB_FRAME_H
#define AB_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define AB_MAGIC 0x41425553u
#define AB_VERSION 1u
#define AB_HDR_SIZE 24u
#define AB_MAX_PAYLOAD (1024u * 1024u)

enum ab_msg_type {
    AB_MSG_HELLO = 1,
    AB_MSG_PING  = 2,
    AB_MSG_PONG  = 3,
    AB_MSG_MSG   = 4,
    AB_MSG_ACK   = 5,
    AB_MSG_ERR   = 6
};

enum ab_parse_status {
    AB_PARSE_OK = 0,
    AB_PARSE_NEED_MORE = 1,
    AB_PARSE_ERR_MAGIC = -1,
    AB_PARSE_ERR_VERSION = -2,
    AB_PARSE_ERR_LEN = -3,
    AB_PARSE_ERR_ARGS = -4
};

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint64_t agent_id;
    uint32_t payload_len;
} ab_header_t;

int ab_frame_encode(uint8_t *out, size_t out_cap, size_t *out_written,
                  uint16_t type, uint32_t flags, uint64_t agent_id,
                  const uint8_t *payload, uint32_t payload_len);

int ab_frame_decode(const uint8_t *buf, size_t buf_len,
                  ab_header_t *hdr_out, const uint8_t **payload_out,
                  size_t *consumed_out);

#endif
