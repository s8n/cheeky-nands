// Stand-in for source/net.c in the scripted UI runs: no sockets. The test queues request lines;
// net_accept() hands the app a reader for them, and everything the app sends is kept for the
// test to look at. The protocol logic that runs is the real one (bis_serve), and so is what
// the app does about a waiting connection: a reader's silence (ui_net_silent) is told to the
// app's idle hook a second at a time, until the app lets the reader go.
// An empty line in the queue ends the reader's connection; the next entry is asked over a new one.
#include <stdio.h>
#include <string.h>
#include "net.h"

#define STEP_NS 1000000000ULL

static char g_requests[16][96];
static int g_request_n, g_request_i;
static char g_sent[1 << 18];
static size_t g_sent_len;
static int g_listen_calls;
static bool g_nonblocking = true;
static int g_call;               // link calls made on this connection, the next one included
static int g_silent_before;      // the reader is silent before this call of the next connection
static u64 g_silence;            // ... for this long

Result net_init(void) { return 0; }

int net_listen_port(Result* rc, u16 port) {
    (void)port;
    g_listen_calls++;
    *rc = 0;
    return 7;
}

void net_close(int fd) { (void)fd; }

int net_accept(int listen_fd) {
    (void)listen_fd;
    return g_request_i < g_request_n ? 8 : -1;
}

// The reader is silent before the call it was scripted to be silent before: the app's idle
// hook hears of it a second at a time. False when the app let the reader go.
static bool silence_first(NetConn* c) {
    g_call++;
    if (g_call != g_silent_before) return true;
    g_silent_before = 0;
    const u64 silence = g_silence;
    if (!c->idle) return true;
    for (u64 t = STEP_NS; t <= silence; t += STEP_NS)
        if (!c->idle(c->idle_user, t)) return false;
    return true;
}

static Result stub_send(void* user, const void* data, size_t len) {
    NetConn* c = user;
    if (!silence_first(c)) return NBE_RC_NET;
    if (g_sent_len + len >= sizeof g_sent) return NBE_RC_NET;
    memcpy(g_sent + g_sent_len, data, len);
    g_sent_len += len;
    return 0;
}

static int stub_recv(void* user, char* line, size_t cap) {
    NetConn* c = user;
    // What the reader does is decided before the silence: a reader that is let go of during
    // it does not get to do it.
    const bool closed = g_request_i >= g_request_n || g_requests[g_request_i][0] == '\0';
    if (g_request_i < g_request_n) g_request_i++;
    if (!silence_first(c)) return -1;
    if (closed) return 0; // the reader closed the connection
    snprintf(line, cap, "%s", g_requests[g_request_i - 1]);
    return 1;
}

void net_conn_init(NetConn* c, int fd) {
    (void)fd;
    memset(c, 0, sizeof *c);
    c->fd = 8;
    c->link.user = c;
    c->link.send_all = stub_send;
    c->link.recv_line = stub_recv;
    c->nonblocking = g_nonblocking;
    g_call = 0;
}

void net_conn_set_idle(NetConn* c, bool (*idle)(void* user, u64 idle_ns), void* user) {
    c->idle = idle;
    c->idle_user = user;
}

// ---- what the test scripts ---------------------------------------------------------------------

void ui_net_request(const char* line) {
    if (g_request_n < (int)(sizeof g_requests / sizeof g_requests[0]))
        snprintf(g_requests[g_request_n++], sizeof g_requests[0], "%s", line);
}

void ui_net_silent(int before_call, int seconds) {
    g_silent_before = before_call;
    g_silence = (u64)seconds * STEP_NS;
}

void ui_net_nonblocking(bool yes) { g_nonblocking = yes; }

const char* ui_net_sent(void) { return g_sent; }

int ui_net_listen_calls(void) { return g_listen_calls; }
