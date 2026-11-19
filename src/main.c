#define _POSIX_C_SOURCE 200809L
#include "ab_net.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ab_server_t *g_srv = NULL;

static void on_sigint(int sig) {
    (void)sig;
    if (g_srv) {
        ab_server_stop(g_srv);
    }
}

static uint64_t read_u64_le_local(const uint8_t *p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8u; i++) {
        v |= ((uint64_t)p[i]) << (i * 8u);
    }
    return v;
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

static void route_agent_msg(ab_server_t *srv, ab_conn_t *sender,
                            const ab_header_t *hdr, const uint8_t *payload) {
    uint64_t sender_id = (hdr->agent_id != 0ull) ? hdr->agent_id : sender->agent_id;

    if (payload && hdr->payload_len >= 8u) {
        uint64_t target_id = read_u64_le_local(payload);
        if (target_id != 0ull) {
            ab_conn_t *dst = ab_server_find_by_agent(srv, target_id);
            if (!dst || !dst->active) {
                send_reply_frame(srv, sender, AB_MSG_ERR, sender_id);
                return;
            }

            const uint8_t *inner_payload = payload + 8u;
            uint32_t inner_len = hdr->payload_len - 8u;
            uint8_t fwd[AB_HDR_SIZE + 4096u];
            size_t flen = 0;

            if (inner_len <= 4096u &&
                ab_frame_encode(fwd, sizeof(fwd), &flen,
                                AB_MSG_MSG, hdr->flags, sender_id,
                                inner_payload, inner_len) == AB_PARSE_OK) {
                ab_conn_send(srv, dst, fwd, flen);
            }
            send_reply_frame(srv, sender, AB_MSG_ACK, sender_id);
            return;
        }
    }

    send_reply_frame(srv, sender, AB_MSG_ACK, sender_id);
}

static void handle_frame(ab_conn_t *conn, const ab_header_t *hdr,
                         const uint8_t *payload, void *user_ctx) {
    ab_server_t *srv = (ab_server_t *)user_ctx;
    uint64_t id = (hdr->agent_id != 0ull) ? hdr->agent_id : conn->agent_id;

    if (hdr->type == AB_MSG_PING) {
        send_reply_frame(srv, conn, AB_MSG_PONG, id);
    } else if (hdr->type == AB_MSG_HELLO) {
        send_reply_frame(srv, conn, AB_MSG_ACK, id);
    } else if (hdr->type == AB_MSG_MSG) {
        route_agent_msg(srv, conn, hdr, payload);
    }
}

int main(void) {
    ab_server_t *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        fprintf(stderr, "failed to allocate agent-bus server\n");
        return 1;
    }
    g_srv = srv;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (ab_server_init(srv, "127.0.0.1", 9090u, handle_frame, srv) < 0) {
        fprintf(stderr, "failed to initialize agent-bus server\n");
        free(srv);
        return 1;
    }

    printf("agent-bus listening on 127.0.0.1:9090...\n");
    fflush(stdout);
    int rc = ab_server_run(srv);
    ab_server_close(srv);
    free(srv);
    g_srv = NULL;
    return (rc < 0) ? 1 : 0;
}
