#define _POSIX_C_SOURCE 200809L
#include "ab_net.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static ab_server_t *g_srv = NULL;

static void on_sigint(int sig) {
    (void)sig;
    if (g_srv) {
        ab_server_stop(g_srv);
    }
}

static void send_reply_frame(ab_server_t *srv, ab_conn_t *conn,
                             uint16_t reply_type, uint64_t agent_id) {
    uint8_t pkt[AB_HDR_SIZE];
    size_t written = 0;
    if (ab_frame_encode(pkt, sizeof(pkt), &written,
                        reply_type, 0u, agent_id, NULL, 0u) == AB_PARSE_OK) {
        ab_conn_send(srv, conn, pkt, written);
    }
}

static void handle_frame(ab_conn_t *conn, const ab_header_t *hdr,
                         const uint8_t *payload, void *user_ctx) {
    (void)payload;
    ab_server_t *srv = (ab_server_t *)user_ctx;
    uint64_t id = (hdr->agent_id != 0ull) ? hdr->agent_id : conn->agent_id;

    if (hdr->type == AB_MSG_PING) {
        send_reply_frame(srv, conn, AB_MSG_PONG, id);
    } else if (hdr->type == AB_MSG_MSG) {
        send_reply_frame(srv, conn, AB_MSG_ACK, id);
    }
}

int main(void) {
    ab_server_t srv;
    g_srv = &srv;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (ab_server_init(&srv, "127.0.0.1", 9090u, handle_frame, &srv) < 0) {
        fprintf(stderr, "failed to initialize agent-bus server\n");
        return 1;
    }

    printf("agent-bus listening on 127.0.0.1:9090...\n");
    int rc = ab_server_run(&srv);
    ab_server_close(&srv);
    return (rc < 0) ? 1 : 0;
}
