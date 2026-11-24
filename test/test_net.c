// The reader service on the host, over real sockets, for net_pull.py to talk to
// (test/net_test.sh):
//
//     test_net <port> [drop-after=<bytes>] [stall-after=<ms>] <id>=<file>:<size>:<seed> ...
//
// Each <id> gets a pseudo-random image of <size> bytes at <file>. With drop-after, the first
// READ's connection is cut once that many bytes were sent, the way a lost connection would be.
// With stall-after, a reader that lets that many milliseconds pass without a byte going
// through is let go of, the way the app does after two minutes. Answers until it is killed.
// Prints one line per connection: what it last handled, and how the connection ended.
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include "net.h"

typedef struct {
    NetConn conn;
    size_t sent, limit;
    int lines; // request lines read on this connection
} Wrap;

static size_t g_drop;      // bytes after which the first READ's connection is cut
static bool g_drop_used;
static u64 g_stall_ns;     // how long a reader may be silent before it is let go of

static Result wrap_send(void* user, const void* data, size_t len) {
    Wrap* w = user;
    if (w->limit != 0 && w->sent + len > w->limit) return NBE_RC_NET; // the connection "drops"
    const Result rc = w->conn.link.send_all(&w->conn, data, len);
    w->sent += len;
    return rc;
}

static int wrap_recv(void* user, char* line, size_t cap) {
    Wrap* w = user;
    const int got = w->conn.link.recv_line(&w->conn, line, cap);
    if (got == 1 && w->lines++ == 0 && !g_drop_used && strncmp(line, "READ", 4) == 0) {
        w->limit = g_drop; // the first READ is the one whose connection drops
        g_drop_used = true;
    }
    return got;
}

// What the app does about a silent reader, in short: lets it go.
static bool stall_idle(void* user, u64 idle_ns) {
    (void)user;
    return idle_ns < g_stall_ns;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: test_net <port> [drop-after=<bytes>] [stall-after=<ms>] "
                        "<id>=<file>:<size>:<seed> ...\n");
        return 2;
    }
    const int port = atoi(argv[1]);
    BisTable t;
    BisServeResult r;

    shim_init("/nonexistent"); // nothing is written anywhere: no SD card is needed
    for (int i = 2; i < argc; i++) {
        unsigned id;
        char file[600];
        unsigned long long size, seed;
        if (strncmp(argv[i], "drop-after=", 11) == 0) {
            g_drop = (size_t)strtoull(argv[i] + 11, NULL, 0);
            continue;
        }
        if (strncmp(argv[i], "stall-after=", 12) == 0) {
            g_stall_ns = (u64)strtoull(argv[i] + 12, NULL, 0) * 1000000ULL;
            continue;
        }
        if (sscanf(argv[i], "%u=%599[^:]:%llu:%llu", &id, file, &size, &seed) != 4) {
            fprintf(stderr, "bad argument: %s\n", argv[i]);
            return 2;
        }
        shim_make_image(file, (s64)size, seed, NULL, 0);
        shim_storage_set(id, file);
    }
    bis_scan(&t);

    Result rc = net_init();
    if (R_FAILED(rc)) {
        fprintf(stderr, "net_init failed: %08x\n", (unsigned)rc);
        return 1;
    }
    const int listen_fd = net_listen_port(&rc, (u16)port);
    if (listen_fd < 0) {
        fprintf(stderr, "listening on port %d failed\n", port);
        return 1;
    }
    printf("listening on %d\n", port);
    fflush(stdout);

    for (;;) {
        struct pollfd waiting = { listen_fd, POLLIN, -1 };
        (void)poll(&waiting, 1, -1);
        const int fd = net_accept(listen_fd);
        if (fd < 0) continue;

        Wrap w;
        net_conn_init(&w.conn, fd);
        w.sent = 0;
        w.limit = 0;
        w.lines = 0;
        if (g_stall_ns != 0) net_conn_set_idle(&w.conn, stall_idle, NULL);
        NbeLink link = { &w, wrap_send, wrap_recv };
        bis_serve(&link, &t, NULL, &r);
        net_close(fd);
        printf("served \"%s\" rc=%08x stage=%s sent=%lld\n", r.request, (unsigned)r.rc, r.stage,
               (long long)r.sent);
        // A refused request leaves the connection as it was; anything else that went wrong
        // means the reader was left short, and the connection was dropped.
        printf("%s\n", R_FAILED(r.rc) && !r.refused ? "dropped" : "closed");
        fflush(stdout);
    }
}
