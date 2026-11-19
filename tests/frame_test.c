#include "ab_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const uint8_t msg[] = "agent-1001-heartbeat";
    uint8_t wire[256];
    size_t written = 0;

    int rc = ab_frame_encode(wire, sizeof(wire), &written,
                             AB_MSG_MSG, 1u, 1001ull,
                             msg, (uint32_t)(sizeof(msg) - 1u));
    assert(rc == AB_PARSE_OK);
    assert(written == AB_HDR_SIZE + sizeof(msg) - 1u);

    /* Partial read check */
    ab_header_t hdr;
    const uint8_t *pl = NULL;
    size_t consumed = 0;
    assert(ab_frame_decode(wire, AB_HDR_SIZE - 1u, &hdr, &pl, &consumed) == AB_PARSE_NEED_MORE);
    assert(ab_frame_decode(wire, written - 1u, &hdr, &pl, &consumed) == AB_PARSE_NEED_MORE);

    /* Full decode check */
    assert(ab_frame_decode(wire, written, &hdr, &pl, &consumed) == AB_PARSE_OK);
    assert(consumed == written);
    assert(hdr.magic == AB_MAGIC);
    assert(hdr.version == AB_VERSION);
    assert(hdr.type == AB_MSG_MSG);
    assert(hdr.flags == 1u);
    assert(hdr.agent_id == 1001ull);
    assert(hdr.payload_len == sizeof(msg) - 1u);
    assert(pl != NULL);
    assert(memcmp(pl, msg, hdr.payload_len) == 0);

    /* Bad magic check */
    uint8_t bad[AB_HDR_SIZE];
    memset(bad, 0xFF, sizeof(bad));
    assert(ab_frame_decode(bad, sizeof(bad), &hdr, &pl, &consumed) == AB_PARSE_ERR_MAGIC);

    printf("frame_test passed!\n");
    return 0;
}
