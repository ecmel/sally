#include "control.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    LINE_MAX_LEN = 4096,
    // A streaming client with more than this waiting misses frames.
    BACKLOG_MAX = 512 * 1024,
};

struct ControlClient {
    int fd;
    bool dead, streaming;
    char in[LINE_MAX_LEN];
    size_t nin;
    uint8_t *out;
    size_t nout, cap, sent;
    ControlClient *next;
};

struct Control {
    int fd;
    char path[sizeof ((struct sockaddr_un *)0)->sun_path];  // a Unix socket's, or empty
    ControlClient *clients;
};

static void nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

Control *control_open(const char *addr, char *err, size_t errlen) {
    Control *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    char *end;
    long port = strtol(addr, &end, 10);
    if (*addr && !*end) {
        if (port < 1 || port > 65535) {
            snprintf(err, errlen, "%s is not a port number", addr);
            free(c);
            return NULL;
        }
        struct sockaddr_in sin = {.sin_len = sizeof sin, .sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        c->fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        if (c->fd >= 0) setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (c->fd < 0 || bind(c->fd, (struct sockaddr *)&sin, sizeof sin) < 0) goto fail;
    } else {
        struct sockaddr_un sun = {.sun_family = AF_UNIX};
        if (strlen(addr) >= sizeof sun.sun_path) {
            snprintf(err, errlen, "socket path too long: %s", addr);
            free(c);
            return NULL;
        }
        strcpy(sun.sun_path, addr);
        sun.sun_len = (uint8_t)SUN_LEN(&sun);
        c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (c->fd < 0) goto fail;
        unlink(addr);  // left over from a run that did not close it
        if (bind(c->fd, (struct sockaddr *)&sun, SUN_LEN(&sun)) < 0) goto fail;
        strcpy(c->path, addr);
    }
    if (listen(c->fd, 8) < 0) goto fail;
    nonblocking(c->fd);
    return c;
fail:
    snprintf(err, errlen, "%s: %s", addr, strerror(errno));
    if (c->fd >= 0) close(c->fd);
    if (c->path[0]) unlink(c->path);
    free(c);
    return NULL;
}

static void client_free(ControlClient *cl) {
    close(cl->fd);
    free(cl->out);
    free(cl);
}

void control_close(Control *c) {
    if (!c) return;
    while (c->clients) {
        ControlClient *next = c->clients->next;
        client_free(c->clients);
        c->clients = next;
    }
    close(c->fd);
    if (c->path[0]) unlink(c->path);
    free(c);
}

static void flush(ControlClient *cl) {
    while (!cl->dead && cl->sent < cl->nout) {
        ssize_t n = write(cl->fd, cl->out + cl->sent, cl->nout - cl->sent);
        if (n > 0) {
            cl->sent += n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) cl->dead = true;
            break;
        }
    }
    if (cl->sent == cl->nout) cl->sent = cl->nout = 0;
}

void control_send(ControlClient *cl, const void *data, size_t n) {
    if (cl->dead) return;
    if (cl->sent && cl->nout + n > cl->cap) {
        memmove(cl->out, cl->out + cl->sent, cl->nout - cl->sent);
        cl->nout -= cl->sent;
        cl->sent = 0;
    }
    if (cl->nout + n > cl->cap) {
        size_t cap = cl->cap ? cl->cap : 4096;
        while (cap < cl->nout + n) cap *= 2;
        uint8_t *out = realloc(cl->out, cap);
        if (!out) {
            cl->dead = true;
            return;
        }
        cl->out = out;
        cl->cap = cap;
    }
    memcpy(cl->out + cl->nout, data, n);
    cl->nout += n;
    flush(cl);
}

void control_printf(ControlClient *cl, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    control_send(cl, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

void control_set_streaming(ControlClient *cl, bool on) { cl->streaming = on; }

bool control_streaming(Control *c) {
    for (ControlClient *cl = c->clients; cl; cl = cl->next)
        if (cl->streaming && !cl->dead) return true;
    return false;
}

void control_broadcast(Control *c, const char *header, const void *data, size_t n) {
    for (ControlClient *cl = c->clients; cl; cl = cl->next) {
        if (!cl->streaming || cl->dead || cl->nout - cl->sent > BACKLOG_MAX) continue;
        control_send(cl, header, strlen(header));
        control_send(cl, data, n);
    }
}

static void accept_clients(Control *c) {
    for (;;) {
        int fd = accept(c->fd, NULL, NULL);
        if (fd < 0) return;
        ControlClient *cl = calloc(1, sizeof *cl);
        if (!cl) {
            close(fd);
            return;
        }
        nonblocking(fd);
        int one = 1, size = 1 << 20;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        // Room for several frames, so a stream does not wait on each poll.
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);  // fails harmlessly on Unix sockets
        cl->fd = fd;
        cl->next = c->clients;
        c->clients = cl;
    }
}

static void read_lines(ControlClient *cl, void (*line)(void *, ControlClient *, char *), void *ctx) {
    for (;;) {
        ssize_t n = read(cl->fd, cl->in + cl->nin, sizeof cl->in - cl->nin);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) cl->dead = true;
            return;
        }
        if (n == 0) {
            cl->dead = true;
            return;
        }
        cl->nin += n;
        char *start = cl->in, *nl;
        while (!cl->dead && (nl = memchr(start, '\n', cl->in + cl->nin - start))) {
            *nl = 0;
            if (nl > start && nl[-1] == '\r') nl[-1] = 0;
            line(ctx, cl, start);
            start = nl + 1;
        }
        cl->nin -= start - cl->in;
        memmove(cl->in, start, cl->nin);
        if (cl->nin == sizeof cl->in) {
            control_printf(cl, "error line too long\n");
            cl->nin = 0;
        }
    }
}

void control_poll(Control *c, void (*line)(void *, ControlClient *, char *), void *ctx) {
    accept_clients(c);
    for (ControlClient *cl = c->clients; cl; cl = cl->next) {
        if (!cl->dead) read_lines(cl, line, ctx);
        flush(cl);
    }
    for (ControlClient **p = &c->clients; *p;) {
        ControlClient *cl = *p;
        if (cl->dead) {
            *p = cl->next;
            client_free(cl);
        } else {
            p = &cl->next;
        }
    }
}
