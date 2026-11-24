// Host tests for the transport of the network reader (source/net.c): what happens while a
// reader makes the app wait. The four socket calls the transport makes (fcntl, poll, send,
// recv) are replaced by scripted ones through the linker's --wrap, so that "not yet", partial
// sends, errors and giving up are exact. The clock is the shim's virtual one: the scripted
// idle hook moves it a second per call, so idle times can be checked to the second.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "shim.h"
#include "net.h"

static int g_checks, g_failures;

#define CHECK(cond, ...) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

#define SECOND 1000000000ULL
#define NS(x) ((unsigned long long)(x))

// ---- the scripted socket ----------------------------------------------------------------------

static int g_flag_sets;        // fcntl(F_SETFL) calls that ask for O_NONBLOCK
static bool g_no_nonblock;     // the socket cannot be made non-blocking
static bool g_poll_fails;     // poll() answers EIO
static int g_wait_polls;      // polls that pass without the socket after every "not yet"
static int g_not_ready;       // ... of which this many still have to
static int g_timeouts;        // polls that passed without the socket, in all
static int g_polls;           // every poll, those included
static short g_last_events;    // what the last poll waited for
static int g_send_not_yet;     // send() calls that still answer EAGAIN
static bool g_send_enobufs;    // ... one that answers ENOBUFS
static int g_send_interrupted; // ... that answer EINTR
static bool g_send_broken;     // send() answers EPIPE
static bool g_send_nothing;    // send() answers 0
static size_t g_send_max;      // the most one send() takes (0: everything)
static char g_sent[1 << 20];
static size_t g_sent_len;
static int g_send_calls;
static const char* g_in;       // what recv() gives, one byte per call
static size_t g_in_pos;
static int g_recv_not_yet;     // recv() calls that still answer EAGAIN
static const char* g_in_plan;  // per recv() call: 'w' answers EAGAIN, '.' gives the next byte
static size_t g_in_plan_pos;   // (when the plan is used up, every call gives a byte)
static bool g_in_broken;       // recv() answers ECONNRESET once the input is used up

int __wrap_fcntl(int fd, int cmd, ...) {
    (void)fd;
    va_list args;
    va_start(args, cmd);
    const int arg = va_arg(args, int);
    va_end(args);
    CHECK(cmd == F_SETFL && arg == O_NONBLOCK, "fcntl(%d, 0x%x): asked for more than O_NONBLOCK",
          cmd, arg);
    if (g_no_nonblock) {
        errno = EINVAL;
        return -1;
    }
    g_flag_sets++;
    return 0;
}

int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    CHECK(nfds == 1 && (timeout == NET_WAIT_MS || timeout == 0), "poll over %zu sockets with a %d ms wait",
          (size_t)nfds, timeout);
    CHECK(fds->events == POLLIN || fds->events == POLLOUT, "poll for events 0x%x", fds->events);
    if (timeout == 0) return 0; // net_accept() asking whether a reader is waiting: none is
    if (g_poll_fails) {
        errno = EIO;
        return -1;
    }
    g_polls++;
    g_last_events = fds->events;
    if (g_not_ready > 0) {
        g_not_ready--;
        g_timeouts++;
        return 0;
    }
    fds->revents = fds->events;
    return 1;
}

ssize_t __wrap_send(int fd, const void* buf, size_t len, int flags) {
    (void)fd;
    CHECK((flags & ~MSG_NOSIGNAL) == 0, "send with flags 0x%x", flags);
    g_send_calls++;
    if (g_send_interrupted > 0) {
        g_send_interrupted--;
        errno = EINTR;
        return -1;
    }
    if (g_send_not_yet > 0) {
        g_send_not_yet--;
        errno = EAGAIN;
        g_not_ready = g_wait_polls;
        return -1;
    }
    if (g_send_enobufs) {
        g_send_enobufs = false;
        errno = ENOBUFS;
        g_not_ready = g_wait_polls;
        return -1;
    }
    if (g_send_broken) {
        errno = EPIPE;
        return -1;
    }
    if (g_send_nothing) return 0;
    size_t n = len;
    if (g_send_max != 0 && n > g_send_max) n = g_send_max;
    if (g_sent_len + n > sizeof g_sent) {
        CHECK(0, "the fake socket was sent too much");
        errno = ENOBUFS;
        return -1;
    }
    memcpy(g_sent + g_sent_len, buf, n);
    g_sent_len += n;
    return (ssize_t)n;
}

static bool input_left(void) { return g_in != NULL && g_in_pos < strlen(g_in); }

ssize_t __wrap_recv(int fd, void* buf, size_t len, int flags) {
    (void)fd;
    (void)flags;
    CHECK(len == 1, "recv asked for %zu bytes: a request line is read one byte at a time", len);
    const bool wait = g_recv_not_yet > 0 ||
                      (g_in_plan != NULL && g_in_plan_pos < strlen(g_in_plan) &&
                       g_in_plan[g_in_plan_pos] == 'w');
    if (g_recv_not_yet > 0) g_recv_not_yet--;
    else if (g_in_plan != NULL && g_in_plan_pos < strlen(g_in_plan)) g_in_plan_pos++;
    if (wait) {
        errno = EAGAIN;
        g_not_ready = g_wait_polls;
        return -1;
    }
    if (!input_left()) {
        if (g_in_broken) {
            g_in_broken = false;
            errno = ECONNRESET;
            return -1;
        }
        return 0; // the reader closed the connection
    }
    *(char*)buf = g_in[g_in_pos++];
    return 1;
}

// ---- the idle hook ----------------------------------------------------------------------------

static int g_marker;
static int g_idle_calls;
static u64 g_idle_seen[64]; // the idle time of each call
static int g_give_up_at;    // the call at which the hook gives up (0: never)

static bool test_idle(void* user, u64 idle_ns) {
    CHECK(user == &g_marker, "the idle hook was called with another user than its own");
    if (g_idle_calls < (int)(sizeof g_idle_seen / sizeof g_idle_seen[0]))
        g_idle_seen[g_idle_calls] = idle_ns;
    g_idle_calls++;
    shim_advance_ns(SECOND); // time passes while the socket makes us wait
    return g_give_up_at == 0 || g_idle_calls < g_give_up_at;
}

// ---- helpers ----------------------------------------------------------------------------------

static void socket_as_new(void) {
    g_flag_sets = 0;
    g_no_nonblock = false;
    g_poll_fails = false;
    g_wait_polls = 0;
    g_not_ready = 0;
    g_timeouts = 0;
    g_polls = 0;
    g_last_events = 0;
    g_send_not_yet = 0;
    g_send_enobufs = false;
    g_send_interrupted = 0;
    g_send_broken = false;
    g_send_nothing = false;
    g_send_max = 0;
    g_sent_len = 0;
    g_send_calls = 0;
    g_in = NULL;
    g_in_pos = 0;
    g_recv_not_yet = 0;
    g_in_plan = NULL;
    g_in_plan_pos = 0;
    g_in_broken = false;
    g_idle_calls = 0;
    memset(g_idle_seen, 0, sizeof g_idle_seen);
    g_give_up_at = 0;
}

// A connection that a reader made at time `at`, with the scripted idle hook. One at a time.
static NetConn* connected(u64 at) {
    static NetConn c;
    socket_as_new();
    shim_set_virtual_time(1);
    shim_advance_ns(at - shim_now_ns());
    net_conn_init(&c, 7);
    net_conn_set_idle(&c, test_idle, &g_marker);
    return &c;
}

static Result send_bytes(NetConn* c, const void* data, size_t len) {
    return c->link.send_all(c->link.user, data, len);
}

static Result send_text(NetConn* c, const char* s) { return send_bytes(c, s, strlen(s)); }

static int recv_line(NetConn* c, char* line, size_t cap) {
    return c->link.recv_line(c->link.user, line, cap);
}

// The idle times the hook was told of, as text for a failure message.
static const char* idle_times(void) {
    static char text[64 * 12];
    size_t pos = 0;
    text[0] = '\0';
    for (int i = 0; i < g_idle_calls && i < 64; i++)
        pos += (size_t)snprintf(text + pos, sizeof text - pos, "%s%llu", i ? " " : "",
                                NS(g_idle_seen[i] / SECOND));
    return text;
}

// ---- the tests --------------------------------------------------------------------------------

static void test_connect(void) {
    NetConn* c = connected(5 * SECOND);
    CHECK(c->fd == 7 && c->nonblocking && g_flag_sets == 1, "the socket is not non-blocking (%d sets)",
          g_flag_sets);
    CHECK(c->link.user == c && c->link.send_all != NULL && c->link.recv_line != NULL,
          "the link is not set up");
    CHECK(c->progress_ns == 5 * SECOND, "progress starts at %llu s, not at the connection",
          NS(c->progress_ns / SECOND));

    // A socket that cannot be made non-blocking says so, and its link still works.
    socket_as_new();
    g_no_nonblock = true;
    net_conn_init(c, 7);
    CHECK(!c->nonblocking && g_flag_sets == 0,
          "a socket that cannot be made non-blocking is said to be one");
    g_in = "PING\n";
    char line[32];
    CHECK(recv_line(c, line, sizeof line) == 1 && strcmp(line, "PING") == 0,
          "without non-blocking mode a line does not come through");
}

static void test_send(void) {
    NetConn* c = connected(0);
    CHECK(send_text(c, "PONG\n") == 0 && g_send_calls == 1 && g_sent_len == 5 &&
              memcmp(g_sent, "PONG\n", 5) == 0,
          "a short answer is not sent as it is");
    CHECK(send_bytes(c, "", 0) == 0 && g_send_calls == 1, "sending nothing called send()");

    // The reader takes nothing for a while: the idle hook hears how long, the wait is for
    // the socket to take, and the clock of it starts again once a byte went through.
    c = connected(10 * SECOND);
    g_send_not_yet = 1;
    g_wait_polls = 3;
    CHECK(send_text(c, "abc") == 0, "sending after a wait failed");
    CHECK(g_timeouts == 3 && g_last_events == POLLOUT, "%d polls passed without the socket (wanted 3),"
          " the last for events 0x%x", g_timeouts, g_last_events);
    CHECK(g_idle_calls == 3, "the idle hook was called %d times for three waits", g_idle_calls);
    CHECK(g_idle_seen[0] == 0 && g_idle_seen[1] == SECOND && g_idle_seen[2] == 2 * SECOND,
          "idle times in s: %s, wanted 0 1 2", idle_times());
    CHECK(c->progress_ns == 13 * SECOND, "progress is at %llu s, wanted 13: the send's",
          NS(c->progress_ns / SECOND));
    g_send_not_yet = 1;
    g_wait_polls = 2;
    CHECK(send_text(c, "de") == 0 && g_sent_len == 5 && memcmp(g_sent, "abcde", 5) == 0,
          "the second answer did not follow the first");
    CHECK(g_idle_calls == 5 && g_idle_seen[3] == 0 && g_idle_seen[4] == SECOND,
          "after a byte went through the idle time starts again: in s %s, wanted ... 0 1",
          idle_times());

    // More than one send() takes: every call asks for at most 128 KiB of what is left, of
    // which the socket takes 100000, so 4 calls: all of it, in order.
    c = connected(0);
    static char big[3 * 128 * 1024 + 100];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + 3);
    g_send_max = 100000;
    CHECK(send_bytes(c, big, sizeof big) == 0 && g_send_calls == 4 && g_sent_len == sizeof big &&
              memcmp(g_sent, big, sizeof big) == 0,
          "a large block: %d send() calls for %zu bytes, %zu arrived", g_send_calls, sizeof big,
          g_sent_len);

    // The idle hook gives up: nothing more is tried, and the call fails.
    c = connected(0);
    g_give_up_at = 4;
    g_send_not_yet = 1;
    g_wait_polls = 100;
    CHECK(send_text(c, "abc") == NBE_RC_NET, "giving up did not fail the send");
    CHECK(g_timeouts == 4 && g_polls == 4 && g_idle_calls == 4 && g_send_calls == 1 &&
              g_sent_len == 0,
          "after giving up: %d polls, %d idle calls, %d send() calls", g_polls, g_idle_calls,
          g_send_calls);

    // Without an idle hook the link waits as long as it takes.
    c = connected(0);
    net_conn_set_idle(c, NULL, NULL);
    g_send_not_yet = 2;
    g_wait_polls = 5;
    CHECK(send_text(c, "ab") == 0 && g_timeouts == 10 && g_idle_calls == 0,
          "without an idle hook: %d polls passed for two waits of five, %d idle calls",
          g_timeouts, g_idle_calls);

    // EINTR is tried again; ENOBUFS is BSD's "nothing moved just now".
    c = connected(0);
    g_send_interrupted = 2;
    CHECK(send_text(c, "ab") == 0 && g_send_calls == 3 && g_sent_len == 2,
          "after EINTR: %d send() calls, %zu bytes", g_send_calls, g_sent_len);
    c = connected(0);
    g_send_enobufs = true;
    g_wait_polls = 1;
    CHECK(send_text(c, "ab") == 0 && g_send_calls == 2 && g_timeouts == 1 && g_idle_calls == 1,
          "after ENOBUFS: %d send() calls, %d polls without the socket", g_send_calls,
          g_timeouts);

    // A poll() that fails is an error too, and the idle hook is not asked.
    c = connected(0);
    g_poll_fails = true;
    g_send_not_yet = 1;
    CHECK(send_text(c, "ab") == NBE_RC_NET && g_idle_calls == 0,
          "a poll() that fails did not fail the send (idle calls: %d)", g_idle_calls);

    // A broken connection is an error, and so is a send() that takes nothing.
    c = connected(0);
    g_send_broken = true;
    CHECK(send_text(c, "ab") == NBE_RC_NET && g_polls == 0, "a broken connection did not fail the send");
    c = connected(0);
    g_send_nothing = true;
    CHECK(send_text(c, "ab") == NBE_RC_NET, "a send() that takes nothing did not fail");
}

static void test_recv(void) {
    NetConn* c = connected(0);
    char line[32];
    g_in = "READ 30 0 0\r\n";
    CHECK(recv_line(c, line, sizeof line) == 1 && strcmp(line, "READ 30 0 0") == 0,
          "a line with a CR at its end reads %s", line);

    // The reader closes: before any line that is a close, in the middle of one it is not.
    c = connected(0);
    g_in = "";
    CHECK(recv_line(c, line, sizeof line) == 0, "a close before any line is not 0");
    c = connected(0);
    g_in = "REA";
    CHECK(recv_line(c, line, sizeof line) == -1, "a close in the middle of a line is not -1");

    // A line that just fits the buffer does; one byte more is not a line.
    char small[4];
    c = connected(0);
    g_in = "abc\n";
    CHECK(recv_line(c, small, sizeof small) == 1 && strcmp(small, "abc") == 0,
          "a line of 3 bytes does not fit a buffer of 4");
    c = connected(0);
    g_in = "abcd\n";
    CHECK(recv_line(c, small, sizeof small) == -1, "a line of 4 bytes passed as one for a buffer of 4");

    // No request yet: the wait is for something to read.
    c = connected(0);
    g_in = "LIST\n";
    g_recv_not_yet = 2;
    g_wait_polls = 2;
    CHECK(recv_line(c, line, sizeof line) == 1 && strcmp(line, "LIST") == 0,
          "after a wait the line reads %s", line);
    CHECK(g_timeouts == 4 && g_last_events == POLLIN && g_idle_calls == 4,
          "waiting for a request: %d polls passed without the socket (wanted 4, last for 0x%x),"
          " %d idle calls", g_timeouts, g_last_events, g_idle_calls);

    // The idle hook gives up while a request is awaited.
    c = connected(0);
    g_in = "LIST\n";
    g_recv_not_yet = 1;
    g_wait_polls = 100;
    g_give_up_at = 3;
    CHECK(recv_line(c, line, sizeof line) == -1 && g_timeouts == 3 && g_polls == 3,
          "giving up: %d polls", g_polls);

    // The connection breaks.
    c = connected(0);
    g_in = "";
    g_in_broken = true;
    CHECK(recv_line(c, line, sizeof line) == -1, "a reset connection is not -1");

    // A byte of the reader restarts the idle clock: "PI", a wait, then the rest.
    c = connected(0);
    g_in = "PING\n";
    g_in_plan = "w..w.";
    g_wait_polls = 2;
    CHECK(recv_line(c, line, sizeof line) == 1 && strcmp(line, "PING") == 0,
          "the line reads %s", line);
    CHECK(g_idle_calls == 4 && g_idle_seen[0] == 0 && g_idle_seen[1] == SECOND &&
              g_idle_seen[2] == 0 && g_idle_seen[3] == SECOND,
          "idle times in s: %s, wanted 0 1 0 1", idle_times());
}

static void test_listen(void) {
    Result rc = 1;
    const int fd = net_listen_port(&rc, 0); // any port: nothing connects to it
    CHECK(fd >= 0 && rc == 0, "listening failed: %08x", (unsigned)rc);
    CHECK(net_accept(fd) == -1, "a reader was accepted where none connected");
    net_close(fd);
    net_close(-1);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0); // a line at a time, also when the output goes to a file
    shim_init("/nonexistent"); // for the clock: nothing here touches files
    struct { const char* name; void (*fn)(void); } tests[] = {
        { "connect", test_connect },
        { "send", test_send },
        { "recv", test_recv },
        { "listen", test_listen },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const int before_checks = g_checks, before_failures = g_failures;
        tests[i].fn();
        printf("%-18s %s (%d checks)\n", tests[i].name,
               g_failures == before_failures ? "ok" : "FAILED", g_checks - before_checks);
    }
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
