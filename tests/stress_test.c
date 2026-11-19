#include "ab_frame.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AB_STRESS_FRAMES 20000u
#define AB_STREAM_CAP (256u * 1024u)

static void drain_chunked(const uint8_t *src, size_t src_len, size_t *expected_frames) {
    uint8_t *win = malloc(AB_STREAM_CAP + 64u);
    assert(win != NULL);

    size_t win_len = 0;
    size_t pos = 0;

    while (pos < src_len) {
        size_t step = (pos % 7u) + 1u;
        if (pos + step > src_len) {
            step = src_len - pos;
        }
        assert(win_len + step <= AB_STREAM_CAP + 64u);

        memcpy(win + win_len, src + pos, step);
        win_len += step;
        pos += step;

        size_t off = 0;
        while (off < win_len) {
            ab_header_t hdr;
            const uint8_t *pl = NULL;
            size_t consumed = 0;
            int drc = ab_frame_decode(win + off, win_len - off,
                                      &hdr, &pl, &consumed);
            if (drc == AB_PARSE_NEED_MORE) {
                break;
            }
            assert(drc == AB_PARSE_OK);
            assert(hdr.magic == AB_MAGIC);
            assert(hdr.version == AB_VERSION);
            assert(hdr.payload_len > 0u && pl != NULL);
            off += consumed;
            assert(*expected_frames > 0u);
            (*expected_frames)--;
        }

        if (off > 0u) {
            if (off < win_len) {
                memmove(win, win + off, win_len - off);
            }
            win_len -= off;
        }
    }

    assert(win_len == 0u);
    free(win);
}

int main(void) {
    uint8_t *stream = malloc(AB_STREAM_CAP);
    assert(stream != NULL);

    size_t stream_len = 0;
    size_t expected_frames = 0;

    for (uint32_t i = 0; i < AB_STRESS_FRAMES; i++) {
        uint8_t payload[64];
        int plen = snprintf((char *)payload, sizeof(payload), "agent-msg-%u", i);
        assert(plen > 0 && (size_t)plen < sizeof(payload));

        uint8_t frame[128];
        size_t flen = 0;
        uint16_t mtype = (i % 10u == 0u) ? AB_MSG_PING : AB_MSG_MSG;
        int erc = ab_frame_encode(frame, sizeof(frame), &flen,
                                  mtype, (i & 1u), 10000ull + (uint64_t)(i % 500u),
                                  payload, (uint32_t)plen);
        assert(erc == AB_PARSE_OK);

        if (stream_len + flen > AB_STREAM_CAP) {
            drain_chunked(stream, stream_len, &expected_frames);
            assert(expected_frames == 0u);
            stream_len = 0u;
        }

        memcpy(stream + stream_len, frame, flen);
        stream_len += flen;
        expected_frames++;
    }

    if (stream_len > 0u) {
        drain_chunked(stream, stream_len, &expected_frames);
    }
    assert(expected_frames == 0u);

    /* Invalid version check */
    uint8_t bad_ver[AB_HDR_SIZE];
    size_t w = 0;
    assert(ab_frame_encode(bad_ver, sizeof(bad_ver), &w, AB_MSG_PING, 0u, 1ull, NULL, 0u) == AB_PARSE_OK);
    bad_ver[4] = 99u;
    ab_header_t out_hdr;
    const uint8_t *out_pl = NULL;
    size_t consumed = 0;
    assert(ab_frame_decode(bad_ver, sizeof(bad_ver), &out_hdr, &out_pl, &consumed) == AB_PARSE_ERR_VERSION);

    free(stream);
    printf("stress_test passed! total_processed=%u\n", AB_STRESS_FRAMES);
    return 0;
}
