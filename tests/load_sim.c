#define _POSIX_C_SOURCE 200809L
#include "ab_frame.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CONNECT_BATCH_SIZE 128
#define MAX_CONNECT_ROUNDS 4

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

static void drain_socket_recv(int fd) {
    uint8_t buf[4096];
    for (int k = 0; k < 8; k++) {
        ssize_t r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r > 0) {
            continue;
        }
        if (r == 0) {
            errno = ECONNRESET;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        }
        break;
    }
}

static void sleep_ms(long ms) {
    if (ms <= 0) return;
    struct timespec ts = {
        .tv_sec = ms / 1000L,
        .tv_nsec = (ms % 1000L) * 1000000L
    };
    nanosleep(&ts, NULL);
}

static int initiate_nonblocking_connect(int *fd_out, const struct sockaddr_in *addr) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    if (set_nonblocking(s) < 0) {
        int saved = errno;
        close(s);
        errno = saved;
        return -1;
    }
    int rc = connect(s, (const struct sockaddr *)addr, sizeof(*addr));
    if (rc == 0) {
        *fd_out = s;
        return 1;
    }
    if (errno == EINPROGRESS) {
        *fd_out = s;
        return 0;
    }
    int saved = errno;
    close(s);
    errno = saved;
    return -1;
}

static void connect_slice_batched(const struct sockaddr_in *addr,
                                  int *fds,
                                  int start_idx,
                                  int count,
                                  int *connected_out,
                                  int *conn_err_out,
                                  int *refused_out) {
    int need_connect[CONNECT_BATCH_SIZE];
    int pending_idx[CONNECT_BATCH_SIZE];
    int need_cnt = 0;

    for (int j = 0; j < count; j++) {
        fds[start_idx + j] = -1;
        need_connect[need_cnt++] = start_idx + j;
    }

    int last_err = 0;
    int last_refused = 0;

    for (int round = 0; round < MAX_CONNECT_ROUNDS && need_cnt > 0; round++) {
        int pending_cnt = 0;
        int next_need[CONNECT_BATCH_SIZE];
        int next_need_cnt = 0;

        for (int k = 0; k < need_cnt; k++) {
            int idx = need_connect[k];
            int fd = -1;
            int st = initiate_nonblocking_connect(&fd, addr);
            if (st == 1) {
                fds[idx] = fd;
                (*connected_out)++;
            } else if (st == 0) {
                fds[idx] = fd;
                pending_idx[pending_cnt++] = idx;
            } else {
                last_err = errno;
                if (errno == ECONNREFUSED) {
                    last_refused++;
                }
                next_need[next_need_cnt++] = idx;
            }
        }

        if (pending_cnt > 0) {
            struct pollfd pfds[CONNECT_BATCH_SIZE];
            int remaining = pending_cnt;
            double deadline = now_sec() + 0.25;

            while (remaining > 0 && now_sec() < deadline) {
                int np = 0;
                for (int k = 0; k < pending_cnt; k++) {
                    int idx = pending_idx[k];
                    if (fds[idx] >= 0) {
                        pfds[np].fd = fds[idx];
                        pfds[np].events = POLLOUT;
                        pfds[np].revents = 0;
                        np++;
                    }
                }
                if (np == 0) {
                    break;
                }

                int rem_ms = (int)((deadline - now_sec()) * 1000.0);
                if (rem_ms <= 0) {
                    break;
                }
                if (rem_ms > 25) {
                    rem_ms = 25;
                }

                int prc = poll(pfds, (nfds_t)np, rem_ms);
                if (prc < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (prc == 0) {
                    continue;
                }

                int cursor = 0;
                for (int k = 0; k < pending_cnt; k++) {
                    int idx = pending_idx[k];
                    if (fds[idx] < 0) {
                        continue;
                    }
                    short rev = pfds[cursor++].revents;
                    if (rev == 0) {
                        continue;
                    }
                    int err = 0;
                    socklen_t elen = sizeof(err);
                    if (getsockopt(fds[idx], SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
                        err = errno ? errno : EIO;
                    }
                    if (err == EINPROGRESS) {
                        continue;
                    }
                    if (err == 0 && !(rev & (POLLERR | POLLHUP | POLLNVAL))) {
                        (*connected_out)++;
                        remaining--;
                    } else {
                        close(fds[idx]);
                        fds[idx] = -1;
                        remaining--;
                        last_err = err;
                        if (err == ECONNREFUSED) {
                            last_refused++;
                        }
                        next_need[next_need_cnt++] = idx;
                    }
                }
            }

            for (int k = 0; k < pending_cnt; k++) {
                int idx = pending_idx[k];
                if (fds[idx] >= 0) {
                    close(fds[idx]);
                    fds[idx] = -1;
                    last_err = ETIMEDOUT;
                    next_need[next_need_cnt++] = idx;
                }
            }
        }

        need_cnt = next_need_cnt;
        for (int k = 0; k < need_cnt; k++) {
            need_connect[k] = next_need[k];
        }

        if (need_cnt > 0 && round + 1 < MAX_CONNECT_ROUNDS) {
            sleep_ms(3L);
        }
    }

    for (int k = 0; k < need_cnt; k++) {
        int idx = need_connect[k];
        fds[idx] = -1;
        (*conn_err_out)++;
        if ((*conn_err_out) <= 5) {
            fprintf(stderr, "conn_err[%d] at agent %d: %s (errno=%d)\n",
                    (*conn_err_out) - 1, idx,
                    last_err ? strerror(last_err) : "connect failed", last_err);
        }
    }
    (*refused_out) += last_refused;
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
    int refused_errors = 0;

    for (int base = 0; base < agents; base += CONNECT_BATCH_SIZE) {
        int slice = agents - base;
        if (slice > CONNECT_BATCH_SIZE) {
            slice = CONNECT_BATCH_SIZE;
        }
        connect_slice_batched(&addr, fds, base, slice,
                              &connected, &connect_errors, &refused_errors);
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
            int eagain_spins = 0;
            while (off < flen) {
                ssize_t n = send(s, pkt + off, flen - off, MSG_NOSIGNAL);
                if (n > 0) {
                    off += (size_t)n;
                } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    eagain_spins++;
                    if (eagain_spins > 64) {
                        write_errors++;
                        close(s);
                        fds[i] = -1;
                        break;
                    }
                    struct pollfd pfd = { .fd = s, .events = POLLOUT, .revents = 0 };
                    poll(&pfd, 1, 10);
                } else if (n < 0 && errno == EINTR) {
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
        for (int i = 0; i < agents; i++) {
            if (fds[i] >= 0) {
                drain_socket_recv(fds[i]);
            }
        }
    }

    for (int i = 0; i < agents; i++) {
        if (fds[i] >= 0) {
            drain_socket_recv(fds[i]);
            close(fds[i]);
            fds[i] = -1;
        }
    }

    double dt = now_sec() - t0;
    printf("load_sim finished: agents=%d connected=%d conn_err=%d total_frames_sent=%d write_err=%d elapsed=%.3fs\n",
           agents, connected, connect_errors, total_sent, write_errors, dt);

    free(fds);
    free(sent_msgs);
    if (refused_errors > 0) {
        fprintf(stderr, "refused_errors=%d\n", refused_errors);
    }
    return 0;
}
