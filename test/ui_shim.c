// Host stand-in for the console, pad, applet and clock calls in source/main.c. See ui_shim.h.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ui_shim.h"

enum { UI_COLS = 80, UI_ROWS = 45, UI_MAX_FRAMES = 1024, UI_MAX_STEPS = 4096 };

// ---- screen ----------------------------------------------------------------------------------

static PrintConsole g_console = { UI_COLS, UI_ROWS };
static char g_screen[UI_ROWS][UI_COLS + 1];
static int g_crow, g_ccol;
static FILE* g_real_stdout;
static FILE* g_stream; // stdout of the app: everything it printf()s
static char* g_out;
static size_t g_out_size, g_out_parsed;
static char g_frames[UI_MAX_FRAMES][UI_ROWS * (UI_COLS + 1) + 1];
static int g_frame_n;

static void screen_clear(void) {
    for (int r = 0; r < UI_ROWS; r++) {
        memset(g_screen[r], ' ', UI_COLS);
        g_screen[r][UI_COLS] = '\0';
    }
}

// Lay out what the app has printed since the last call: ESC[row;colH moves the cursor, other
// escape sequences (colours) are skipped, printable characters land on the screen.
static void layout(void) {
    fflush(g_stream);
    while (g_out_parsed < g_out_size) {
        const size_t start = g_out_parsed;
        const char ch = g_out[start];
        if (ch == '\x1b') {
            if (start + 1 >= g_out_size) break; // incomplete: wait for the rest
            if (g_out[start + 1] != '[') {
                g_out_parsed += 2;
                continue;
            }
            size_t j = start + 2;
            int value = 0;
            bool digits = false;
            while (j < g_out_size && g_out[j] >= '0' && g_out[j] <= '9') {
                value = value * 10 + (g_out[j] - '0');
                digits = true;
                j++;
            }
            while (j < g_out_size && !(g_out[j] >= 0x40 && g_out[j] <= 0x7E)) j++;
            if (j >= g_out_size) break; // incomplete escape sequence
            if (g_out[j] == 'H' && digits) {
                g_crow = value - 1;
                g_ccol = 0;
            }
            g_out_parsed = j + 1;
        } else if (ch == '\n') {
            g_crow++;
            g_ccol = 0;
            g_out_parsed++;
        } else if (ch == '\r') {
            g_ccol = 0;
            g_out_parsed++;
        } else {
            if ((unsigned char)ch >= 0x20) {
                if (g_crow >= 0 && g_crow < UI_ROWS && g_ccol >= 0 && g_ccol < UI_COLS)
                    g_screen[g_crow][g_ccol] = ch;
                g_ccol++;
            }
            g_out_parsed++;
        }
    }
}

static const char* screen_text(char* dst) {
    char* p = dst;
    for (int r = 0; r < UI_ROWS; r++) {
        int n = UI_COLS;
        while (n > 0 && g_screen[r][n - 1] == ' ') n--;
        memcpy(p, g_screen[r], (size_t)n);
        p += n;
        *p++ = '\n';
    }
    *p = '\0';
    return dst;
}

PrintConsole* consoleInit(PrintConsole* console) {
    (void)console;
    screen_clear();
    g_crow = g_ccol = 0;
    g_real_stdout = stdout;
    g_stream = open_memstream(&g_out, &g_out_size);
    if (!g_stream) abort();
    g_out_parsed = 0;
    stdout = g_stream; // from here on the app's printf() lands on the screen
    return &g_console;
}

void consoleExit(PrintConsole* console) {
    (void)console;
    if (!g_stream) return;
    layout();
    fclose(g_stream);
    free(g_out);
    g_stream = NULL;
    g_out = NULL;
    stdout = g_real_stdout;
}

// libnx's console keeps what was drawn until it is cleared, and shows it at consoleUpdate().
void consoleClear(void) { layout(); screen_clear(); }
void consoleUpdate(PrintConsole* console) {
    (void)console;
    layout();
    char text[sizeof g_frames[0]];
    screen_text(text);
    if (g_frame_n > 0 && strcmp(text, g_frames[g_frame_n - 1]) == 0) return;
    if (g_frame_n < UI_MAX_FRAMES) snprintf(g_frames[g_frame_n++], sizeof g_frames[0], "%s", text);
}

// ---- pad and applet --------------------------------------------------------------------------

typedef struct { u64 down, held; } UiStep;
static UiStep g_pad_state;
static UiStep g_steps[UI_MAX_STEPS];
static int g_step_n, g_step_i, g_polls_after_end;

void padConfigureInput(u32 max_players, u32 style_set) {
    (void)max_players;
    (void)style_set;
}

void padInitializeDefault(PadState* pad) {
    (void)pad;
    memset(&g_pad_state, 0, sizeof g_pad_state);
}

void padUpdate(PadState* pad) {
    (void)pad;
    if (g_step_i < g_step_n) {
        g_pad_state = g_steps[g_step_i++];
    } else {
        memset(&g_pad_state, 0, sizeof g_pad_state); // nothing pressed any more
    }
}

u64 padGetButtons(const PadState* pad) { (void)pad; return g_pad_state.held; }
u64 padGetButtonsDown(const PadState* pad) { (void)pad; return g_pad_state.down; }

bool appletMainLoop(void) {
    if (g_step_i < g_step_n) {
        g_polls_after_end = 0;
        return true;
    }
    // The script is used up. If the app is still going, let it run a little longer (it may be
    // finishing something), then end it the way the system would.
    return ++g_polls_after_end <= 400;
}

static bool g_sleep_refused, g_sleep_asked;

Result appletSetAutoSleepDisabled(bool flag) {
    if (flag) g_sleep_asked = true;
    return g_sleep_refused ? MAKERESULT(345, 78) : 0;
}

void ui_sleep_refused(void) { g_sleep_refused = true; }
bool ui_sleep_disabled_asked(void) { return g_sleep_asked; }

// Time only passes while the app sleeps, so a scripted run does not depend on the host's speed.
void svcSleepThread(s64 ns) { shim_advance_ns(ns > 0 ? (u64)ns : 1); }

u32 hosversionGet(void) { return MAKEHOSVERSION(23, 0, 1); }

static FsFileSystem g_sd_fs;
static char g_sd_root[1200]; // g_sd_fs.root points into this
static bool g_sd_available;

FsFileSystem* fsdevGetDeviceFileSystem(const char* name) {
    if (!g_sd_available || strcmp(name, "sdmc") != 0) return NULL;
    return &g_sd_fs;
}

void ui_sd_card(int available, const char* root) {
    g_sd_available = available != 0;
    snprintf(g_sd_root, sizeof g_sd_root, "%s", root);
    g_sd_fs.root = g_sd_root;
}

Result fsFsCreateDirectory(FsFileSystem* fs, const char* path) {
    char p[2048];
    snprintf(p, sizeof p, "%s%s", fs->root, path);
    return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : 0x202;
}

// ---- the script ------------------------------------------------------------------------------

void ui_reset_script(void) {
    g_step_n = g_step_i = 0;
    g_polls_after_end = 0;
    memset(&g_pad_state, 0, sizeof g_pad_state);
}

static void step(u64 down, u64 held) {
    if (g_step_n >= UI_MAX_STEPS) return;
    g_steps[g_step_n].down = down;
    g_steps[g_step_n].held = held;
    g_step_n++;
}

void ui_press(u64 button) { step(button, button); }
void ui_repeat(u64 button, int n) {
    for (int i = 0; i < n; i++) ui_press(button);
}
void ui_idle(int polls) {
    for (int i = 0; i < polls; i++) step(0, 0);
}
void ui_hold(u64 button, int polls) {
    for (int i = 0; i < polls; i++) step(0, button);
}
int ui_script_left(void) { return g_step_n - g_step_i; }
int ui_polls_after_script(void) { return g_polls_after_end; }

// ---- the screen, for the test ----------------------------------------------------------------

int ui_frame_count(void) { return g_frame_n; }

const char* ui_frame(int index) {
    return index >= 0 && index < g_frame_n ? g_frames[index] : "";
}

int ui_frame_with(const char* text, int from) {
    for (int i = from > 0 ? from : 0; i < g_frame_n; i++)
        if (strstr(g_frames[i], text)) return i;
    return -1;
}
