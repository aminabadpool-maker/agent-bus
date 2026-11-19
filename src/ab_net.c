#define _POSIX_C_SOURCE 200809L
#include "ab_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static int epoll_update_events(int epoll_fd, ab_conn_t *c) {
    if (!c || c->fd < 0) {
        return -1;
    }
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | (c->want_write ? EPOLLOUT : 0u);
    ev.data.fd = c->fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void conn_reset(ab_server_t *srv, ab_conn_t *c) {
    if (!c) {
        return;
    }
    if (c->fd >= 0) {
        if (srv && srv->epoll_fd >= 0) {
            epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        }
        close(c->fd);
    }
    c->fd = -1;
    c->active = 0;
    c->want_write = 0;
    c->agent_id = 0;
    c->recv_len = 0;
    c->send_len = 0;
    c->send_off = 0;
}

static int epoll_add_read(int epoll_fd, int fd) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

static void handle_accept(ab_server_t *srv) {
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int cfd = accept(srv->listen_fd, (struct sockaddr *)&addr, &addr_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (cfd < 0 || cfd >= AB_MAX_CONNS || set_nonblocking(cfd) < 0) {
            close(cfd);
            continue;
        }

        if (epoll_add_read(srv->epoll_fd, cfd) < 0) {
            close(cfd);
            continue;
        }

        ab_conn_t *c = &srv->conns[cfd];
        memset(c, 0, sizeof(*c));
        c->fd = cfd;
        c->active = 1;
    }
}

static void flush_conn_send(ab_server_t *srv, ab_conn_t *c) {
    if (!c || !c->active || c->fd < 0) {
        return;
    }

    while (c->send_off < c->send_len) {
        ssize_t n = send(c->fd, c->send_buf + c->send_off,
                         c->send_len - c->send_off, MSG_NOSIGNAL);
        if (n > 0) {
            c->send_off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            conn_reset(srv, c);
            return;
        }
    }

    if (c->send_off == c->send_len) {
        c->send_off = 0;
        c->send_len = 0;
        if (c->want_write) {
            c->want_write = 0;
            epoll_update_events(srv->epoll_fd, c);
        }
    } else {
        if (!c->want_write) {
            c->want_write = 1;
            epoll_update_events(srv->epoll_fd, c);
        }
    }
}

int ab_conn_send(ab_server_t *srv, ab_conn_t *c, const uint8_t *data, size_t len) {
    if (!srv || !c || !c->active || c->fd < 0 || !data || len == 0u) {
        return -1;
    }

    if (c->send_len == c->send_off && c->send_len == 0u) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(c->fd, data + sent, len - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += (size_t)n;
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                conn_reset(srv, c);
                return -1;
            }
        }
        if (sent == len) {
            return 0;
        }
        size_t rem = len - sent;
        if (rem > AB_SEND_BUF_CAP) {
            conn_reset(srv, c);
            return -1;
        }
        memcpy(c->send_buf, data + sent, rem);
        c->send_len = rem;
        c->send_off = 0;
        c->want_write = 1;
        epoll_update_events(srv->epoll_fd, c);
        return 0;
    }

    if (c->send_off > 0u) {
        memmove(c->send_buf, c->send_buf + c->send_off, c->send_len - c->send_off);
        c->send_len -= c->send_off;
        c->send_off = 0;
    }

    if (c->send_len + len > AB_SEND_BUF_CAP) {
        conn_reset(srv, c);
        return -1;
    }

    memcpy(c->send_buf + c->send_len, data, len);
    c->send_len += len;
    flush_conn_send(srv, c);
    return 0;
}

static void process_conn_buffer(ab_server_t *srv, ab_conn_t *c) {
    size_t offset = 0;

    while (c->recv_len - offset > 0u) {
        ab_header_t hdr;
        const uint8_t *payload = NULL;
        size_t consumed = 0;

        int rc = ab_frame_decode(c->recv_buf + offset, c->recv_len - offset,
                                 &hdr, &payload, &consumed);
        if (rc == AB_PARSE_NEED_MORE) {
            break;
        }
        if (rc != AB_PARSE_OK) {
            conn_reset(srv, c);
            return;
        }

        if (hdr.type == AB_MSG_HELLO && hdr.agent_id != 0ull) {
            c->agent_id = hdr.agent_id;
        }

        if (srv->on_frame) {
            srv->on_frame(c, &hdr, payload, srv->user_ctx);
        }

        offset += consumed;
        if (!c->active) {
            return;
        }
    }

    if (offset > 0u && offset < c->recv_len) {
        memmove(c->recv_buf, c->recv_buf + offset, c->recv_len - offset);
    }
    c->recv_len -= offset;
}

static void handle_client_read(ab_server_t *srv, int fd) {
    if (fd < 0 || fd >= AB_MAX_CONNS) {
        return;
    }
    ab_conn_t *c = &srv->conns[fd];
    if (!c->active) {
        return;
    }

    for (;;) {
        if (c->recv_len == AB_RECV_BUF_CAP) {
            conn_reset(srv, c);
            return;
        }

        ssize_t n = recv(c->fd, c->recv_buf + c->recv_len,
                         AB_RECV_BUF_CAP - c->recv_len, 0);
        if (n > 0) {
            c->recv_len += (size_t)n;
            process_conn_buffer(srv, c);
            if (!c->active) {
                return;
            }
        } else if (n == 0) {
            conn_reset(srv, c);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            conn_reset(srv, c);
            return;
        }
    }
}

int ab_server_init(ab_server_t *srv, const char *host, uint16_t port,
                   ab_on_frame_fn on_frame, void *user_ctx) {
    if (!srv || !host || port == 0u) {
        return -1;
    }

    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;
    srv->epoll_fd = -1;
    srv->on_frame = on_frame;
    srv->user_ctx = user_ctx;

    for (int i = 0; i < AB_MAX_CONNS; i++) {
        srv->conns[i].fd = -1;
        srv->conns[i].active = 0;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        return -1;
    }

    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(lfd);
        return -1;
    }

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(lfd);
        return -1;
    }

    if (set_nonblocking(lfd) < 0 || listen(lfd, 4096) < 0) {
        close(lfd);
        return -1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        close(lfd);
        return -1;
    }

    if (epoll_add_read(epfd, lfd) < 0) {
        close(epfd);
        close(lfd);
        return -1;
    }

    srv->listen_fd = lfd;
    srv->epoll_fd = epfd;
    srv->running = 1;
    return 0;
}

int ab_server_run(ab_server_t *srv) {
    if (!srv || srv->listen_fd < 0 || srv->epoll_fd < 0) {
        return -1;
    }

    struct epoll_event events[256];
    srv->running = 1;

    while (srv->running) {
        int nfds = epoll_wait(srv->epoll_fd, events, 256, 250);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == srv->listen_fd) {
                handle_accept(srv);
            } else if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                if (fd >= 0 && fd < AB_MAX_CONNS) {
                    conn_reset(srv, &srv->conns[fd]);
                }
            } else {
                if (ev & EPOLLIN) {
                    handle_client_read(srv, fd);
                }
                if (fd >= 0 && fd < AB_MAX_CONNS && srv->conns[fd].active && (ev & EPOLLOUT)) {
                    flush_conn_send(srv, &srv->conns[fd]);
                }
            }
        }
    }

    return 0;
}

void ab_server_stop(ab_server_t *srv) {
    if (srv) {
        srv->running = 0;
    }
}

void ab_server_close(ab_server_t *srv) {
    if (!srv) {
        return;
    }
    srv->running = 0;
    for (int i = 0; i < AB_MAX_CONNS; i++) {
        if (srv->conns[i].active) {
            conn_reset(srv, &srv->conns[i]);
        }
    }
    if (srv->epoll_fd >= 0) {
        close(srv->epoll_fd);
        srv->epoll_fd = -1;
    }
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
}

ab_conn_t *ab_server_find_by_agent(ab_server_t *srv, uint64_t agent_id) {
    if (!srv || agent_id == 0ull) {
        return NULL;
    }
    for (int i = 0; i < AB_MAX_CONNS; i++) {
        if (srv->conns[i].active && srv->conns[i].agent_id == agent_id) {
            return &srv->conns[i];
        }
    }
    return NULL;
}
