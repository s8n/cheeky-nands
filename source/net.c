#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "net.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifdef NBE_HOST_TEST
#include <signal.h>
#endif

// One send() call carries at most this much of a larger block. The bytes of a chunk are
// handed over in pieces, so that a send() which takes only part of it is nothing special.
#define NET_PIECE (128ULL * 1024)

Result net_init(void) {
#ifdef NBE_HOST_TEST
    signal(SIGPIPE, SIG_IGN); // a reader that went away shows up as an error, not as a signal
    return 0;
#else
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.tcp_tx_buf_size = 0x40000;      // 256 KiB in flight; the default is 32 KiB
    cfg.tcp_tx_buf_max_size = 0x400000; // 4 MiB
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) rc = socketInitialize(NULL); // then as the SDK would have it
    return rc;
#endif
}

int net_listen_port(Result* rc, u16 port) {
    *rc = NBE_RC_NET;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;

    const int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    *rc = 0;
    return fd;
}

void net_close(int fd) {
    if (fd >= 0) close(fd);
}

int net_accept(int listen_fd) {
    struct pollfd waiting = { listen_fd, POLLIN, 0 };
    if (poll(&waiting, 1, 0) <= 0) return -1; // nothing is waiting
    return accept(listen_fd, NULL, NULL);
}

// ---------------------------------------------------------------------------------------------
// One connection
// ---------------------------------------------------------------------------------------------

static Result conn_send(void* user, const void* data, size_t len);
static int conn_recv(void* user, char* line, size_t cap);

// Make send() and recv() return at once when they cannot do anything, so that all the waiting
// is done in wait_ready(), where the idle hook is asked. The flag goes on alone: the console's
// fcntl takes O_NONBLOCK and nothing else.
static Result set_nonblocking(int fd) {
    return fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ? NBE_RC_NET : 0;
}

void net_conn_init(NetConn* c, int fd) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->link.user = c;
    c->link.send_all = conn_send;
    c->link.recv_line = conn_recv;
    c->progress_ns = bis_now_ns();
    c->nonblocking = set_nonblocking(fd) == 0;
}

void net_conn_set_idle(NetConn* c, bool (*idle)(void* user, u64 idle_ns), void* user) {
    c->idle = idle;
    c->idle_user = user;
}

// Wait until the socket can do `events` (POLLIN or POLLOUT), asking the idle hook after every
// NET_WAIT_MS that passes without it.
// 1: it can. 0: the idle hook gave up. -1: poll() failed.
static int wait_ready(NetConn* c, short events) {
    for (;;) {
        struct pollfd waiting = { c->fd, events, 0 };
        const int n = poll(&waiting, 1, NET_WAIT_MS);
        if (n > 0) return 1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c->idle && !c->idle(c->idle_user, bis_now_ns() - c->progress_ns)) return 0;
    }
}

// "Nothing went through just now": the socket is full or empty (EAGAIN), or BSD's way of
// saying that the network interface's queue is full (ENOBUFS). Both pass.
static bool nothing_moved(void) { return errno == EAGAIN || errno == ENOBUFS; }

// All of `len`, or NBE_RC_NET.
static Result conn_send(void* user, const void* data, size_t len) {
    NetConn* c = user;
    const u8* p = data;
    while (len > 0) {
        const size_t piece = len < NET_PIECE ? len : (size_t)NET_PIECE;
        const ssize_t n = send(c->fd, p, piece, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!nothing_moved()) return NBE_RC_NET;
            if (wait_ready(c, POLLOUT) != 1) return NBE_RC_NET; // the reader takes nothing
            continue;
        }
        if (n == 0) return NBE_RC_NET;
        p += (size_t)n;
        len -= (size_t)n;
        c->progress_ns = bis_now_ns();
    }
    return 0;
}

// One line, without its '\n' (and without any '\r').
// 1: a line. 0: the other side closed before a line. -1: an error, a line too long for the
// buffer, the idle hook giving up, or a close in the middle of a line.
static int conn_recv(void* user, char* line, size_t cap) {
    NetConn* c = user;
    size_t n = 0;
    for (;;) {
        char ch;
        const ssize_t got = recv(c->fd, &ch, 1, 0); // request lines are short: one byte at a time
        if (got < 0) {
            if (errno == EINTR) continue;
            if (!nothing_moved()) return -1;
            if (wait_ready(c, POLLIN) != 1) return -1; // no request is coming
            continue;
        }
        if (got == 0) return n == 0 ? 0 : -1; // the reader closed the connection
        c->progress_ns = bis_now_ns();
        if (ch == '\n') {
            line[n] = '\0';
            return 1;
        }
        if (n + 1 >= cap) return -1;
        if (ch != '\r') line[n++] = ch;
    }
}
