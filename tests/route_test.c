#define _POSIX_C_SOURCE 200809L
#include "ab_frame.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_tcp(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "inet_pton() failed for %s\n", host);
        close(fd);
        return -1;
    }

    int connected = 0;
    for (int attempt = 0; attempt < 25; attempt++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            connected = 1;
            break;
        }
        if (errno != ECONNREFUSED && errno != EINPROGRESS) {
            break;
        }
        usleep(40000);
    }
    if (!connected) {
        fprintf(stderr, "connect(%s:%u) failed: %s\n", host, (unsigned)port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void send_all_bytes(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            assert(0 && "send failed in route_test");
        }
    }
}

static int recv_one_frame(int fd, uint8_t *buf, size_t cap,
                          ab_header_t *hdr_out, const uint8_t **pl_out) {
    size_t len = 0;
    for (;;) {
        size_t consumed = 0;
        int rc = ab_frame_decode(buf, len, hdr_out, pl_out, &consumed);
        if (rc == AB_PARSE_OK) {
            return 0;
        }
        assert(rc == AB_PARSE_NEED_MORE);
        assert(len < cap);

        ssize_t n = recv(fd, buf + len, cap - len, 0);
        if (n > 0) {
            len += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
}

static void write_u64_le_local(uint8_t *p, uint64_t v) {
    for (size_t i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (i * 8u)) & 0xffu);
    }
}

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 9090u;

    int fd_a = connect_tcp(host, port);
    assert(fd_a >= 0);
    int fd_b = connect_tcp(host, port);
    assert(fd_b >= 0);

    uint8_t pkt[512];
    size_t flen = 0;

    /* 1. Register Agent 101 and Agent 202 via HELLO */
    assert(ab_frame_encode(pkt, sizeof(pkt), &flen, AB_MSG_HELLO, 0u, 101ull, NULL, 0u) == AB_PARSE_OK);
    send_all_bytes(fd_a, pkt, flen);

    assert(ab_frame_encode(pkt, sizeof(pkt), &flen, AB_MSG_HELLO, 0u, 202ull, NULL, 0u) == AB_PARSE_OK);
    send_all_bytes(fd_b, pkt, flen);

    uint8_t rx[1024];
    ab_header_t rhdr;
    const uint8_t *rpl = NULL;
    assert(recv_one_frame(fd_a, rx, sizeof(rx), &rhdr, &rpl) == 0);
    assert(rhdr.type == AB_MSG_ACK);
    assert(recv_one_frame(fd_b, rx, sizeof(rx), &rhdr, &rpl) == 0);
    assert(rhdr.type == AB_MSG_ACK);

    /* 2. Agent 101 sends routed message to Agent 202 */
    const char *text = "hello-agent-202";
    size_t tlen = strlen(text);
    uint8_t payload[128];
    assert(8u + tlen <= sizeof(payload));
    write_u64_le_local(payload, 202ull);
    memcpy(payload + 8u, text, tlen);

    assert(ab_frame_encode(pkt, sizeof(pkt), &flen, AB_MSG_MSG, 1u, 101ull,
                           payload, (uint32_t)(8u + tlen)) == AB_PARSE_OK);
    send_all_bytes(fd_a, pkt, flen);

    /* Expect ACK on Agent 101 */
    assert(recv_one_frame(fd_a, rx, sizeof(rx), &rhdr, &rpl) == 0);
    assert(rhdr.type == AB_MSG_ACK);

    /* Expect forwarded MSG on Agent 202 with sender agent_id == 101 */
    uint8_t rx_b[1024];
    assert(recv_one_frame(fd_b, rx_b, sizeof(rx_b), &rhdr, &rpl) == 0);
    assert(rhdr.type == AB_MSG_MSG);
    assert(rhdr.agent_id == 101ull);
    assert(rhdr.payload_len == (uint32_t)tlen);
    assert(rpl != NULL && memcmp(rpl, text, tlen) == 0);

    /* 3. Route to non-existent agent 999 -> expect ERR on Agent 101 */
    write_u64_le_local(payload, 999ull);
    assert(ab_frame_encode(pkt, sizeof(pkt), &flen, AB_MSG_MSG, 0u, 101ull,
                           payload, (uint32_t)(8u + tlen)) == AB_PARSE_OK);
    send_all_bytes(fd_a, pkt, flen);

    assert(recv_one_frame(fd_a, rx, sizeof(rx), &rhdr, &rpl) == 0);
    assert(rhdr.type == AB_MSG_ERR);

    close(fd_a);
    close(fd_b);

    printf("route_test passed!\n");
    return 0;
}
