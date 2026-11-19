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

static void handle_frame(ab_conn_t *conn, const ab_header_t *hdr,
                         const uint8_t *payload, void *user_ctx) {
    (void)user_ctx;
    printf("[fd=%d agent=%llu] type=%u flags=%u len=%u payload_bytes=%s\n",
           conn->fd,
           (unsigned long long)hdr->agent_id,
           (unsigned)hdr->type,
           (unsigned)hdr->flags,
           (unsigned)hdr->payload_len,
           (payload && hdr->payload_len > 0u) ? "present" : "none");
}

int main(void) {
    ab_server_t srv;
    g_srv = &srv;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    if (ab_server_init(&srv, "127.0.0.1", 9090u, handle_frame, NULL) < 0) {
        fprintf(stderr, "failed to initialize agent-bus server\n");
        return 1;
    }

    printf("agent-bus listening on 127.0.0.1:9090...\n");
    int rc = ab_server_run(&srv);
    ab_server_close(&srv);
    return (rc < 0) ? 1 : 0;
}
