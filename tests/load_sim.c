#define _POSIX_C_SOURCE 200809L
#include "ab_frame.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 9090u;
    int agents = (argc > 3) ? atoi(argv[3]) : 256;
    int msgs_per_agent = (argc > 4) ? atoi(argv[4]) : 20;

    if (agents <= 0 || agents > 16384 || msgs_per_agent <= 0) {
        fprintf(stderr, "usage: %s [host] [port] [agents<=16384] [msgs_per_agent]\n", argv[0]);
        return 1;
    }

    int *fds = calloc((size_t)agents, sizeof(int));
    int *sent_msgs = calloc((size_t)agents, sizeof(int));
    if (!fds || !sent_msgs) {
        free(fds);
        free(sent_msgs);
        return 1;
    }
    for (int i = 0; i < agents; i++) {
        fds[i] = -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid host: %s\n", host);
        free(fds);
        free(sent_msgs);
        return 1;
    }

    double t0 = now_sec();
    int connected = 0;
    int connect_errors = 0;

    for (int i = 0; i < agents; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            connect_errors++;
            continue;
        }
        if (set_nonblocking(s) < 0) {
            close(s);
            connect_errors++;
            continue;
        }
        int rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
        if (rc < 0 && errno != EINPROGRESS) {
            close(s);
            connect_errors++;
            continue;
        }
        fds[i] = s;
        connected++;
    }

    int total_sent = 0;
    int write_errors = 0;

    for (int m = 0; m < msgs_per_agent; m++) {
        for (int i = 0; i < agents; i++) {
            int s = fds[i];
            if (s < 0) continue;

            uint8_t pkt[128];
            size_t flen = 0;
            uint16_t mtype = (m == 0) ? AB_MSG_HELLO : AB_MSG_MSG;
            char body[32];
            int blen = snprintf(body, sizeof(body), "m=%d", m);
            if (blen < 0) blen = 0;

            if (ab_frame_encode(pkt, sizeof(pkt), &flen, mtype, 0u,
                                (uint64_t)(i + 1), (const uint8_t *)body,
                                (uint32_t)blen) != AB_PARSE_OK) {
                continue;
            }

            size_t off = 0;
            while (off < flen) {
                ssize_t n = send(s, pkt + off, flen - off, MSG_NOSIGNAL);
                if (n > 0) {
                    off += (size_t)n;
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                    /* Short spin for non-blocking socket flush */
                    continue;
                } else {
                    write_errors++;
                    close(s);
                    fds[i] = -1;
                    break;
                }
            }
            if (fds[i] >= 0 && off == flen) {
                sent_msgs[i]++;
                total_sent++;
            }
        }
    }

    for (int i = 0; i < agents; i++) {
        if (fds[i] >= 0) {
            close(fds[i]);
        }
    }

    double elapsed = now_sec() - t0;
    printf("load_sim finished: agents=%d connected=%d conn_err=%d total_frames_sent=%d write_err=%d elapsed=%.3fs\n",
           agents, connected, connect_errors, total_sent, write_errors, elapsed);

    free(fds);
    free(sent_msgs);
    return (connect_errors == 0 && write_errors == 0) ? 0 : 1;
}
