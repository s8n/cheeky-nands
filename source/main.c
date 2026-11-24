// nand-backup-experiments: list the console's BIS (NAND) partitions as the FS sysmodule
// exposes them through OpenBisStorage, and dump the selected one (or all of them) to the SD
// card. Text UI.
//
// NAND is only ever read here; see bis.c. The only writes go to the SD card, in the one
// directory named below.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <switch.h>
#include "bis.h"
#include "nbe_fmt.h"
#include "net.h"

#define OUT_DIR "/nand-backup-experiments" // as the SD card's FsFileSystem sees it
#ifndef NBE_SD_MOUNT
#define NBE_SD_MOUNT "sdmc:" // the same directory as stdio sees it (the host test overrides it)
#endif
#define LOG_PATH NBE_SD_MOUNT OUT_DIR "/log.txt"

#define CYAN  "\x1b[36;1m"
#define GREEN "\x1b[32;1m"
#define RED   "\x1b[31;1m"
#define WHITE "\x1b[37;1m"
#define RESET "\x1b[0m"

#define LOG_LINES 60
#define LOG_WIDTH 200 // a dump message is up to 191 characters
#define NBE_LINE_MAX  160
#define HEADER_ROWS 3
#define LOG_MIN_ROWS 8
#define LIST_MAX_ROWS 24

// Key repeat, in the list and in the partial-dump dialog: a held key acts again after
// REPEAT_DELAY_NS, then every REPEAT_INTERVAL_NS.
#define REPEAT_DELAY_NS    400000000ULL
#define REPEAT_INTERVAL_NS 110000000ULL

static PrintConsole* g_con;
static int g_cw = 80, g_ch = 45;
static PadState g_pad;
static FsFileSystem* g_sd;
static BisTable g_table;
static BisDumpResult g_dump; // several KiB: keep it off the stack
static int g_sel;
static int g_top;
static bool g_split = true;
static bool g_quit;
static s64 g_free;
static bool g_have_free;
static u64 g_last_draw;
static char g_log[LOG_LINES][LOG_WIDTH];
static int g_log_n;
static FILE* g_logf;
static int g_listen = -1;  // the network reader's listening socket, while it is on
static bool g_net_ready;   // the socket driver has been started
// How long a reader may let pass without a byte going through its connection before it is let
// go of: longer than net_pull.py's minute of waiting for the console, so that a reader that is
// only slow still gets its answer; shorter than the minutes a lost connection would otherwise
// hold the app for.
#define NET_STALL_NS 120000000000ULL
#define SECOND_NS 1000000000ULL
static const char* g_net_let_go; // why the last connection was let go of, or NULL
static bool g_net_said_blocking; // the warning about sockets that cannot be made non-blocking

// ---------------------------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------------------------

// Write one plain-text line at a 0-based row. `text` carries no escape sequences, so it can be
// cut to the console width without breaking one; the colour goes around it. One column is
// left free so a full line never makes the console wrap.
static void put(int row, const char* color, const char* text) {
    char line[NBE_LINE_MAX];
    size_t n = strlen(text);
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, text, n);
    line[n] = '\0';
    if ((int)n > g_cw - 1) line[g_cw - 1] = '\0';
    printf("\x1b[%d;1H%s%s" RESET, row + 1, color, line);
}

// The file name part of a path, copied into a bounded buffer.
static void file_name_of(char* dst, size_t cap, const char* path) {
    const char* slash = strrchr(path, '/');
    snprintf(dst, cap, "%.*s", (int)cap - 1, slash ? slash + 1 : path);
}

static void ui_log(const char* fmt, ...) {
    char text[LOG_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    if (g_log_n >= LOG_LINES) {
        memmove(g_log, g_log + 1, sizeof g_log - sizeof g_log[0]);
        g_log_n = LOG_LINES - 1;
    }
    snprintf(g_log[g_log_n++], LOG_WIDTH, "%s", text);

    if (g_logf) {
        const time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        fprintf(g_logf, "[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, text);
        fflush(g_logf); // survive a freeze or a forced exit
    }
}

// Rows available for the partition list; shrinks on a small screen so the log keeps some.
static int list_height(void) {
    int h = g_ch - HEADER_ROWS - 1 - 1 - 1 - LOG_MIN_ROWS - 2; // two footer rows
    if (h < 6) h = 6;
    if (h > LIST_MAX_ROWS) h = LIST_MAX_ROWS;
    return h;
}

static int menu_rows(void) { return 1 + g_table.count; }

static void keep_selection_visible(void) {
    const int rows = menu_rows();
    const int max_h = list_height();
    const int h = rows < max_h ? rows : max_h;
    if (h <= 0) return;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + h) g_top = g_sel - h + 1;
    if (g_top > rows - h) g_top = rows - h;
    if (g_top < 0) g_top = 0;
}

// ---------------------------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------------------------

static void draw_menu(void) {
    char line[NBE_LINE_MAX], sz[24];
    const int rows = menu_rows();
    const int max_h = list_height();
    const int list_h = rows < max_h ? rows : max_h;
    keep_selection_visible();

    consoleClear();

    const u32 fw = hosversionGet();
    if (fw)
        snprintf(line, sizeof line, "%s v%s  [%s, fw %u.%u.%u]", NBE_APP_NAME, NBE_APP_VERSION,
                 nbe_platform(), HOSVER_MAJOR(fw), HOSVER_MINOR(fw), HOSVER_MICRO(fw));
    else
        snprintf(line, sizeof line, "%s v%s  [%s]", NBE_APP_NAME, NBE_APP_VERSION, nbe_platform());
    put(0, CYAN, line);

    char net[32];
    if (g_listen >= 0)
        snprintf(net, sizeof net, "ON, port %d", NBE_NET_PORT);
    else
        snprintf(net, sizeof net, "OFF");
    if (g_have_free) {
        nbe_fmt_size(sz, sizeof sz, g_free);
        snprintf(line, sizeof line, "SD free %s  -> sdmc:%s/  net: %s", sz, OUT_DIR, net);
    } else {
        snprintf(line, sizeof line, "-> sdmc:%s/  net: %s", OUT_DIR, net);
    }
    put(1, WHITE, line);

    // Row 2 says what [A] would do with the current selection.
    const char* hint_color = WHITE;
    if (g_sel == 0) {
        nbe_fmt_size(sz, sizeof sz, g_table.openable_total);
        snprintf(line, sizeof line, "[A] dump ALL: %d openable partition(s), %s%s",
                 g_table.openable_count, sz, g_split ? ", over 4 GiB split" : ", never split");
    } else {
        const BisPartition* p = &g_table.parts[g_sel - 1];
        BisPlan plan;
        char file[64];
        bis_plan(&plan, OUT_DIR, p->name, p->size, g_split);
        nbe_fmt_size(sz, sizeof sz, p->size);
        if (p->openable) {
            file_name_of(file, sizeof file, plan.base);
            if (plan.multi)
                snprintf(line, sizeof line, "[A] dump %s: %s -> %s.00 .. .%02d (%d files)",
                         p->name, sz, file, plan.parts - 1, plan.parts);
            else
                snprintf(line, sizeof line, "[A] dump %s: %s -> %s (1 file)", p->name, sz, file);
        } else {
            char rs[64];
            nbe_fmt_result(rs, sizeof rs, p->open_rc);
            snprintf(line, sizeof line, "[A] dump %s: cannot open (%s)", p->name, rs);
            hint_color = RED;
        }
    }
    put(2, hint_color, line);

    // Partition list, row 0 being ALL.
    for (int v = 0; v < list_h; v++) {
        const int row = g_top + v;
        const char* color = WHITE;
        bool failing = false;
        const bool selected = row == g_sel;
        if (row == 0) {
            nbe_fmt_size(sz, sizeof sz, g_table.openable_total);
            snprintf(line, sizeof line, "  ALL    every openable partition  %10s", sz);
        } else {
            const BisPartition* p = &g_table.parts[row - 1];
            if (p->openable) {
                nbe_fmt_size(sz, sizeof sz, p->size);
                snprintf(line, sizeof line, "%3u] %-31.31s %10s  %s", p->id, p->name, sz,
                         p->alias ? p->alias : "");
            } else {
                char rs[64]; // "0x00320002 PermissionDenied" does not fit a size buffer
                nbe_fmt_result(rs, sizeof rs, p->open_rc);
                snprintf(line, sizeof line, "%3u] %-31.31s %10s  %s", p->id, p->name, "-", rs);
                failing = true;
            }
        }
        char marked[NBE_LINE_MAX + 1]; // the line, with room for the selection marker
        snprintf(marked, sizeof marked, "%s%s", selected ? ">" : " ", line);
        // selected: bright (cyan, or red for a row that cannot be opened); else plain.
        put(HEADER_ROWS + v, selected ? (failing ? RED : CYAN) : (failing ? "\x1b[31m" : color),
            marked);
    }

    // Log.
    const int log_hdr = HEADER_ROWS + list_h + 1;
    int n = snprintf(line, sizeof line, "--- log, also appended to %s ---", LOG_PATH);
    put(log_hdr, CYAN, line);
    (void)n;
    const int log_top = log_hdr + 1;
    const int log_h = g_ch - 2 - log_top;
    int first = g_log_n - log_h;
    if (first < 0) first = 0;
    for (int i = first; i < g_log_n; i++)
        put(log_top + (i - first), WHITE, g_log[i]);

    snprintf(line, sizeof line,
             "[A] dump  [X] split >4GiB: %s  [Y] rescan  [L] GPT  [ZL] partial dump",
             g_split ? "ON " : "OFF");
    put(g_ch - 2, CYAN, line);
    put(g_ch - 1, CYAN, "[R] 4 GiB test  [ZR] network reader  [+] exit");
    consoleUpdate(NULL);
}

// Yes/no dialog. Returns 1 for yes, 0 for no, -1 when the app should exit. With yes == NULL
// the question cannot be answered with yes (used to explain why an action is refused).
static int ui_confirm(const char* const* lines, int n, const char* yes, const char* no) {
    char line[NBE_LINE_MAX];
    const u64 t0 = bis_now_ns();

    for (;;) {
        if (!appletMainLoop()) return -1;
        consoleClear();
        for (int i = 0; i < n; i++) put(3 + i, i == 0 ? WHITE : WHITE, lines[i]);
        if (yes)
            snprintf(line, sizeof line, "[A] %s      [B] %s", yes, no);
        else
            snprintf(line, sizeof line, "[B] %s", no);
        put(3 + n + 1, yes ? CYAN : RED, line);
        consoleUpdate(NULL);

        padUpdate(&g_pad);
        const u64 k = padGetButtonsDown(&g_pad);
        if (bis_now_ns() - t0 > 300000000ULL) { // ignore the press that opened the dialog
            if (yes && (k & HidNpadButton_A)) return 1;
            if (k & HidNpadButton_B) return 0;
            if (k & HidNpadButton_Plus) return -1;
        }
        svcSleepThread(16 * 1000 * 1000LL);
    }
}

// ---------------------------------------------------------------------------------------------
// Dumping
// ---------------------------------------------------------------------------------------------

static void draw_progress(const BisProgress* pg) {
    char line[NBE_LINE_MAX], a[24], b[24], c[24], eta[16], nm[96];
    const char* slash = strrchr(pg->path, '/');
    snprintf(nm, sizeof nm, "%s", slash ? slash + 1 : pg->path);

    consoleClear();
    snprintf(line, sizeof line, "%s v%s", NBE_APP_NAME, NBE_APP_VERSION);
    put(0, CYAN, line);
    if (pg->network) {
        snprintf(line, sizeof line, "sending over the network: %s (id %u)", pg->part->name,
                 pg->part->id);
        put(3, WHITE, line);
        snprintf(line, sizeof line, "request: %s", pg->note ? pg->note : "");
        put(4, WHITE, line);
    } else {
        snprintf(line, sizeof line, "dumping %s (id %u)   file %d of %d", pg->part->name,
                 pg->part->id, pg->part_index + 1, pg->parts);
        put(3, WHITE, line);
        snprintf(line, sizeof line, "-> %s", nm);
        put(4, WHITE, line);
    }

    nbe_fmt_size(a, sizeof a, pg->done);
    nbe_fmt_size(b, sizeof b, pg->total);
    const u64 pct_x10 = pg->total ? (u64)pg->done * 1000 / (u64)pg->total : 1000;
    const u64 ms = pg->elapsed_ns / 1000000ULL;
    const u64 bps = ms ? (u64)pg->done * 1000 / ms : 0; // bytes per second so far
    nbe_fmt_size(c, sizeof c, (s64)bps);
    if (bps) nbe_fmt_hms(eta, sizeof eta, ((u64)pg->total - (u64)pg->done) / bps);
    else snprintf(eta, sizeof eta, "--:--:--");
    snprintf(line, sizeof line, "%s / %s   %llu.%llu%%   %s/s   ETA %s", a, b,
             (unsigned long long)(pct_x10 / 10), (unsigned long long)(pct_x10 % 10), c, eta);
    put(6, WHITE, line);

    int width = g_cw - 4;
    if (width > NBE_LINE_MAX - 3) width = NBE_LINE_MAX - 3;
    const int fill = (int)(pct_x10 * (u64)width / 1000);
    int pos = 0;
    line[pos++] = '[';
    for (int i = 0; i < width; i++) line[pos++] = i < fill ? '#' : '-';
    line[pos++] = ']';
    line[pos] = '\0';
    put(7, GREEN, line);

    put(9, CYAN, "[B] cancel this dump");
    consoleUpdate(NULL);
}

// B or + while the app is busy: asks whether to stop. 0: go on. 1: stop. -1: stop and exit.
static int ask_stop(const char* const* lines, const char* stop_label) {
    const int a = ui_confirm(lines, 2, stop_label, "continue");
    if (a < 0) g_quit = true; // '+' means exit, also from the middle of what the app is doing
    return a;
}

static bool cb_progress(void* user, const BisProgress* pg) {
    (void)user;
    if (!appletMainLoop()) return false; // system wants us gone: unwind, removing partial files

    padUpdate(&g_pad);
    const u64 k = padGetButtonsDown(&g_pad);
    if (k & (HidNpadButton_B | HidNpadButton_Plus)) {
        static const char* const lines[] = {
            "Cancel this dump?",
            "The files written so far are deleted.",
        };
        static const char* const stop_lines[] = {
            "Stop sending?",
            "The reader keeps what it got, and can ask for the rest.",
        };
        if (ask_stop(pg->network ? stop_lines : lines, pg->network ? "stop" : "cancel") != 0)
            return false;
    }

    const u64 now = bis_now_ns();
    if (now - g_last_draw >= 100000000ULL) { // redraw at most 10 times a second
        g_last_draw = now;
        draw_progress(pg);
    }
    return true;
}

// Shown while a part file is being pre-allocated, which can take a few seconds on a slow card.
static void cb_message(void* user, const char* text) {
    (void)user;
    char line[NBE_LINE_MAX];
    consoleClear();
    snprintf(line, sizeof line, "%s v%s", NBE_APP_NAME, NBE_APP_VERSION);
    put(0, CYAN, line);
    put(5, WHITE, text);
    consoleUpdate(NULL);
}

static const BisCallbacks g_cbs = { NULL, cb_progress, cb_message };

static void refresh_free(void) {
    char root[FS_MAX_PATH] = "/";
    g_have_free = R_SUCCEEDED(fsFsGetFreeSpace(g_sd, root, &g_free)) && g_free >= 0;
}

static void log_result(const BisPartition* p, const BisDumpResult* r) {
    char sz[24], spd[24], hms[16], hex[65];
    // The files of a partial dump carry the range in their name: name those.
    const char* const who = r->stem[0] ? r->stem : p->name;
    if (r->status == BisDump_Ok) {
        const u64 ms = r->elapsed_ns / 1000000ULL;
        nbe_fmt_size(sz, sizeof sz, r->size);
        if (ms) {
            nbe_fmt_size(spd, sizeof spd, (s64)((u64)r->size * 1000 / ms));
            nbe_fmt_hms(hms, sizeof hms, ms / 1000);
            ui_log("%s: ok, %s -> %d file(s) in %s (%s/s)", who, sz, r->parts, hms, spd);
        } else {
            ui_log("%s: ok, %s -> %d file(s)", who, sz, r->parts);
        }
        nbe_fmt_hex32(r->sha256, hex);
        ui_log("  sha256 %s", hex);
        if (r->plaintext == 0)
            ui_log("  note: no plaintext signature in the first sector (see README)");
        if (r->used_bounce)
            ui_log("  note: FS could not map the chunk buffer; slow bounce path used");
    } else {
        char rs[64];
        nbe_fmt_result(rs, sizeof rs, r->rc);
        if (r->status == BisDump_Cancelled) {
            ui_log("%s: %s", who, r->message);
        } else {
            // The result code comes first, so it is never the part cut off a long line.
            ui_log("%s: FAILED at %s [%s]", who, r->stage, rs);
            ui_log("  %s", r->message);
            if (R_MODULE(r->rc) == 2 && R_DESCRIPTION(r->rc) >= 6400 &&
                R_DESCRIPTION(r->rc) <= 6449)
                ui_log("  PermissionDenied: this process may not open that partition");
        }
    }
}

// true when a failed dump stopped before it had deleted anything. Nothing changed for the
// other partitions then, so an ALL run can go on with the next one.
static bool failed_before_replacing(const BisDumpResult* r) {
    return strcmp(r->stage, "plan") == 0 || strcmp(r->stage, "open") == 0 ||
           strcmp(r->stage, "replace") == 0;
}

// Bytes taken by the files an earlier dump left for this partition (0 when there are none),
// or -1 when they exist but cannot be measured.
static s64 existing_bytes(const BisPartition* p) {
    if (!bis_outputs_exist(g_sd, OUT_DIR, p->name)) return 0;
    return bis_outputs_size(g_sd, OUT_DIR, p->name);
}

// The same over every openable partition.
static s64 existing_bytes_all(void) {
    s64 total = 0;
    for (int i = 0; i < g_table.count; i++) {
        if (!g_table.parts[i].openable) continue;
        const s64 b = existing_bytes(&g_table.parts[i]);
        if (b < 0) return -1;
        total += b;
    }
    return total;
}

// One dialog line about the SD card's space for a dump of `need` bytes, where deleting the
// earlier dump frees `replaced` bytes (-1: there is one but it could not be measured). Returns
// whether it fits. A dump replaces the earlier one in place, so that space does count.
static bool describe_space(s64 need, s64 replaced, char* line, size_t cap) {
    char a[24], b[24];
    if (!g_have_free) {
        snprintf(line, cap, "SD card free space: unknown.");
        return true;
    }
    nbe_fmt_size(a, sizeof a, g_free);
    if (replaced > 0) {
        nbe_fmt_size(b, sizeof b, g_free + replaced);
        snprintf(line, cap, "SD card free space: %s, %s once the old dump is deleted.", a, b);
    } else if (replaced < 0) {
        snprintf(line, cap, "SD card free space: %s (the old dump could not be measured).", a);
    } else {
        snprintf(line, cap, "SD card free space: %s.", a);
    }
    return g_free + (replaced > 0 ? replaced : 0) >= need;
}

static void run_dump(const BisPartition* p) {
    g_last_draw = 0; // show the first progress line at once
    bis_dump(p, g_sd, OUT_DIR, g_split, &g_cbs, &g_dump);
    log_result(p, &g_dump);
    refresh_free();
}

// Ask before dumping one partition. Returns false when the user backs out; sets g_quit if the
// dialog asked for the app to exit.
static bool ask_dump_one(const BisPartition* p) {
    const char* lines[6];
    char text[6][NBE_LINE_MAX], sz[24], file[64];
    int n = 0;
    BisPlan plan;

    refresh_free();
    bis_plan(&plan, OUT_DIR, p->name, p->size, g_split);
    file_name_of(file, sizeof file, plan.base);

    nbe_fmt_size(sz, sizeof sz, p->size);
    snprintf(text[n], sizeof text[n], "Dump %s (id %u, %s) to the SD card?", p->name, p->id, sz);
    lines[n] = text[n];
    n++;
    if (plan.multi)
        snprintf(text[n], sizeof text[n], "writes %s.00 .. .%02d: %d files of up to 4 GiB each",
                 file, plan.parts - 1, plan.parts);
    else
        snprintf(text[n], sizeof text[n], "writes %s (%s)", file, sz);
    lines[n] = text[n];
    n++;

    const s64 existing = existing_bytes(p);
    const bool fits = describe_space(p->size, existing, text[n], sizeof text[n]);
    lines[n] = text[n];
    n++;
    if (!fits) {
        lines[n] = "NOT ENOUGH FREE SPACE on the SD card.";
        n++;
    }
    if (existing != 0) {
        lines[n] = "An earlier dump of this partition is deleted FIRST. If this dump";
        n++;
        lines[n] = "then fails or is cancelled, that earlier one is gone.";
        n++;
    }

    const int a = ui_confirm(lines, n, fits ? "start" : NULL, "back");
    if (a < 0) g_quit = true;
    return a == 1;
}

static void action_dump_all(void) {
    if (g_table.openable_count == 0) {
        ui_log("nothing to dump: no partition could be opened");
        return;
    }

    const char* lines[8];
    char text[8][NBE_LINE_MAX], sz[24];
    int n = 0;
    refresh_free();

    nbe_fmt_size(sz, sizeof sz, g_table.openable_total);
    snprintf(text[n], sizeof text[n], "Dump ALL %d openable partition(s), %s in total?",
             g_table.openable_count, sz);
    lines[n] = text[n];
    n++;
    lines[n] = g_split ? "dumps over 4 GiB are split into 4 GiB files."
                       : "files are never split: only correct on an exFAT card.";
    n++;

    const s64 existing = existing_bytes_all();
    const bool fits = describe_space(g_table.openable_total, existing, text[n], sizeof text[n]);
    lines[n] = text[n];
    n++;
    if (!fits) {
        lines[n] = "NOT ENOUGH FREE SPACE on the SD card.";
        n++;
    }
    if (existing != 0) {
        lines[n] = "Each partition's earlier dump is deleted right before its new one";
        n++;
        lines[n] = "is written. If that write fails, the earlier dump is gone.";
        n++;
    }

    const int a = ui_confirm(lines, n, fits ? "start" : NULL, "back");
    if (a < 0) g_quit = true;
    if (a != 1) return;

    int ok = 0, failed = 0;
    bool stopped = false;
    for (int i = 0; i < g_table.count; i++) {
        const BisPartition* p = &g_table.parts[i];
        if (!p->openable) continue;
        g_last_draw = 0;
        const BisDumpStatus s = bis_dump(p, g_sd, OUT_DIR, g_split, &g_cbs, &g_dump);
        log_result(p, &g_dump);
        if (s == BisDump_Cancelled) {
            stopped = true;
            break;
        }
        if (s == BisDump_Ok) {
            ok++;
            continue;
        }
        failed++;
        if (!failed_before_replacing(&g_dump)) {
            // This one failed after its earlier dump was already gone (card or storage
            // trouble). Going on would cost each next partition its earlier dump before
            // meeting the same problem, so stop here.
            ui_log("ALL: stopping at %s; the partitions after it were not touched", p->name);
            stopped = true;
            break;
        }
    }
    ui_log("ALL: %d dumped, %d failed%s", ok, failed, stopped ? ", stopped early" : "");
    refresh_free();
}

static void action_dump_selected(void) {
    if (g_sel == 0) {
        action_dump_all();
        return;
    }
    const BisPartition* p = &g_table.parts[g_sel - 1];
    if (!p->openable) {
        char rs[64];
        nbe_fmt_result(rs, sizeof rs, p->open_rc);
        ui_log("%s (id %u) cannot be dumped: %s", p->name, p->id, rs);
        return;
    }
    if (ask_dump_one(p)) run_dump(p);
}

// ---------------------------------------------------------------------------------------------
// Partition table and partial dumps
// ---------------------------------------------------------------------------------------------

static BisGpt g_gpt; // the last table read; several KiB, so not on the stack

static const BisPartition* selected_partition(void) {
    return g_sel > 0 ? &g_table.parts[g_sel - 1] : NULL;
}

// Show the GUID partition table at the start of the selected partition, and keep it as text.
static void action_show_gpt(void) {
    const BisPartition* p = selected_partition();
    static char rows[BIS_GPT_MAX_ENTRIES][96];
    const char* lines[BIS_GPT_MAX_ENTRIES + 4];
    char title[NBE_LINE_MAX], more[NBE_LINE_MAX], rs[64], line[NBE_LINE_MAX];
    int n = 0, used = 0, shown = 0;
    // Rows left for entries under the title, the column heads and the prompt.
    const int room = g_ch - 10;

    if (!p) {
        ui_log("ALL has no partition table of its own: select a partition first");
        return;
    }
    consoleClear();
    snprintf(line, sizeof line, "reading the partition table at the start of %s ...", p->name);
    put(3, CYAN, line);
    consoleUpdate(NULL);
    bis_read_gpt(p->id, &g_gpt);

    if (R_FAILED(g_gpt.rc)) {
        nbe_fmt_result(rs, sizeof rs, g_gpt.rc);
        ui_log("%s: the partition table could not be read [%s]", p->name, rs);
        return;
    }
    if (!g_gpt.found) {
        ui_log("%s: no partition table at its start", p->name);
        return;
    }
    const Result wrc = bis_write_gpt(g_sd, OUT_DIR, p->name, &g_gpt);
    if (R_FAILED(wrc)) {
        nbe_fmt_result(rs, sizeof rs, wrc);
        ui_log("%s: partition table read, but %s.gpt.txt could not be written [%s]", p->name,
               p->name, rs);
    } else {
        ui_log("%s: partition table read (%u entries, %llu-byte blocks), kept as %s.gpt.txt",
               p->name, g_gpt.entries, (unsigned long long)g_gpt.sector_size, p->name);
    }

    snprintf(title, sizeof title, "Partition table at the start of %s  (%llu-byte blocks)", p->name,
             (unsigned long long)g_gpt.sector_size);
    lines[n++] = title;
    lines[n++] = "name                             start        size";
    for (u32 i = 0; i < g_gpt.entries; i++) {
        char a[24], b[24];
        if (!g_gpt.table[i].name[0]) continue;
        used++;
        if (shown >= room) continue;
        nbe_fmt_size(a, sizeof a, (s64)(g_gpt.table[i].first_lba * g_gpt.sector_size));
        nbe_fmt_size(b, sizeof b, bis_gpt_size_of(&g_gpt, (int)i));
        snprintf(rows[shown], sizeof rows[0], "%-32.32s %10s  %10s", g_gpt.table[i].name, a, b);
        lines[n++] = rows[shown];
        shown++;
    }
    if (used > shown) {
        snprintf(more, sizeof more, "... %d of %d entries shown. All of them are in %s.gpt.txt",
                 shown, used, p->name);
        lines[n++] = more;
    }
    if (ui_confirm(lines, n, NULL, "back") < 0) g_quit = true;
}

// The lowest set bit of `v` (0 for 0): when several move keys are down, the lowest acts.
static u64 lowest_bit(u64 v) { return v & (0ULL - v); }

// Let the user choose where a partial dump of `p` ends. Returns 1 to start (*end holds the
// end), 0 to go back, -1 to exit the app.
static int ui_pick_end(const BisPartition* p, s64* end, bool from_table) {
    char text[8][NBE_LINE_MAX];
    const u64 t0 = bis_now_ns();
    u64 repeat_key = 0, repeat_at = 0; // the move key being repeated, and when it repeats
    // What one press moves the end by: a 32nd of the partition (32 MiB at most), and on the
    // shoulder buttons a quarter (1 GiB at most).
    s64 small = p->size / 32, big = p->size / 4;
    if (small > (s64)(32 * BIS_MIB)) small = (s64)(32 * BIS_MIB);
    if (big > (s64)BIS_GIB) big = (s64)BIS_GIB;
    small &= ~(s64)511;
    big &= ~(s64)511;
    if (small < 512) small = 512;
    if (big < small) big = small;

    for (;;) {
        char sz[24], all[24], a[24], b[24];
        int n = 0;
        bool fits;

        if (!appletMainLoop()) return -1;
        nbe_fmt_size(sz, sizeof sz, *end);
        nbe_fmt_size(all, sizeof all, p->size);
        snprintf(text[n], sizeof text[n], "Partial dump of %s: from offset 0 up to", p->name);
        n++;
        snprintf(text[n], sizeof text[n], "     %s   (0x%llx bytes)", sz, (unsigned long long)*end);
        n++;
        nbe_fmt_size(a, sizeof a, small);
        nbe_fmt_size(b, sizeof b, big);
        snprintf(text[n], sizeof text[n], "of %s in total.", all);
        n++;
        snprintf(text[n], sizeof text[n], "[Left] [Right] -/+ %s    [L] [R] -/+ %s    hold to repeat",
                 a, b);
        n++;
        snprintf(text[n], sizeof text[n], "%s",
                 from_table ? "The end is the start of USER, taken from the partition table."
                            : "No partition table found: starting from the full size.");
        n++;
        fits = describe_space(*end, 0, text[n], sizeof text[n]); // never replaces another dump
        n++;
        if (!fits) {
            snprintf(text[n], sizeof text[n], "NOT ENOUGH FREE SPACE on the SD card.");
            n++;
        }

        consoleClear();
        for (int i = 0; i < n; i++) put(3 + i, WHITE, text[i]);
        snprintf(text[n], sizeof text[n], "%s", fits ? "[A] start      [B] back" : "[B] back");
        put(3 + n + 1, fits ? CYAN : RED, text[n]);
        consoleUpdate(NULL);

        padUpdate(&g_pad);
        const u64 k = padGetButtonsDown(&g_pad);
        const u64 held = padGetButtons(&g_pad);
        const u64 now = bis_now_ns();
        if (now - t0 > 300000000ULL) { // ignore the press that opened the dialog
            if (fits && (k & HidNpadButton_A)) return 1;
            if (k & HidNpadButton_B) return 0;
            if (k & HidNpadButton_Plus) return -1;

            // A move key acts when it is pressed, and again while it is held: after
            // REPEAT_DELAY_NS, then every REPEAT_INTERVAL_NS. A key that is still held from
            // before the window ended counts as pressed now, so a quick press-and-hold is
            // not lost.
            const u64 move_keys =
                HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_L | HidNpadButton_R;
            u64 move = 0;
            bool repeating = false;
            if (k & move_keys) {
                move = lowest_bit(k & move_keys); // pressed now
            } else if (held & repeat_key) {
                if (now >= repeat_at) { // held: repeat
                    move = repeat_key;
                    repeating = true;
                }
            } else if (held & move_keys) {
                move = lowest_bit(held & move_keys); // held since before, or another one still down
            }
            if (move) {
                const s64 step = (move & (HidNpadButton_L | HidNpadButton_R)) ? big : small;
                const int dir = (move & (HidNpadButton_Right | HidNpadButton_R)) ? 1 : -1;
                s64 e = *end + dir * step;
                if (e > p->size) e = p->size; // the whole partition
                else if (e < 512) e = 512;    // one block
                *end = e;
                from_table = false;
                repeat_key = move;
                repeat_at = now + (repeating ? REPEAT_INTERVAL_NS : REPEAT_DELAY_NS);
            }
        }
        svcSleepThread(16 * 1000 * 1000LL);
    }
}

// Dump the selected partition from its start up to an end the user picks. When the partition
// starts with a table that has a USER entry, the end starts at USER's first byte.
static void action_dump_partial(void) {
    const BisPartition* p = selected_partition();
    BisRange range;
    s64 end, user;
    bool from_table = false;
    char rs[64], line[NBE_LINE_MAX];

    if (!p) {
        ui_log("ALL cannot be dumped partially: select a partition");
        return;
    }
    if (!p->openable) {
        nbe_fmt_result(rs, sizeof rs, p->open_rc);
        ui_log("%s cannot be opened [%s]", p->name, rs);
        return;
    }

    consoleClear();
    snprintf(line, sizeof line, "reading the partition table at the start of %s ...", p->name);
    put(3, CYAN, line);
    consoleUpdate(NULL);
    bis_read_gpt(p->id, &g_gpt);
    end = p->size;
    user = bis_gpt_start_of(&g_gpt, "USER");
    if (g_gpt.found && user > 0 && user < p->size) {
        end = user;
        from_table = true;
    }

    const int a = ui_pick_end(p, &end, from_table);
    if (a < 0) g_quit = true;
    if (a != 1) return;

    range.start = 0;
    range.length = end;
    g_last_draw = 0;
    bis_dump_range(p, g_sd, OUT_DIR, g_split, &range, &g_cbs, &g_dump);
    log_result(p, &g_dump);
    refresh_free();
}

// ---------------------------------------------------------------------------------------------
// Network reader (see bis_serve() in bis.c for the requests)
// ---------------------------------------------------------------------------------------------

static void log_served(const BisServeResult* r) {
    char sz[24], spd[24], hms[16], hex[65], rs[64];
    const u64 ms = r->elapsed_ns / 1000000ULL;

    if (r->request[0] == '\0') {
        ui_log("network: a reader connected and left");
        return;
    }
    nbe_fmt_size(sz, sizeof sz, r->sent);
    if (r->refused) {
        nbe_fmt_result(rs, sizeof rs, r->rc);
        ui_log("network: %s: refused at %s [%s]", r->request, r->stage, rs);
        return;
    }
    if (r->cancelled) {
        ui_log("network: %s: stopped after %s", r->request, sz);
        return;
    }
    if (R_FAILED(r->rc)) {
        nbe_fmt_result(rs, sizeof rs, r->rc);
        ui_log("network: %s: not completed (at %s, after %s) [%s]", r->request, r->stage, sz, rs);
        return;
    }
    if (r->sent > 0) {
        nbe_fmt_size(spd, sizeof spd, ms ? (s64)((u64)r->sent * 1000 / ms) : 0);
        nbe_fmt_hms(hms, sizeof hms, ms / 1000);
        ui_log("network: %s: sent %s in %s (%s/s)", r->request, sz, hms, spd);
    } else {
        ui_log("network: answered %s", r->request);
    }
    if (strncmp(r->request, "HASH", 4) == 0) {
        nbe_fmt_hex32(r->sha256, hex);
        ui_log("  sha256 %s", hex);
    }
}

// ZR: turn the reader on and off. On, a program on the PC can pull partition bytes.
static void action_toggle_network(void) {
    char rs[64], line[NBE_LINE_MAX];

    if (g_listen >= 0) {
        net_close(g_listen);
        g_listen = -1;
        ui_log("network reader off");
        return;
    }
    if (!g_net_ready) {
        consoleClear();
        snprintf(line, sizeof line, "starting the socket driver ...");
        put(3, CYAN, line);
        consoleUpdate(NULL);
        const Result rc = net_init();
        if (R_FAILED(rc)) {
            nbe_fmt_result(rs, sizeof rs, rc);
            ui_log("network: the socket driver did not start [%s]", rs);
            return;
        }
        g_net_ready = true;
    }
    Result rc = 0;
    const int fd = net_listen_port(&rc, NBE_NET_PORT);
    if (fd < 0) {
        nbe_fmt_result(rs, sizeof rs, rc);
        ui_log("network: listening on port %d failed [%s]", NBE_NET_PORT, rs);
        return;
    }
    g_listen = fd;
    ui_log("network reader listening on port %d: pull from the PC with net_pull.py", NBE_NET_PORT);
}

// Shown while a reader makes us wait (net_idle): since when, and until when we wait.
static void draw_waiting(const BisServeResult* r, u64 idle_ns) {
    char line[NBE_LINE_MAX];
    consoleClear();
    snprintf(line, sizeof line, "%s v%s", NBE_APP_NAME, NBE_APP_VERSION);
    put(0, CYAN, line);
    if (r->request[0] != '\0') {
        put(3, WHITE, "waiting for the reader:");
        snprintf(line, sizeof line, "request: %s", r->request);
        put(4, WHITE, line);
    } else {
        put(3, WHITE, "a reader is connected and sends no request.");
    }
    snprintf(line, sizeof line, "nothing has gone through for %llu s; it is let go of at %llu s",
             (unsigned long long)(idle_ns / SECOND_NS),
             (unsigned long long)(NET_STALL_NS / SECOND_NS));
    put(6, WHITE, line);
    put(g_ch - 1, CYAN, "[B] let this reader go   [+] exit");
    consoleUpdate(NULL);
}

// Asked by the transport while a reader makes us wait (NetConn in net.h): at most every 16 ms,
// with the time since a byte last went through. Keeps the pad and the system answered, shows
// that we wait, and lets go of a reader that stays silent.
static bool net_idle(void* user, u64 idle_ns) {
    static u64 shown_second = ~0ULL;
    static const char* const lines[] = {
        "Let this reader go?",
        "It keeps what it got, and can ask for the rest.",
    };
    BisServeResult* r = user;

    if (!appletMainLoop()) { // the system wants us gone
        g_net_let_go = "the system asked us to exit";
        return false;
    }
    padUpdate(&g_pad);
    if (padGetButtonsDown(&g_pad) & (HidNpadButton_B | HidNpadButton_Plus)) {
        if (ask_stop(lines, "let go") != 0) {
            g_net_let_go = "you let it go";
            return false;
        }
    }
    if (idle_ns >= NET_STALL_NS) {
        g_net_let_go = "it went silent";
        return false;
    }
    if (idle_ns / SECOND_NS != shown_second) { // the screen shows whole seconds
        shown_second = idle_ns / SECOND_NS;
        draw_waiting(r, idle_ns);
    }
    return true;
}

// What came of a connection: its last request (log_served), or why it was let go of.
static void log_connection(const BisServeResult* r) {
    char sz[24], hms[16];
    const char* why = g_net_let_go;
    g_net_let_go = NULL;
    if (why == NULL) {
        log_served(r);
        return;
    }
    nbe_fmt_hms(hms, sizeof hms, NET_STALL_NS / SECOND_NS);
    if (strcmp(why, "it went silent") != 0) {
        ui_log("network: connection let go (%s)", why);
    } else if (strcmp(r->stage, "send") == 0) { // in the middle of sending an answer
        nbe_fmt_size(sz, sizeof sz, r->sent);
        ui_log("network: %s: the reader took nothing for %s: let go after %s", r->request, hms, sz);
    } else if (r->request[0] == '\0') { // it never asked for anything
        ui_log("network: a reader connected and sent nothing for %s: let go", hms);
    } else {
        ui_log("network: the reader went silent for %s after %s: let go", hms, r->request);
    }
}

// Called once per pass of the main loop while the reader is on: serves one reader, if one has
// connected, and returns. That takes as long as the reader keeps asking, or until it is let
// go of; the pad and the system are answered all the while (net_idle).
static void net_step(void) {
    const int fd = net_accept(g_listen);
    if (fd < 0) return;
    NetConn c;
    BisServeResult r; // of this connection: filled by bis_serve(), shown by log_connection()
    net_conn_init(&c, fd);
    if (!c.nonblocking && !g_net_said_blocking) {
        g_net_said_blocking = true;
        ui_log("network: this console's sockets cannot be made non-blocking: a reader that goes "
               "silent can keep the app until it is restarted");
    }
    net_conn_set_idle(&c, net_idle, &r);
    bis_serve(&c.link, &g_table, &g_cbs, &r);
    net_close(fd);
    log_connection(&r);
}

// The one directory this app writes to. Returns 0 when it is there (made now or earlier).
static Result ensure_out_dir(void) {
    char dir[FS_MAX_PATH] = OUT_DIR;
    const Result rc = fsFsCreateDirectory(g_sd, dir);
    if (R_SUCCEEDED(rc)) return 0;
    FsDirEntryType type = FsDirEntryType_File;
    if (R_SUCCEEDED(fsFsGetEntryType(g_sd, dir, &type)) && type == FsDirEntryType_Dir) return 0;
    return rc;
}

static void action_rescan(void) {
    char sz[24];
    // On screen before the scan, not after: should the system not answer for some partition
    // id, this is what the screen shows.
    consoleClear();
    put(3, CYAN, "scanning: asking for every partition id from 0 to 255 ...");
    consoleUpdate(NULL);
    bis_scan(&g_table);
    nbe_fmt_size(sz, sizeof sz, g_table.openable_total);
    ui_log("scan: %d partition(s) listed, %d openable (%s in total)", g_table.count,
           g_table.openable_count, sz);
    if (g_table.truncated) ui_log("  list truncated at %d entries", BIS_MAX_PARTITIONS);
    if (g_sel >= menu_rows()) g_sel = menu_rows() - 1;
}

// Optional experiment, see README "Files larger than 4 GiB".
static void action_bigfile_test(void) {
    static const char* const lines[] = {
        "SD card test: a file of exactly 4 GiB",
        "Creates an empty 4 GiB file, reads its stored size back, then",
        "deletes it. No data is written; 4 GiB must be free.",
        "On FAT32 this asks the filesystem for a length it cannot store.",
        "How it answers is what this test records.",
        "Nothing else on the card is written.",
    };
    const int a = ui_confirm(lines, 6, "run test", "back");
    if (a < 0) g_quit = true;
    if (a != 1) return;

    BisBigFileResult r;
    char sz[24], rs[64];
    bis_bigfile_test(g_sd, OUT_DIR, (s64)BIS_BIGFILE_TEST_SIZE, &r);

    if (R_FAILED(r.create_rc)) {
        nbe_fmt_result(rs, sizeof rs, r.create_rc);
        ui_log("4GiB test: CreateFile refused: %s", rs);
        ui_log("  -> refused: FAT32, or under 4 GiB free (free 4 GiB and run again to tell)");
    } else if (r.accepts) {
        ui_log("4GiB test: a 4 GiB file was created and read back with the right length");
        ui_log("  -> this card takes files over 4 GiB (exFAT): single-file dumps work here");
    } else {
        nbe_fmt_size(sz, sizeof sz, r.stored_size);
        nbe_fmt_result(rs, sizeof rs, r.reopen_rc);
        ui_log("4GiB test: created, but the length reads back as %s [%s]", sz, rs);
        ui_log("  -> lengths do not survive on this card: splitting is REQUIRED");
    }
    if (R_FAILED(r.delete_rc)) {
        nbe_fmt_result(rs, sizeof rs, r.delete_rc);
        ui_log("  warning: _bigfile_test.bin could not be deleted: %s", rs);
    }
    refresh_free();
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    g_con = consoleInit(NULL);
    if (g_con && g_con->consoleWidth >= 40 && g_con->consoleHeight >= 20) {
        g_cw = g_con->consoleWidth;
        g_ch = g_con->consoleHeight;
    }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    g_sd = fsdevGetDeviceFileSystem("sdmc");
    if (!g_sd) {
        printf("\x1b[10;1H" RED "The SD card is not available.\n" RESET
               "Dumps and the log are written to\n" WHITE "sdmc:%s/\n" RESET
               "\nPress [+] to exit.", OUT_DIR);
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&g_pad);
            if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) break;
            svcSleepThread(16 * 1000 * 1000LL);
        }
        consoleExit(NULL);
        return 0;
    }

    const Result dir_rc = ensure_out_dir(); // cannot be made: said in the log below
    g_logf = fopen(LOG_PATH, "a");
    refresh_free();

    const u32 fw = hosversionGet();
    ui_log("---- %s v%s (%s, fw %u.%u.%u) ----", NBE_APP_NAME, NBE_APP_VERSION, nbe_platform(),
           HOSVER_MAJOR(fw), HOSVER_MINOR(fw), HOSVER_MICRO(fw));
    ui_log("read-only towards NAND; output sdmc:%s/", OUT_DIR);
    // A long transfer is not to be cut short by the console falling asleep. When the console
    // will not promise that, it is said here: whoever runs a long transfer keeps it awake.
    const Result sleep_rc = appletSetAutoSleepDisabled(true);
    if (R_FAILED(sleep_rc)) {
        char rs[64];
        nbe_fmt_result(rs, sizeof rs, sleep_rc);
        ui_log("the console could not be asked not to sleep (%s): keep it awake during a long transfer",
               rs);
    } else {
        ui_log("the console will not fall asleep on its own while this app runs");
    }
    if (R_FAILED(dir_rc)) {
        char rs[64];
        nbe_fmt_result(rs, sizeof rs, dir_rc);
        ui_log("the output directory could not be made (%s): dumps will fail", rs);
    }
    action_rescan();

    u64 repeat_at = 0;
    int repeat_dir = 0;
    while (!g_quit && appletMainLoop()) {
        padUpdate(&g_pad);
        const u64 down = padGetButtonsDown(&g_pad);
        const u64 held = padGetButtons(&g_pad);
        const int rows = menu_rows();
        const u64 now = bis_now_ns();

        if (down & HidNpadButton_AnyUp) repeat_dir = -1;
        else if (down & HidNpadButton_AnyDown) repeat_dir = 1;
        else if (!(held & (HidNpadButton_AnyUp | HidNpadButton_AnyDown))) repeat_dir = 0;

        if (repeat_dir && (down & (HidNpadButton_AnyUp | HidNpadButton_AnyDown) || now >= repeat_at)) {
            g_sel = (g_sel + repeat_dir + rows) % rows;
            repeat_at = now + (down ? REPEAT_DELAY_NS : REPEAT_INTERVAL_NS);
        }

        if (down & HidNpadButton_A) action_dump_selected();
        if (down & HidNpadButton_X) {
            g_split = !g_split;
            ui_log("dumps over 4 GiB are %s", g_split ? "split into 4 GiB files"
                                                       : "NOT split: only correct on exFAT");
        }
        if (down & HidNpadButton_Y) action_rescan();
        if (down & HidNpadButton_R) action_bigfile_test();
        if (down & HidNpadButton_L) action_show_gpt();
        if (down & HidNpadButton_ZL) action_dump_partial();
        if (down & HidNpadButton_ZR) action_toggle_network();
        if (down & (HidNpadButton_Plus | HidNpadButton_Minus)) break;

        if (g_listen >= 0) net_step();

        draw_menu();
        svcSleepThread(16 * 1000 * 1000LL);
    }

    if (g_listen >= 0) net_close(g_listen);
    if (g_logf) fclose(g_logf);
    consoleExit(NULL);
    return 0;
}
