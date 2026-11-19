#define _POSIX_C_SOURCE 200809L
#include "ab_net.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    ab_server_t *srv;
    pthread_t tid;
} srv_thread_t;

static void on_test_frame(ab_conn_t *conn, const ab_header_t *hdr,
                          const uint8_t *payload, void *user_ctx) {
    (void)payload;
    ab_server_t *srv = (ab_server_t *)user_ctx;
    uint64_t id = (hdr->agent_id != 0ull) ? hdr->agent_id : conn->agent_id;
    uint8_t pkt[AB_HDR_SIZE];
    size_t written = 0;

    if (hdr->type == AB_MSG_PING) {
        if (ab_frame_encode(pkt, sizeof(pkt), &written,
                            AB_MSG_PONG, 0u, id, NULL, 0u) == AB_PARSE_OK) {
            ab_conn_send(srv, conn, pkt, written);
        }
    } else if (hdr->type == AB_MSG_HELLO || hdr->type == AB_MSG_PONG) {
        if (ab_frame_encode(pkt, sizeof(pkt), &written,
                            AB_MSG_ACK, 0u, id, NULL, 0u) == AB_PARSE_OK) {
            ab_conn_send(srv, conn, pkt, written);
        }
    }
}

static void *srv_worker(void *arg) {
    srv_thread_t *st = (srv_thread_t *)arg;
    ab_server_run(st->srv);
    return NULL;
}

static int connect_loopback(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    return fd;
}

static void send_all(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            assert(0 && "send failed in heartbeat_test");
        }
    }
}

static int recv_frame_blocking(int fd, uint8_t *buf, size_t cap,
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

int main(void) {
    uint16_t port = 19191u;
    ab_server_t *srv = calloc(1, sizeof(*srv));
    assert(srv != NULL);

    assert(ab_server_init(srv, "127.0.0.1", port, on_test_frame, srv) == 0);
    srv->ping_interval_sec = 1;
    srv->idle_timeout_sec = 2;

    srv_thread_t st;
    st.srv = srv;
    assert(pthread_create(&st.tid, NULL, srv_worker, &st) == 0);

    /* 1. Active PING/PONG keeps connection alive */
    int fd_alive = connect_loopback(port);
    uint8_t pkt[AB_HDR_SIZE];
    size_t flen = 0;
    assert(ab_frame_encode(pkt, sizeof(pkt), &flen,
                           AB_MSG_HELLO, 0u, 777ull, NULL, 0u) == AB_PARSE_OK);
    send_all(fd_alive, pkt, flen);

    uint8_t rx[256];
    ab_header_t hdr;
    const uint8_t *pl = NULL;
    assert(recv_frame_blocking(fd_alive, rx, sizeof(rx), &hdr, &pl) == 0);
    assert(hdr.type == AB_MSG_ACK);

    /* Wait for server active PING and respond with PONG */
    sleep(1);
    ab_server_sweep_timers(srv, time(NULL) + 1);
    assert(recv_frame_blocking(fd_alive, rx, sizeof(rx), &hdr, &pl) == 0);
    assert(hdr.type == AB_MSG_PING);

    assert(ab_frame_encode(pkt, sizeof(pkt), &flen,
                           AB_MSG_PONG, 0u, 777ull, NULL, 0u) == AB_PARSE_OK);
    send_all(fd_alive, pkt, flen);
    assert(recv_frame_blocking(fd_alive, rx, sizeof(rx), &hdr, &pl) == 0);
    assert(hdr.type == AB_MSG_ACK);
    assert(ab_server_find_by_agent(srv, 777ull) != NULL);

    /* 2. Idle timeout sweeps inactive connection and removes agent from map */
    int fd_idle = connect_loopback(port);
    assert(ab_frame_encode(pkt, sizeof(pkt), &flen,
                           AB_MSG_HELLO, 0u, 888ull, NULL, 0u) == AB_PARSE_OK);
    send_all(fd_idle, pkt, flen);
    assert(recv_frame_blocking(fd_idle, rx, sizeof(rx), &hdr, &pl) == 0);
    assert(hdr.type == AB_MSG_ACK);
    assert(ab_server_find_by_agent(srv, 888ull) != NULL);

    /* Force sweep past idle_timeout_sec (2s) */
    ab_server_sweep_timers(srv, time(NULL) + 5);
    assert(ab_server_find_by_agent(srv, 888ull) == NULL);

    uint8_t dummy = 0;
    ssize_t r_eof = recv(fd_idle, &dummy, 1u, 0);
    assert(r_eof <= 0);

    close(fd_alive);
    close(fd_idle);

    ab_server_stop(srv);
    pthread_join(st.tid, NULL);
    ab_server_close(srv);
    free(srv);

    printf("heartbeat_test passed!\n");
    return 0;
}
