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

static int wait_connect_complete(int fd, int timeout_ms) {
    struct pollfd pfd;
    int remaining_ms = timeout_ms;

    for (;;) {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        double t_start = now_sec();
        int prc = poll(&pfd, 1, remaining_ms > 0 ? remaining_ms : 0);
        int elapsed_ms = (int)((now_sec() - t_start) * 1000.0);
        if (elapsed_ms > 0) {
            remaining_ms -= elapsed_ms;
        }

        if (prc > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                return -1;
            }
            if (err == EINPROGRESS && remaining_ms > 0) {
                continue;
            }
            if (err != 0) {
                errno = err;
                return -1;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = ECONNRESET;
                return -1;
            }
            return 0;
        }
        if (prc == 0 || remaining_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
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
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        break;
    }
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

    for (int i = 0; i < agents; i++) {
        if (i > 0 && (i % 256) == 0) {
            struct timespec pace = { .tv_sec = 0, .tv_nsec = 2000000L };
            nanosleep(&pace, NULL);
        }

        const int max_attempts = 32;
        int ok = 0;
        for (int attempt = 0; attempt < max_attempts; attempt++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) {
                if (attempt == max_attempts - 1) {
                    if (connect_errors < 5) {
                        fprintf(stderr, "conn_err[%d] at agent %d: %s (errno=%d)\n",
                                connect_errors, i, strerror(errno), errno);
                    }
                    connect_errors++;
                }
                continue;
            }
            if (set_nonblocking(s) < 0) {
                int saved_err = errno;
                close(s);
                if (attempt == max_attempts - 1) {
                    if (connect_errors < 5) {
                        fprintf(stderr, "conn_err[%d] at agent %d: %s (errno=%d)\n",
                                connect_errors, i, strerror(saved_err), saved_err);
                    }
                    connect_errors++;
                }
                continue;
            }
            int rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
            if (rc < 0) {
                if (errno != EINPROGRESS || wait_connect_complete(s, 5000) < 0) {
                    int saved_err = errno;
                    close(s);
                    if (saved_err == ECONNREFUSED && attempt + 1 < max_attempts) {
                        long delay_ms = 5L * (attempt + 1);
                        if (delay_ms > 50L) {
                            delay_ms = 50L;
                        }
                        struct timespec ts = {
                            .tv_sec = delay_ms / 1000L,
                            .tv_nsec = (delay_ms % 1000L) * 1000000L
                        };
                        nanosleep(&ts, NULL);
                        continue;
                    }
                    if (saved_err == ECONNREFUSED) {
                        refused_errors++;
                    }
                    if (connect_errors < 5) {
                        fprintf(stderr, "conn_err[%d] at agent %d: %s (errno=%d)\n",
                                connect_errors, i, strerror(saved_err), saved_err);
                    }
                    connect_errors++;
                    break;
                }
            }
            fds[i] = s;
            connected++;
            ok = 1;
            break;
        }
        (void)ok;
    }

    int total_sent = 0;
    int write_errors = 0;

    for (int m = 0; m < msgs_per_agent; m++) {
        for (int i = 0; i < agents; i++) {
            int s = fds[i];
            if (s < 0) continue;

            drain_socket_recv(s);

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
