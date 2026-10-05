// The control socket: line-based commands from other programs, and replies
// that may carry binary data. Nonblocking; the owner polls it.

#ifndef SALLY_CONTROL_H
#define SALLY_CONTROL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct Control Control;
typedef struct ControlClient ControlClient;

// Listens on `addr`: a port number for TCP on 127.0.0.1, or else the path
// of a Unix socket. Returns NULL with a message in `err` on failure.
Control *control_open(const char *addr, char *err, size_t errlen);
void control_close(Control *c);

// Accepts connections, reads what has arrived and calls `line` for each
// complete line, without its newline. Then sends what is waiting.
void control_poll(Control *c, void (*line)(void *ctx, ControlClient *client, char *text), void *ctx);
// Sleeps until a client connects or sends something, a client with output
// waiting can take more, or `timeout` seconds pass (forever if negative).
void control_wait(Control *c, double timeout);

// Queues output to a client. Nothing is ever sent in part: a client that
// falls behind gets its bytes later, in order.
void control_send(ControlClient *client, const void *data, size_t n);
void control_printf(ControlClient *client, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Clients that asked for every frame.
void control_set_streaming(ControlClient *client, bool on);
bool control_streaming(Control *c);
// Sends `header` and `data` to each streaming client, skipping clients
// still behind with earlier output.
void control_broadcast(Control *c, const char *header, const void *data, size_t n);

#endif
