// Scripted runs of the console UI (source/main.c, compiled as nbe_app_main). What the app
// printf()s is laid out on a text screen and the pad answers from a script (test/ui_shim.c), so
// the flows can be checked without a console. The UI keeps its state in statics, hence one
// scenario per process:
//
//     test_ui <scenario> <work directory>        test/ui_test.sh runs them all
//
// Each scenario sets up a simulated console, writes the button script, runs the app, and then
// checks what was on screen (ui_frame*), what is on the "SD card", and that the script was used
// up exactly: a step left over means the app did not go where the script expected.
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ui_shim.h"
#include "net.h"
#include "bis.h"
#include "nbe_fmt.h"

int nbe_app_main(int argc, char** argv);

#define OUT_DIR "/cheeky-nands"
#define C ((s64)BIS_CHUNK_SIZE)
#define P ((s64)BIS_PART_SIZE)
enum { BLIND = 22 }; // polls for a dialog's 300 ms start-up window to pass (19 are needed)

static FILE* g_report; // the real stdout; the app's stdout is the screen
static int g_checks, g_failures;
static char g_work[500], g_sd[700], g_img[700];

static void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_report, fmt, ap);
    va_end(ap);
    fputc('\n', g_report);
    fflush(g_report);
}

#define CHECK(cond, ...) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            fprintf(g_report, "  FAIL %s:%d: ", __FILE__, __LINE__); \
            say(__VA_ARGS__); \
        } \
    } while (0)

// ---- fixtures --------------------------------------------------------------------------------

// A fresh simulated console. The SD card is g_sd, which the app creates itself.
static void world(int sd_card) {
    shim_init(g_sd);
    ui_reset_script();
    ui_sd_card(sd_card, g_sd);
}

// Path buffers that are reused in turn: a value stays valid until four paths later.
static char g_paths[4][1500];
static int g_path_i;

static const char* in(const char* dir, const char* fmt, ...) {
    char rel[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof rel, fmt, ap);
    va_end(ap);
    char* out = g_paths[g_path_i++ & 3];
    snprintf(out, sizeof g_paths[0], "%s%s", dir, rel);
    return out;
}
#define SD(...) in(g_sd, __VA_ARGS__)
#define IMG(...) in(g_img, __VA_ARGS__)

// A copy of a path that has to survive later path calls.
static const char* keep(const char* s) {
    static char kept[4][1500];
    static int i;
    char* out = kept[i++ & 3];
    snprintf(out, sizeof kept[0], "%s", s);
    return out;
}

// A partition image for `id`, registered with the shim.
static const char* storage(u32 id, const char* name, s64 size, u64 seed) {
    const char* p = IMG("/%s", name);
    shim_make_image(p, size, seed, NULL, 0);
    shim_storage_set(id, p);
    return keep(p);
}

// The part files of a dump called `stem` (a dump of `len` bytes), joined in order.
static const char* concat_dump(const char* stem, s64 len) {
    static char cat[700];
    BisPlan plan;
    snprintf(cat, sizeof cat, "%s/concat.bin", g_work);
    FILE* c = fopen(cat, "wb");
    CHECK(c != NULL, "cannot open the concatenation file");
    bis_plan(&plan, OUT_DIR, stem, len, true);
    if (c) {
        for (int i = 0; i < plan.parts; i++) {
            char rel[FS_MAX_PATH];
            FILE* f;
            bis_part_path(&plan, i, rel);
            f = fopen(SD("%s", rel), "rb");
            CHECK(f != NULL, "cannot open %s", rel);
            if (f) {
                u8 buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, c);
                fclose(f);
            }
        }
        fclose(c);
    }
    return cat;
}

// A file holding the bytes [off, off + len) of the image, to compare a dump with.
static const char* image_slice(const char* img, s64 off, s64 len) {
    static char out[700];
    snprintf(out, sizeof out, "%s/slice.bin", g_work);
    FILE* f = fopen(img, "rb");
    FILE* o = fopen(out, "wb");
    CHECK(f != NULL && o != NULL, "cannot open %s", img);
    if (f && o) {
        u8 buf[65536];
        s64 left = len;
        fseek(f, (long)off, SEEK_SET);
        while (left > 0) {
            const size_t n = left < (s64)sizeof buf ? (size_t)left : sizeof buf;
            if (fread(buf, 1, n, f) != n) {
                CHECK(0, "the image ended early");
                break;
            }
            fwrite(buf, 1, n, o);
            left -= (s64)n;
        }
    }
    if (f) fclose(f);
    if (o) fclose(o);
    return out;
}

static s64 fsize(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (s64)st.st_size : -1;
}

static char g_log[65536];

static bool log_has(const char* text) {
    FILE* f = fopen(SD(OUT_DIR "/log.txt"), "rb");
    g_log[0] = '\0';
    if (f) {
        const size_t n = fread(g_log, 1, sizeof g_log - 1, f);
        g_log[n] = '\0';
        fclose(f);
    }
    return strstr(g_log, text) != NULL;
}

static int count_outputs(const char* prefix) {
    DIR* d = opendir(SD(OUT_DIR));
    int n = 0;
    if (!d) return -1;
    const struct dirent* e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) n++;
    closedir(d);
    return n;
}

// `pattern` with its one %s replaced by a formatted size.
static const char* sized(const char* pattern, s64 size) {
    static char out[4][200];
    static int i;
    char s[24];
    nbe_fmt_size(s, sizeof s, size);
    char* o = out[i++ & 3];
    snprintf(o, sizeof out[0], pattern, s);
    return o;
}

// The menu row of partition `id` (row 0 is ALL): row i + 1 shows the i-th entry of the list.
// (This opens every partition id once: mind it where OpenBisStorage calls are counted.)
static int menu_row(u32 id) {
    BisTable t;
    bis_scan(&t);
    for (int i = 0; i < t.count; i++)
        if (t.parts[i].id == id) return i + 1;
    return -1;
}

// Select `id` and open its dump dialog. The next press answers the dialog.
static void open_dialog(u32 id) {
    const int row = menu_row(id);
    CHECK(row > 0, "partition %u is not in the list", id);
    ui_idle(1);
    ui_repeat(HidNpadButton_Down, row > 0 ? row : 0);
    ui_press(HidNpadButton_A);
    ui_idle(BLIND);
}

static int seen(const char* text) { return ui_frame_with(text, 0) >= 0; }
static int never(const char* text) { return !seen(text); }

// ---- scenarios -------------------------------------------------------------------------------

static void s_list_and_dump(void) {
    const s64 SIZE0 = C + 100; // two chunks, one part: here a part is exactly two chunks
    world(1);
    const char* img0 = storage(0, "a0.bin", SIZE0, 1);
    storage(30, "a30.bin", C + 100, 2);
    storage(77, "a77.bin", 512, 3);
    shim_storage_deny(27, 0x320002);

    ui_idle(1);
    ui_repeat(HidNpadButton_Down, 1);
    ui_press(HidNpadButton_A);
    ui_idle(BLIND);
    ui_press(HidNpadButton_A);
    ui_idle(2); // one progress callback per chunk
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(ui_frame_count() > 5, "%d frames were shown", ui_frame_count());
    CHECK(seen("cheeky-nands v0.1"), "no title");
    CHECK(never("[host-test, fw"), "the title still names the console and its firmware");
    CHECK(seen("\nscan: 21 partition(s) listed, 3 openable"), "no scan line");
    CHECK(seen("\n>  ALL    every openable partition"), "ALL is not selected at start");
    CHECK(seen(" 0] BootPartition1Root") && seen("  BOOT0"), "no row for id 0");
    CHECK(seen(" 30] User") && seen("  USER"), "no row for id 30");
    CHECK(seen(" 27] CalibrationBinary") && seen("-  0x00320002 PermissionDenied"),
          "a refused id does not show why");
    CHECK(seen(" 77] Unknown_77"), "no row for an unknown id");
    CHECK(seen("34] SignedSystemPartitionOnSafeMode "),
          "a name of 31 characters is cut in the list (the longest in the table)");
    CHECK(seen(" 10] BootPartition2Root") && seen("-  0x002EE202 InvalidArgument"),
          "a partition the system does not have does not show why, completely");
    CHECK(seen(sized("[A] dump BootPartition1Root: %s -> BootPartition1Root.bin (1 file)", SIZE0)),
          "no hint for the selection");
    CHECK(seen(sized("Dump BootPartition1Root (id 0, %s) to the SD card?", SIZE0)), "no dialog");
    CHECK(seen("SD card free space: unknown."), "no free space line");
    CHECK(seen("[A] start      [B] back"), "no dialog buttons");
    CHECK(seen("dumping BootPartition1Root (id 0)   file 1 of 1"), "no progress screen");
    {
        char a[24], b[24], exp[160];
        nbe_fmt_size(a, sizeof a, C);
        nbe_fmt_size(b, sizeof b, SIZE0);
        const u64 pct10 = (u64)C * 1000 / (u64)SIZE0; // tenths of a percent after chunk one
        snprintf(exp, sizeof exp, "%s / %s   %llu.%llu%%", a, b, (unsigned long long)(pct10 / 10),
                 (unsigned long long)(pct10 % 10));
        CHECK(seen(exp), "no progress line after the first chunk");
    }
    CHECK(seen(sized("\nBootPartition1Root: ok, %s -> 1 file(s)", SIZE0)), "no ok line");
    CHECK(seen("\n  sha256 "), "no hash line");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
    CHECK(shim_files_equal(SD(OUT_DIR "/BootPartition1Root.bin"), img0), "dump differs from the image");
    CHECK(fsize(SD(OUT_DIR "/BootPartition1Root.sha256")) > 0 &&
              fsize(SD(OUT_DIR "/BootPartition1Root.info")) > 0,
          "side files missing");
    CHECK(log_has("BootPartition1Root: ok, "), "log file has no ok line");
    CHECK(count_outputs("BootPartition1Root") == 3, "%d files for the partition",
          count_outputs("BootPartition1Root"));
}

// The card is too small: no way to start, and nothing is written.
static void s_low_space(void) {
    world(1);
    storage(30, "b30.bin", 100000, 4);
    shim_free_space(1000);

    open_dialog(30);
    ui_press(HidNpadButton_A); // there is no [A] to answer
    ui_idle(BLIND);
    ui_press(HidNpadButton_B); // back
    ui_idle(1);
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("SD card free space: 1000 B."), "no free space line");
    CHECK(seen("\nNOT ENOUGH FREE SPACE on the SD card."), "no warning");
    CHECK(seen("[B] back"), "no way back");
    CHECK(never("[A] start"), "the dump could be started");
    CHECK(never("dumping User"), "the dump ran");
    CHECK(count_outputs("User") == 0 && !log_has("User:"), "something was written");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// The card is too small for the partition, unless the space of the dump that gets replaced
// counts: then it is enough. (With a check that ignores the old dump this scenario must fail.)
static void s_replace_frees_space(void) {
    const s64 size = P + 123;             // two parts
    const s64 old = size + 20 + 30;       // the earlier dump: the same data plus two text files
    const s64 free_now = size - 31195;    // 100000: too little now, enough once the old dump is gone
    world(1);
    const char* img = storage(30, "c30.bin", size, 5);
    mkdir(SD(OUT_DIR), 0755);

    // The earlier dump is made through the shim, so that the card's free space accounts for it
    // and deleting it really frees space.
    FsFileSystem fs = { g_sd };
    shim_free_space(free_now + old);
    CHECK(fsFsCreateFile(&fs, OUT_DIR "/User.bin.00", P, 0) == 0 &&
              fsFsCreateFile(&fs, OUT_DIR "/User.bin.01", 123, 0) == 0 &&
              fsFsCreateFile(&fs, OUT_DIR "/User.sha256", 20, 0) == 0 &&
              fsFsCreateFile(&fs, OUT_DIR "/User.info", 30, 0) == 0,
          "cannot make the earlier dump");

    open_dialog(30);
    ui_press(HidNpadButton_A); // start
    ui_idle(3);                // part 0 is two chunks, part 1 is one
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    {
        char a[24], b[24], exp[200];
        nbe_fmt_size(a, sizeof a, free_now);
        nbe_fmt_size(b, sizeof b, free_now + old);
        snprintf(exp, sizeof exp, "SD card free space: %s, %s once the old dump is deleted.", a, b);
        CHECK(seen(exp), "no free space line that counts the old dump");
    }
    CHECK(seen("\nAn earlier dump of this partition is deleted FIRST."),
          "no warning that the old dump goes first");
    CHECK(seen("writes User.bin.00 .. .01: 2 files of up to 4 GiB each"), "no file list");
    CHECK(seen(sized("\nUser: ok, %s -> 2 file(s)", size)), "no ok line");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
    CHECK(fsize(SD(OUT_DIR "/User.bin.00")) == P && fsize(SD(OUT_DIR "/User.bin.01")) == 123,
          "part lengths %lld and %lld", (long long)fsize(SD(OUT_DIR "/User.bin.00")),
          (long long)fsize(SD(OUT_DIR "/User.bin.01")));
    {
        // The parts in order must add up to the image.
        const char* cat = in(g_work, "/concat.bin");
        FILE* c = fopen(cat, "wb");
        FILE* f0 = fopen(SD(OUT_DIR "/User.bin.00"), "rb");
        FILE* f1 = fopen(SD(OUT_DIR "/User.bin.01"), "rb");
        CHECK(c && f0 && f1, "cannot open the parts");
        if (c && f0 && f1) {
            u8 buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f0)) > 0) fwrite(buf, 1, n, c);
            while ((n = fread(buf, 1, sizeof buf, f1)) > 0) fwrite(buf, 1, n, c);
        }
        if (c) fclose(c);
        if (f0) fclose(f0);
        if (f1) fclose(f1);
        CHECK(shim_files_equal(cat, img), "the parts do not add up to the image");
    }
}

// A dump fails after the old one is gone: ALL must stop, not cost every partition its old dump.
static void s_all_stops_after_a_loss(void) {
    world(1);
    storage(0, "d0.bin", C + 100, 6);
    storage(30, "d30.bin", C + 100, 7);
    shim_fail_write_at(0, 0x33334444, 1); // the first write of the first dump

    ui_idle(1);
    ui_press(HidNpadButton_A);
    ui_idle(BLIND);
    ui_press(HidNpadButton_A); // start
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen(sized("\nDump ALL 2 openable partition(s), %s in total?", 2 * (C + 100))), "no dialog");
    CHECK(seen("\nBootPartition1Root: FAILED at write [0x33334444]"), "no failure line with the code");
    CHECK(seen("\n  write to BootPartition1Root.bin at 0x0 failed"), "no failure message");
    CHECK(seen("\nALL: stopping at BootPartition1Root; the partitions after it were not touched"),
          "no stop line");
    CHECK(seen("\nALL: 0 dumped, 1 failed, stopped early"), "no summary");
    CHECK(never("dumping User"), "the next partition was started");
    CHECK(shim_reads_of(30) == 0, "User was read %u times", shim_reads_of(30));
    CHECK(count_outputs("User") == 0 && count_outputs("BootPartition1Root") == 0,
          "files left behind: User %d, BootPartition1Root %d", count_outputs("User"),
          count_outputs("BootPartition1Root"));
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// A partition that cannot be opened costs nothing: ALL goes on with the next one.
static void s_all_goes_on_past_a_refused_open(void) {
    world(1);
    storage(27, "e27.bin", C + 100, 8);
    const char* img30 = storage(30, "e30.bin", C + 100, 9);
    shim_storage_deny_nth(27, 0x320002, 2); // the app's scan is the first open of id 27

    ui_idle(1);
    ui_press(HidNpadButton_A);
    ui_idle(BLIND);
    ui_press(HidNpadButton_A); // start
    ui_idle(2);                // User is two chunks
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\nCalibrationBinary: FAILED at open [0x00320002 PermissionDenied]"),
          "no failure line for the refused partition");
    CHECK(seen("\n  OpenBisStorage(27) failed"), "no failure message");
    CHECK(seen("\n  PermissionDenied: this process may not open that partition"), "no hint");
    CHECK(seen("\nUser: ok, "), "User was not dumped");
    CHECK(seen("\nALL: 1 dumped, 1 failed\n"), "no summary");
    CHECK(shim_files_equal(SD(OUT_DIR "/User.bin"), img30), "dump differs from the image");
    CHECK(count_outputs("CalibrationBinary") == 0, "files for the refused partition");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

static void script_cancel(u64 answer, int app_returns_to_the_menu) {
    world(1);
    storage(30, "f30.bin", 3 * C + 5, 10);

    open_dialog(30);
    ui_press(HidNpadButton_A); // start
    ui_idle(1);                // chunk 1
    ui_press(HidNpadButton_B); // chunk 2: B asks whether to cancel
    ui_idle(BLIND);
    ui_press(answer);          // A: yes, cancel.  Plus: cancel, and exit the app
    if (app_returns_to_the_menu) ui_press(HidNpadButton_Plus);
}

static void s_cancel(void) {
    script_cancel(HidNpadButton_A, 1);
    nbe_app_main(0, NULL);

    CHECK(seen("\nCancel this dump?"), "no cancel dialog");
    CHECK(seen("\nUser: cancelled after ") && seen(sized("User: cancelled after %s", 2 * C)),
          "no cancelled line");
    CHECK(count_outputs("User") == 0, "%d files left behind", count_outputs("User"));
    CHECK(log_has("User: cancelled after"), "log file has no cancelled line");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

static void s_plus_in_cancel_dialog_exits(void) {
    script_cancel(HidNpadButton_Plus, 0); // the app exits by itself
    nbe_app_main(0, NULL);

    CHECK(seen("\nCancel this dump?"), "no cancel dialog");
    CHECK(count_outputs("User") == 0, "%d files left behind", count_outputs("User"));
    CHECK(log_has("User: cancelled after"),
          "the app left without unwinding the dump (log has no cancelled line)");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

static void s_no_sd_card(void) {
    world(0);

    ui_idle(1);
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("The SD card is not available."), "no message");
    CHECK(seen("Press [+] to exit."), "no way out named");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// More partitions than the list can show, and than the table holds: the selection must scroll
// and wrap inside the list. List row r (r >= 1) is the r-th entry of the scan.
static void s_scroll(void) {
    enum { DOWNS = 40, UPS = 41, ROWS = 24 }; // ROWS is LIST_MAX_ROWS in main.c
    BisTable t;
    char want[200];
    world(1);
    for (u32 id = 0; id < BIS_MAX_PARTITIONS + 6; id++) storage(id, "g.bin", 512, 11);
    bis_scan(&t);
    CHECK(t.count == BIS_MAX_PARTITIONS && t.truncated, "table: %d rows, truncated=%d", t.count,
          t.truncated);

    ui_idle(1);
    ui_repeat(HidNpadButton_Down, DOWNS);
    ui_repeat(HidNpadButton_Up, UPS);
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    const int rows = t.count + 1;
    CHECK(seen("\nscan: 64 partition(s) listed, 64 openable"), "no scan line");
    CHECK(seen("\n  list truncated at 64 entries"), "no truncation line");

    // Down from ALL: list row DOWNS is selected, and the list has scrolled to keep it in view.
    const int sel1 = DOWNS;
    snprintf(want, sizeof want, "\n>%3u] %s", t.parts[sel1 - 1].id, t.parts[sel1 - 1].name);
    int f = ui_frame_with(want, 0);
    CHECK(f >= 0, "no frame with list row %d selected (%s)", sel1, want + 1);
    if (f >= 0) {
        const char* text = ui_frame(f);
        const BisPartition* first = &t.parts[sel1 - ROWS];
        snprintf(want, sizeof want, "\n %3u] %s", first->id, first->name);
        CHECK(strstr(text, want) != NULL, "with row %d selected the list does not start at %s",
              sel1, first->name);
        CHECK(strstr(text, "ALL    every") == NULL, "ALL is still on screen after scrolling down");
    }

    // Then Up past the top: the selection wraps round to the last row.
    const int sel2 = ((sel1 - UPS) % rows + rows) % rows;
    CHECK(sel2 == rows - 1, "wrap arithmetic in this test: %d", sel2);
    snprintf(want, sizeof want, "\n>%3u] %s", t.parts[sel2 - 1].id, t.parts[sel2 - 1].name);
    f = ui_frame_with(want, 0);
    CHECK(f >= 0, "no frame with the last row selected");
    if (f >= 0) {
        const char* text = ui_frame(f);
        const BisPartition* first = &t.parts[sel2 - ROWS];
        snprintf(want, sizeof want, "\n %3u] %s", first->id, first->name);
        CHECK(strstr(text, want) != NULL, "at the end the list does not start at %s", first->name);
        int marks = 0;
        for (const char* q = text; (q = strstr(q, "\n>")) != NULL; q += 2) marks++;
        CHECK(marks == 1, "%d rows are marked as selected", marks);
    }
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

static void s_split_toggle(void) {
    world(1);

    ui_idle(1);
    ui_press(HidNpadButton_X);
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(ui_frame_with("split >4GiB: ON ", 0) >= 0, "split is not ON at start");
    CHECK(seen("\ndumps over 4 GiB are NOT split: only correct on exFAT"), "no log line for the toggle");
    CHECK(seen("split >4GiB: OFF"), "the footer does not show OFF");
    CHECK(ui_frame_with(", never split", 0) >= 0, "the ALL hint does not follow the toggle");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// ZL: the end starts at the full size and moves with the pad.
static void s_partial_manual(void) {
    static const char stem[] = "UserDataRoot+0x0-0x3e000";
    world(1);
    const char* img = storage(20, "j20.bin", 2 * P, 93);
    const int row = menu_row(20);

    ui_idle(1);
    ui_repeat(HidNpadButton_Down, row);
    ui_press(HidNpadButton_ZL);
    ui_idle(BLIND);
    ui_press(HidNpadButton_Left);   // -8 KiB
    ui_press(HidNpadButton_R);      // +64 KiB: past the end, so it stays at the end
    ui_press(HidNpadButton_Left);   // -8 KiB again
    ui_press(HidNpadButton_A);      // start
    ui_idle(4);                     // 0x3E000 bytes are four chunks
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\n     256 KiB   (0x40000 bytes)"), "the end does not start at the full size");
    CHECK(seen("[Left] [Right] -/+ 8 KiB    [L] [R] -/+ 64 KiB    hold to repeat"), "no key help with the step sizes");
    CHECK(seen("\n     248 KiB   (0x3e000 bytes)"), "Left did not move the end");
    CHECK(never("0x4e000 bytes"), "R moved the end past the end of the partition");
    CHECK(seen("\nUserDataRoot+0x0-0x3e000: ok, 248 KiB -> 2 file(s)"), "no ok line for the partial dump");
    CHECK(shim_files_equal(concat_dump(stem, 0x3E000), image_slice(img, 0, 0x3E000)),
          "the partial dump differs from the image");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// The end of the partial dump on the card: from the one "<name>+0x0-0x<end>.info" file.
static s64 partial_end_dumped(void) {
    DIR* d = opendir(SD(OUT_DIR));
    s64 end = -1;
    if (!d) return -1;
    const struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        unsigned long long v = 0;
        if (sscanf(e->d_name, "UserDataRoot+0x0-0x%llx.info", &v) == 1) end = (s64)v;
    }
    closedir(d);
    return end;
}

// Holding a move key repeats the move, so a long way down goes quickly; letting go stops it.
static void s_partial_repeat(void) {
    enum { SIZE = 2 * 0x20000, STEP = 0x2000, HOLD = 60 }; // STEP is a 32nd of SIZE
    char want[120];
    world(1);
    storage(20, "k20.bin", SIZE, 94);
    const int row = menu_row(20);

    ui_idle(1);
    ui_repeat(HidNpadButton_Down, row);
    ui_press(HidNpadButton_ZL);
    ui_idle(BLIND);
    ui_press(HidNpadButton_Left); // moves once...
    ui_hold(HidNpadButton_Left, HOLD); // ...and repeats while held
    ui_idle(3);                // let go: nothing more
    ui_press(HidNpadButton_A); // start
    ui_idle(12);               // more idle polls than the dump can use
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    const s64 end = partial_end_dumped();
    CHECK(end > 0, "no partial dump on the card");
    const int moves = end > 0 ? (int)((SIZE - end) / STEP) : -1;
    CHECK(moves >= 4 && moves <= 12, "%d moves from %d held polls: the repeat is missing, or too fast",
          moves, HOLD);
    // Each poll is 16 ms. The press moves at once; the repeats come after 400 ms (25 polls) and
    // then every 110 ms (7 polls): polls 22, 47, 54, 61, 68, 75, 82 of the dialog.
    CHECK(moves == 7, "%d moves from %d held polls, expected 7", moves, HOLD);
    snprintf(want, sizeof want, "\n     200 KiB   (0x%x bytes)", (unsigned)end);
    CHECK(seen(want), "the dialog never showed the end that was dumped (%s)", want + 1);
    CHECK(seen("\nUserDataRoot+0x0-0x32000: ok, 200 KiB -> 2 file(s)"), "no ok line for the partial dump");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// ZR turns the reader on, a reader's requests are served, ZR turns it off.
// What the reader got first: `head`, then `len` bytes of the image from its start. Returns
// where those bytes end in everything the app sent (0 when the head is not there).
static size_t check_answer_bytes(const char* head, const char* img, s64 len) {
    const char* sent = ui_net_sent();
    const size_t hlen = strlen(head);
    CHECK(strncmp(sent, head, hlen) == 0, "the reader's first answer is not %s", head);
    if (strncmp(sent, head, hlen) != 0) return 0;
    FILE* o = fopen(in(g_work, "/sent.bin"), "wb");
    CHECK(o != NULL, "cannot write what the reader got");
    if (o) {
        fwrite(sent + hlen, 1, (size_t)len, o);
        fclose(o);
    }
    CHECK(shim_files_equal(in(g_work, "/sent.bin"), image_slice(img, 0, len)),
          "the bytes sent differ from the image");
    return hlen + (size_t)len;
}

// That, followed by the hash line.
static void check_first_answer(const char* head, const char* img, s64 len) {
    const size_t end = check_answer_bytes(head, img, len);
    if (end != 0) CHECK(strncmp(ui_net_sent() + end, "SHA256 ", 7) == 0, "no hash after the bytes");
}

static void s_net_serve(void) {
    enum { ASK = 65536 };
    static const char head[] = "OK 0 65536 65636\n";
    world(1);
    const char* img = storage(30, "m30.bin", C + 100, 95);
    // As net_pull.py does it: one request per connection.
    ui_net_request("READ 30 0 65536");
    ui_net_request(""); // the reader closes its connection
    ui_net_request("READ 28 0 4096"); // a partition the console refuses to open
    ui_net_request("");

    ui_idle(1);
    ui_press(HidNpadButton_ZR); // on: the first connection is served in this pass
    ui_idle(1);                 // read by the progress poll of the chunk that was sent
    ui_idle(1);                 // a pass of the main loop: the second connection is served
    ui_press(HidNpadButton_ZR); // off
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    check_first_answer(head, img, ASK);
    const char* sent = ui_net_sent();
    CHECK(strstr(sent + strlen(head) + ASK, "ERR 002ee202 open\n") != NULL,
          "the read that was refused was not answered with ERR");
    CHECK(ui_net_listen_calls() == 1, "listen was called %d times", ui_net_listen_calls());
    CHECK(ui_sleep_disabled_asked(), "the console was not asked not to sleep");
    CHECK(seen("\nthe console will not fall asleep on its own while this app runs"),
          "no line that the console will stay awake");

    CHECK(seen("\nnetwork reader listening on port 28900: pull from the PC with net_pull.py"),
          "no line that the reader is listening");
    CHECK(seen("net: ON, port 28900"), "the header does not show the reader as on");
    CHECK(seen("sending over the network: User (id 30)"), "no sending screen");
    CHECK(seen("request: READ 30 0 65536"), "the sending screen does not show the request");
    CHECK(seen("\nnetwork: READ 30 0 65536: sent 64 KiB in 00:00:00 ("), "no line for what was sent");
    CHECK(seen("\nnetwork: READ 28 0 4096: refused at open [0x002EE202 InvalidArgument]"),
          "no line for the refused read");
    CHECK(never("\nnetwork: answered READ 28"), "the refused read is logged as answered");
    const int off = ui_frame_with("\nnetwork reader off", 0);
    CHECK(off >= 0 && strstr(ui_frame(off), "net: OFF") != NULL,
          "no line that the reader is off, or the header still shows it on");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// A reader that takes its time, but no more than it is given: the app waits, says so, and
// then answers.
static void s_net_wait(void) {
    enum { ASK = 65536 };
    static const char head[] = "OK 0 65536 65636\n";
    world(1);
    const char* img = storage(30, "m30.bin", C + 100, 95);
    ui_net_request("READ 30 0 65536");
    ui_net_request("");
    ui_net_silent(1, 119); // 119 seconds before its request: one short of being let go of

    ui_idle(1);
    ui_press(HidNpadButton_ZR); // on: the reader is there at once
    ui_idle(119);                // one pad poll per second of its silence
    ui_idle(1);                 // read by the progress poll of the chunk that is then sent
    ui_press(HidNpadButton_ZR); // off
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\na reader is connected and sends no request."), "no screen that a reader is waited for");
    CHECK(seen("\nnothing has gone through for 119 s; it is let go of at 120 s"),
          "no screen for how long the reader has been silent");
    CHECK(never("\nnetwork: a reader connected and sent nothing"), "a reader within its time was let go of");
    check_first_answer(head, img, ASK);
    CHECK(seen("\nnetwork: READ 30 0 65536: sent 64 KiB in 00:00:00 ("), "the request was not answered");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// A reader that connects and says nothing for longer than it is given is let go of; the app
// goes on listening, and the next reader is answered.
static void s_net_stall(void) {
    enum { ASK = 65536 };
    static const char head[] = "OK 0 65536 65636\n";
    world(1);
    const char* img = storage(30, "m30.bin", C + 100, 95);
    ui_net_request(""); // this reader would close its connection without a request ...
    ui_net_silent(1, 121); // ... after 121 seconds, which is longer than it is given
    ui_net_request("READ 30 0 65536"); // a later reader
    ui_net_request("");

    ui_idle(1);
    ui_press(HidNpadButton_ZR); // on: the silent reader is let go of in this pass
    ui_idle(120);                // one pad poll per second it was given
    ui_idle(1);                 // a pass of the main loop: the next reader is served
    ui_idle(1);                 // read by the progress poll of the chunk that is sent
    ui_press(HidNpadButton_ZR); // off
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\nnothing has gone through for 119 s; it is let go of at 120 s"),
          "no screen for how long the reader was silent");
    CHECK(seen("\nnetwork: a reader connected and sent nothing for 00:02:00: let go"),
          "no line that the silent reader was let go of");
    check_first_answer(head, img, ASK); // nothing of the silent one is in what was sent
    CHECK(seen("\nnetwork: READ 30 0 65536: sent 64 KiB in 00:00:00 ("), "the next reader was not answered");
    CHECK(ui_net_listen_calls() == 1, "listen was called %d times", ui_net_listen_calls());
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// A reader that asks for a partition and then takes nothing is let go of in the middle of the
// answer: the log says how much of it had gone through, and no hash pretends it is complete.
static void s_net_stall_sending(void) {
    enum { ASK = 65536 };
    static const char head[] = "OK 0 65636 65636\n";
    world(1);
    const char* img = storage(30, "m30.bin", C + 100, 95);
    ui_net_request("READ 30 0 0");
    ui_net_request("");
    // What is done on the connection, counted: 1 its request, 2 the app's "OK" line, 3 the
    // first chunk, 4 the second. Before that one the reader takes nothing any more.
    ui_net_silent(4, 121);

    ui_idle(1);
    ui_press(HidNpadButton_ZR); // on: the answer starts in this pass
    ui_idle(1);                 // read by the progress poll of the first chunk
    ui_idle(120);               // one pad poll per second the reader was given
    ui_press(HidNpadButton_ZR); // off
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\nwaiting for the reader:") && seen("\nrequest: READ 30 0 0"),
          "no screen that the reader is waited for in the middle of an answer");
    CHECK(seen("\nnetwork: READ 30 0 0: the reader took nothing for 00:02:00: let go after 64 KiB"),
          "no line that the reader was let go of, and after how much");
    const size_t end = check_answer_bytes(head, img, ASK);
    CHECK(end != 0 && ui_net_sent()[end] == '\0', "something went out after the chunk the reader took");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// B while a reader is waited for asks whether to let it go. A does, and the app goes on
// listening.
static void s_net_let_go(void) {
    world(1);
    ui_net_request("");
    ui_net_silent(1, 600); // a reader that would be silent for ten minutes

    ui_idle(1);
    ui_press(HidNpadButton_ZR); // on: the silent reader is there at once
    ui_press(HidNpadButton_B);  // in the first second of its silence: B asks
    ui_idle(BLIND);             // the dialog's start-up window
    ui_press(HidNpadButton_A);  // A: let it go
    ui_press(HidNpadButton_ZR); // off
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\nLet this reader go?"), "no dialog");
    CHECK(seen("\nnetwork: connection let go (you let it go)"), "no line that you let the reader go");
    CHECK(never("sent nothing for"), "letting a reader go was logged as its silence");
    const int on = ui_frame_with("\nnetwork: connection let go", 0);
    CHECK(on >= 0 && strstr(ui_frame(on), "net: ON, port 28900") != NULL,
          "the reader is not on any more after a reader was let go of");
    CHECK(strcmp(ui_net_sent(), "") == 0, "something was sent to a reader that asked nothing");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// A console whose sockets cannot be made non-blocking: said in the log, and the reader works.
static void s_net_blocking(void) {
    world(1);
    ui_net_nonblocking(false);
    ui_net_request("PING");
    ui_net_request("");
    ui_net_request("PING");
    ui_net_request("");

    ui_idle(1);
    ui_press(HidNpadButton_ZR); // on: the first reader is served in this pass
    ui_idle(1);                 // a pass of the main loop: the second one
    ui_press(HidNpadButton_ZR); // off
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("\nnetwork: this console's sockets cannot be made non-blocking"),
          "no warning that a silent reader can keep the app");
    CHECK(strcmp(ui_net_sent(), "PONG\nPONG\n") == 0, "the readers were not answered: %.40s",
          ui_net_sent());
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// A console that will not promise to stay awake: said in the log, and the app goes on.
static void s_sleep_refused(void) {
    world(1);
    ui_sleep_refused();

    ui_idle(1);
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(ui_sleep_disabled_asked(), "the console was not asked not to sleep");
    CHECK(seen("\nthe console could not be asked not to sleep (0x"),
          "no line that the console may fall asleep during a long transfer");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// The longest name in the table has 31 characters. It is shown whole, also in a row that
// opens, where the size follows it.
static void s_long_name(void) {
    world(1);
    storage(34, "a34.bin", C, 97);

    ui_idle(1);
    ui_press(HidNpadButton_Plus);
    nbe_app_main(0, NULL);

    CHECK(seen("34] SignedSystemPartitionOnSafeMode "),
          "the longest name is cut, or runs into its size, in a row that opens");
    CHECK(ui_script_left() == 0, "%d script steps were not consumed", ui_script_left());
}

// ---- runner ----------------------------------------------------------------------------------

static const struct { const char* name; void (*fn)(void); } g_scenarios[] = {
    { "list-and-dump", s_list_and_dump },
    { "low-space", s_low_space },
    { "replace-frees-space", s_replace_frees_space },
    { "all-stops-after-a-loss", s_all_stops_after_a_loss },
    { "all-goes-on-past-refused-open", s_all_goes_on_past_a_refused_open },
    { "cancel", s_cancel },
    { "plus-in-cancel-dialog", s_plus_in_cancel_dialog_exits },
    { "no-sd-card", s_no_sd_card },
    { "scroll", s_scroll },
    { "split-toggle", s_split_toggle },
    { "partial-manual", s_partial_manual },
    { "partial-repeat", s_partial_repeat },
    { "net-serve", s_net_serve },
    { "net-wait", s_net_wait },
    { "net-stall", s_net_stall },
    { "net-stall-sending", s_net_stall_sending },
    { "net-let-go", s_net_let_go },
    { "net-blocking", s_net_blocking },
    { "sleep-refused", s_sleep_refused },
    { "long-name", s_long_name },
};

int main(int argc, char** argv) {
    g_report = stdout;
    const int n = (int)(sizeof g_scenarios / sizeof g_scenarios[0]);
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < n; i++) printf("%s\n", g_scenarios[i].name);
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: test_ui <scenario> <work directory>   (--list names them)\n");
        return 2;
    }
    int i;
    for (i = 0; i < n && strcmp(g_scenarios[i].name, argv[1]) != 0; i++)
        ;
    if (i == n) {
        fprintf(stderr, "unknown scenario: %s\n", argv[1]);
        return 2;
    }

    snprintf(g_work, sizeof g_work, "%s", argv[2]);
    snprintf(g_sd, sizeof g_sd, "%s/sdcard", g_work);
    snprintf(g_img, sizeof g_img, "%s/images", g_work);
    mkdir(g_sd, 0755); // the card itself; the app makes its own directory inside it
    mkdir(g_img, 0755);
    shim_set_virtual_time(1);

    g_scenarios[i].fn();
    // Every scenario ends with the app being told to stop (or stopping itself). If it went
    // on after the script ran out, it did not stop for the reason the scenario assumes.
    CHECK(ui_polls_after_script() == 0, "the app went on for %d polls after the script ended",
          ui_polls_after_script());
    fprintf(g_report, "%s: %d checks, %d failed\n", g_scenarios[i].name, g_checks, g_failures);
    if (getenv("NBE_UI_DUMP")) { // every screen, for looking into a failure
        for (int k = 0; k < ui_frame_count(); k++) {
            fprintf(g_report, "==== frame %d ====\n", k);
            fputs(ui_frame(k), g_report);
        }
    }
    if (g_failures) {
        // The last screen, so that a failure can be read.
        fprintf(g_report, "---- last screen (%d frames in all) ----\n", ui_frame_count());
        fputs(ui_frame(ui_frame_count() - 1), g_report);
        fprintf(g_report, "---- %d script steps left ----\n", ui_script_left());
    }
    return g_failures ? 1 : 0;
}
