// The network side of the reader service: a TCP server on the console, asked from the PC with
// net_pull.py. The same file compiles for the host (POSIX sockets), where the tests run it
// over loopback. What may be asked is defined in bis_serve() (bis.c): the console only reads.
#pragma once
#include "nbe_platform.h"
#include "bis.h"

#define NBE_NET_PORT 28900
#define NET_WAIT_MS 16 // for how long the socket is polled before the idle hook is asked

// Bring the socket driver up (on the console: the SDK's BSD sockets). 0 when it is usable.
Result net_init(void);

// Listen for readers on `port`. A socket for net_accept(), or -1 with *rc set.
int net_listen_port(Result* rc, u16 port);
void net_close(int fd);

// A reader that has connected, or -1 when none has. Never blocks.
int net_accept(int listen_fd);

// One connection to a reader. Its link never waits in silence: whenever the reader makes it
// wait (it sends no request, or takes no data), `idle` is called every NET_WAIT_MS with the
// time since a byte last went through the socket. When idle returns false, the waiting
// function fails and bis_serve() gives up on the connection.
typedef struct {
    NbeLink link;     // what bis_serve() talks to
    int fd;
    bool (*idle)(void* user, u64 idle_ns);
    void* idle_user;
    u64 progress_ns;  // when a byte last went through the socket
    bool nonblocking; // send() and recv() return at once when they cannot do anything
} NetConn;

// `fd` is a socket that a reader connected to. Its link is ready for bis_serve().
void net_conn_init(NetConn* c, int fd);

// To be asked while the link waits. Without one, the link waits as long as it takes.
void net_conn_set_idle(NetConn* c, bool (*idle)(void* user, u64 idle_ns), void* user);
