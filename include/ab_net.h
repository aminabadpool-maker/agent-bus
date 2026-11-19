#ifndef AB_NET_H
#define AB_NET_H

#include "ab_frame.h"
#include <stddef.h>
#include <stdint.h>

#define AB_MAX_CONNS 16384
#define AB_RECV_BUF_CAP 65536u
#define AB_SEND_BUF_CAP 65536u

typedef struct {
    int fd;
    int active;
    int want_write;
    uint64_t agent_id;
    uint8_t recv_buf[AB_RECV_BUF_CAP];
    size_t recv_len;
    uint8_t send_buf[AB_SEND_BUF_CAP];
    size_t send_len;
    size_t send_off;
} ab_conn_t;

typedef void (*ab_on_frame_fn)(ab_conn_t *conn, const ab_header_t *hdr,
                               const uint8_t *payload, void *user_ctx);

typedef struct {
    int listen_fd;
    int epoll_fd;
    int running;
    ab_conn_t conns[AB_MAX_CONNS];
    ab_on_frame_fn on_frame;
    void *user_ctx;
} ab_server_t;

int ab_server_init(ab_server_t *srv, const char *host, uint16_t port,
                   ab_on_frame_fn on_frame, void *user_ctx);
int ab_server_run(ab_server_t *srv);
void ab_server_stop(ab_server_t *srv);
void ab_server_close(ab_server_t *srv);
int ab_conn_send(ab_server_t *srv, ab_conn_t *conn, const uint8_t *data, size_t len);
ab_conn_t *ab_server_find_by_agent(ab_server_t *srv, uint64_t agent_id);

#endif
