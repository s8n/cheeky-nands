// Host tests for the dump engine (source/bis.c, compiled unchanged with NBE_HOST_TEST).
// Part and chunk sizes come from the compile line, so multi-part logic runs with small files.
//
// Independent oracles: part contents are compared with the source image byte for byte, the
// .sha256 sidecar is checked by coreutils sha256sum -c, and the whole-image hash is compared
// with what sha256sum prints for the source image.
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include "shim.h"
#include "bis.h"
#include "nbe_fmt.h"

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

#define OUT_DIR "/dumps"
#define RC_READ_FAIL  0x11112222
#define RC_WRITE_FAIL 0x33334444

static char g_root[512], g_sd[600], g_img[600];
static FsFileSystem g_fs;

typedef struct {
    int calls;
    int cancel_at; // -1: never; else return false at this call (1-based)
    s64 last_done;
} Ctx;

static BisProgress g_last_pg;    // the last report, for tests that look at its fields
static BisPartition g_last_part; // a copy of its row: the engine's need not outlive the call

static bool on_progress(void* user, const BisProgress* pg) {
    Ctx* c = user;
    g_last_pg = *pg;
    g_last_pg.part = NULL;
    if (pg->part) {
        g_last_part = *pg->part;
        g_last_pg.part = &g_last_part;
    }
    c->calls++;
    c->last_done = pg->done;
    CHECK(pg->done > 0 && pg->done <= pg->total, "progress done %lld / %lld", (long long)pg->done,
          (long long)pg->total);
    CHECK(pg->part_index >= 0 && pg->part_index < pg->parts, "part index %d of %d",
          pg->part_index, pg->parts);
    return c->cancel_at < 0 || c->calls < c->cancel_at;
}

// ---- world -----------------------------------------------------------------------------------

#ifndef NBE_TEST_TMPDIR
#define NBE_TEST_TMPDIR "build-host/tmp" // project-relative; test/run.sh passes an absolute one
#endif

// Remove a file, or a directory with everything below it.
static void remove_tree(const char* path) {
    DIR* d = opendir(path);
    if (d) {
        const struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[1200];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            remove_tree(sub);
        }
        closedir(d);
    }
    remove(path);
}

// mkdir -p.
static void make_dirs(const char* path) {
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    for (char* s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        mkdir(buf, 0755);
        *s = '/';
    }
    mkdir(buf, 0755);
}

static bool g_world_ready;

static void remove_world(void) {
    if (g_world_ready) remove_tree(g_root);
}

// A fresh simulated console: an empty SD card, no partition images, no faults. The first call
// creates a run directory under NBE_TEST_TMPDIR; the process exit removes it again.
static void world(void) {
    if (!g_world_ready) {
        char tmpl[1200];
        make_dirs(NBE_TEST_TMPDIR);
        snprintf(tmpl, sizeof tmpl, "%s/run-XXXXXX", NBE_TEST_TMPDIR);
        const char* d = mkdtemp(tmpl);
        if (!d) {
            printf("cannot create a run directory in %s: %s\n", NBE_TEST_TMPDIR, strerror(errno));
            exit(2);
        }
        if (strlen(d) > 300) {
            printf("NBE_TEST_TMPDIR is too long for the test's path buffers: %s\n", d);
            exit(2);
        }
        snprintf(g_root, sizeof g_root, "%s", d);
        snprintf(g_sd, sizeof g_sd, "%s/sdcard", g_root);
        snprintf(g_img, sizeof g_img, "%s/images", g_root);
        atexit(remove_world);
        g_world_ready = true;
    }
    shim_init(g_sd);
    g_fs.root = g_sd;

    remove_tree(g_sd);
    remove_tree(g_img);
    make_dirs(g_sd);
    make_dirs(g_img);
    char dumps[1200];
    snprintf(dumps, sizeof dumps, "%s/dumps", g_sd);
    mkdir(dumps, 0755);
}

static void path_in(char* out, size_t cap, const char* dir, const char* fmt, ...) {
    char rel[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rel, sizeof rel, fmt, ap);
    va_end(ap);
    snprintf(out, cap, "%s%s", dir, rel);
}

static s64 fsize(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (s64)st.st_size : -1;
}

// Pseudo-random image, optionally with a fixed head (for the plaintext checks).
static void write_image(const char* rel, s64 size, u64 seed, const u8* head, size_t head_len) {
    char p[900];
    path_in(p, sizeof p, g_img, "/%s", rel);
    shim_make_image(p, size, seed, head, head_len);
    CHECK(fsize(p) == size, "image %s has size %lld", rel, (long long)fsize(p));
}

static bool files_equal(const char* a, const char* b) { return shim_files_equal(a, b); }

// What coreutils says about a file.
static void sha256sum_of(const char* p, char* out65) {
    char cmd[1200], hex[65] = "";
    snprintf(cmd, sizeof cmd, "sha256sum '%s' 2>/dev/null", p);
    FILE* f = popen(cmd, "r");
    out65[0] = '\0';
    if (f) {
        if (fscanf(f, "%64[0-9a-f]", hex) != 1) hex[0] = '\0';
        pclose(f);
    }
    snprintf(out65, 65, "%s", hex);
}

static int sha256sum_check(const char* rel_sha_file) {
    char cmd[1400];
    snprintf(cmd, sizeof cmd, "cd '%s/dumps' && sha256sum -c '%s' >/dev/null 2>&1", g_sd,
             rel_sha_file);
    return system(cmd);
}

// Number of entries directly under the output directory whose name starts with `prefix`.
static int count_outputs(const char* prefix) {
    char dir[700];
    snprintf(dir, sizeof dir, "%s/dumps", g_sd);
    DIR* d = opendir(dir);
    int n = 0;
    if (!d) return -1;
    const struct dirent* e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) n++;
    closedir(d);
    return n;
}

static BisPartition part_of(u32 id, const char* name, s64 size) {
    BisPartition p;
    memset(&p, 0, sizeof p);
    p.id = id;
    snprintf(p.name, sizeof p.name, "%s", name);
    p.openable = true;
    p.size = size;
    return p;
}

// Dump and compare every output with the source image. Returns the dump status.
static BisDumpStatus dump_and_verify(const char* img, u32 id, const char* name, s64 size, bool split,
                                     int expect_parts, Ctx* ctx, BisDumpResult* out) {
    char p[900], cat[900];
    BisPartition part = part_of(id, name, size);
    BisCallbacks cb = { ctx, on_progress, NULL };
    path_in(p, sizeof p, g_img, "/%s", img);
    shim_storage_set(id, p);

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, split, &cb, out);
    CHECK(s == BisDump_Ok && out->status == BisDump_Ok, "dump of %s: status %d stage %s rc %08x: %s",
          name, (int)s, out->stage ? out->stage : "?", (unsigned)out->rc, out->message);
    if (s != BisDump_Ok) return s;

    CHECK(out->parts == expect_parts, "%s: %d parts, expected %d", name, out->parts, expect_parts);
    CHECK(out->multi == (expect_parts > 1), "%s: multi=%d with %d parts", name, out->multi,
          expect_parts);

    // Concatenate the parts in order and compare with the source.
    path_in(cat, sizeof cat, g_root, "/concat.bin");
    FILE* c = fopen(cat, "wb");
    CHECK(c != NULL, "cannot open concat file");
    if (c) {
        BisPlan plan;
        bis_plan(&plan, OUT_DIR, name, size, split);
        for (int i = 0; i < expect_parts; i++) {
            char rel[FS_MAX_PATH], full[1100];
            bis_part_path(&plan, i, rel);
            path_in(full, sizeof full, g_sd, "%s", rel);
            const s64 want = bis_part_length(size, i, plan.multi);
            const s64 got = fsize(full);
            CHECK(got == want, "%s: length %lld, expected %lld", rel, (long long)got, (long long)want);
            FILE* f = fopen(full, "rb");
            CHECK(f != NULL, "cannot open %s", rel);
            if (f) {
                u8 buf[65536];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, c);
                fclose(f);
            }
        }
        fclose(c);
        CHECK(files_equal(cat, p), "%s: concatenated parts differ from the source image", name);
    }

    char want_hash[65], got_hash[65];
    sha256sum_of(p, want_hash);
    nbe_fmt_hex32(out->sha256, got_hash);
    CHECK(want_hash[0] != '\0', "sha256sum gave nothing for %s", p);
    CHECK(strcmp(want_hash, got_hash) == 0, "%s: whole-image hash %s, sha256sum says %s", name,
          got_hash, want_hash);

    char sha_rel[64];
    snprintf(sha_rel, sizeof sha_rel, "%s.sha256", name);
    CHECK(sha256sum_check(sha_rel) == 0, "%s: sha256sum -c rejects %s", name, sha_rel);

    char info[1100], key[96];
    path_in(info, sizeof info, g_sd, "%s/%s.info", OUT_DIR, name);
    snprintf(key, sizeof key, "size=%lld\n", (long long)size);
    FILE* f = fopen(info, "rb");
    char text[2048] = "";
    if (f) { const size_t n = fread(text, 1, sizeof text - 1, f); text[n] = '\0'; fclose(f); }
    CHECK(strstr(text, key) != NULL, "%s.info lacks \"%s\"", name, key);
    snprintf(key, sizeof key, "parts=%d\n", expect_parts);
    CHECK(strstr(text, key) != NULL, "%s.info lacks \"%s\"", name, key);
    snprintf(key, sizeof key, "sha256=%s\n", want_hash);
    CHECK(strstr(text, key) != NULL, "%s.info lacks the whole-image hash", name);

    // Nothing else may appear under this name, and the pre-sized files are the only outputs.
    CHECK(count_outputs(name) == expect_parts + 2, "%s: %d files start with the name, expected %d",
          name, count_outputs(name), expect_parts + 2);

    // The measured size of a dump is its data plus the two text files.
    {
        char sha_path[1100];
        path_in(sha_path, sizeof sha_path, g_sd, "%s/%s.sha256", OUT_DIR, name);
        const s64 want_total = size + fsize(sha_path) + fsize(info);
        const s64 got_total = bis_outputs_size(&g_fs, OUT_DIR, name);
        CHECK(got_total == want_total, "%s: outputs measured as %lld bytes, expected %lld", name,
              (long long)got_total, (long long)want_total);
    }
    return s;
}

static void check_no_outputs(const char* name) {
    const int n = count_outputs(name);
    CHECK(n == 0, "%s: %d output file(s) left behind", name, n);
}

// ---- tests -----------------------------------------------------------------------------------

#define P ((s64)BIS_PART_SIZE)
#define C ((s64)BIS_CHUNK_SIZE)

static void test_plan(void) {
    BisPlan plan;
    char p[FS_MAX_PATH];
    world();

    CHECK(bis_part_count(0, true) == 1, "count(0)");
    CHECK(bis_part_count(P, true) == 1, "count(P)");
    CHECK(bis_part_count(P + 1, true) == 2, "count(P+1)");
    CHECK(bis_part_count(2 * P, true) == 2, "count(2P)");
    CHECK(bis_part_count(2 * P + 1, true) == 3, "count(2P+1)");
    CHECK(bis_part_count(7 * P + 1, false) == 1, "count(7P+1, no split)");

    CHECK(bis_part_length(2 * P + 5, 0, true) == P, "len(0)");
    CHECK(bis_part_length(2 * P + 5, 1, true) == P, "len(1)");
    CHECK(bis_part_length(2 * P + 5, 2, true) == 5, "len(2)");
    CHECK(bis_part_length(2 * P + 5, 0, false) == 2 * P + 5, "len(0, no split)");

    bis_plan(&plan, "/out", "User", 2 * P + 5, true);
    CHECK(plan.multi && plan.parts == 3, "plan 3 parts");
    CHECK(strcmp(plan.base, "/out/User.bin") == 0, "base %s", plan.base);
    CHECK(strcmp(plan.sha, "/out/User.sha256") == 0, "sha %s", plan.sha);
    CHECK(strcmp(plan.info, "/out/User.info") == 0, "info %s", plan.info);
    bis_part_path(&plan, 1, p);
    CHECK(strcmp(p, "/out/User.bin.01") == 0, "part path %s", p);
    bis_plan(&plan, "/out", "User", P, true);
    bis_part_path(&plan, 0, p);
    CHECK(strcmp(p, "/out/User.bin") == 0 && !plan.multi, "single path %s", p);
}

static void test_single_file(void) {
    const s64 size = C + 12345; // not a chunk multiple, and below one part
    const u8 head[4] = { 0x00, 0x11, 0x22, 0x33 };
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    world();
    write_image("a.bin", size, 1, head, sizeof head);
    dump_and_verify("a.bin", 30, "ImgA", size, true, 1, &ctx, &r);
    CHECK(!r.used_bounce, "bounce used without a reason");
    CHECK(ctx.calls == 2, "progress called %d times, expected 2", ctx.calls);
    CHECK(r.plaintext == 0, "random image under a FAT id: plaintext=%d", r.plaintext);
    CHECK(strcmp(r.message, "") != 0 && strstr(r.message, "1 file"), "message: %s", r.message);
}

static void test_multi_part(void) {
    const s64 size = 2 * P + P / 2 + 123;
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    char full[1100];
    world();
    write_image("b.bin", size, 2, NULL, 0);
    dump_and_verify("b.bin", 0, "ImgB", size, true, 3, &ctx, &r);
    CHECK(r.plaintext == -1, "BOOT0 is not checked, plaintext=%d", r.plaintext);
    path_in(full, sizeof full, g_sd, "%s/ImgB.bin.02", OUT_DIR);
    CHECK(fsize(full) == P / 2 + 123, "last part is %lld bytes", (long long)fsize(full));
    path_in(full, sizeof full, g_sd, "%s/ImgB.bin", OUT_DIR);
    CHECK(fsize(full) == -1, "an un-suffixed ImgB.bin exists");
}

static void test_boundaries(void) {
    static const struct { s64 size; int parts; const char* label; } cases[] = {
        { 0, 1, "empty" },         { 1, 1, "one byte" },     { C, 1, "one chunk" },
        { C + 1, 1, "chunk + 1" }, { P - 1, 1, "part - 1" }, { P, 1, "exactly one part" },
        { P + 1, 2, "part + 1" },  { 2 * P, 2, "two parts" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        BisDumpResult r;
        Ctx ctx = { 0, -1, 0 };
        world();
        write_image("c.bin", cases[i].size, 100 + i, NULL, 0);
        dump_and_verify("c.bin", 21, "ImgC", cases[i].size, true, cases[i].parts, &ctx, &r);
        if (cases[i].size == 0)
            CHECK(ctx.calls == 0 && r.plaintext == -1, "empty dump: %d progress calls", ctx.calls);
    }
}

static void test_split_off(void) {
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    const s64 size = 3 * P + 7;
    world();
    write_image("d.bin", size, 4, NULL, 0);
    dump_and_verify("d.bin", 29, "ImgD", size, false, 1, &ctx, &r);
    CHECK(!r.multi, "multi set although split was off");
}

static void test_cancel(void) {
    const s64 size = 5 * C + 9;
    BisPartition part = part_of(30, "ImgE", size);
    BisDumpResult r;
    Ctx ctx = { 0, 3, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900];
    world();
    write_image("e.bin", size, 5, NULL, 0);
    path_in(img, sizeof img, g_img, "/e.bin");
    shim_storage_set(30, img);

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Cancelled, "status %d", (int)s);
    CHECK(ctx.calls == 3 && ctx.last_done == 3 * C, "cancelled after %d calls at %lld", ctx.calls,
          (long long)ctx.last_done);
    CHECK(r.status == BisDump_Cancelled && r.rc == 0, "result status %d rc %08x", (int)r.status,
          (unsigned)r.rc);
    CHECK(strstr(r.message, "cancelled") != NULL, "message: %s", r.message);
    check_no_outputs("ImgE");
}

static void test_read_failure(void) {
    const s64 size = 5 * C + 9;
    BisPartition part = part_of(30, "ImgE", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900];
    world();
    write_image("e.bin", size, 5, NULL, 0);
    path_in(img, sizeof img, g_img, "/e.bin");
    shim_storage_set(30, img);
    shim_fail_read_at(2 * C, RC_READ_FAIL, 1); // 2*C is one whole part: the first read of part 1

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error, "status %d", (int)s);
    CHECK(strcmp(r.stage, "read") == 0, "stage %s", r.stage);
    CHECK(r.rc == RC_READ_FAIL, "rc %08x", (unsigned)r.rc);
    CHECK(r.fail_offset == 2 * C && r.part_index == 1, "offset %lld part %d",
          (long long)r.fail_offset, r.part_index);
    CHECK(strstr(r.message, "read") != NULL, "message: %s", r.message);
    check_no_outputs("ImgE"); // part 0 was complete and must be gone too
}

static void test_read_failure_in_first_part(void) {
    const s64 size = 5 * C + 9;
    BisPartition part = part_of(30, "ImgE", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900];
    world();
    write_image("e.bin", size, 5, NULL, 0);
    path_in(img, sizeof img, g_img, "/e.bin");
    shim_storage_set(30, img);
    shim_fail_read_at(3 * C + 7, RC_READ_FAIL, 1); // no read starts here: never triggers
    shim_fail_read_at(C, RC_READ_FAIL, 1);

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "read") == 0, "status %d stage %s", (int)s, r.stage);
    CHECK(r.rc == RC_READ_FAIL && r.fail_offset == C && r.part_index == 0, "rc %08x offset %lld part %d",
          (unsigned)r.rc, (long long)r.fail_offset, r.part_index);
    check_no_outputs("ImgE");
}

static void test_write_failure(void) {
    // Several parts. Writes at file offset C happen once per part (C is half of P), so the
    // second one is part 1's: part 0 is complete by then and has to be cleaned up too.
    const s64 size = 3 * P + C;
    BisPartition part = part_of(30, "ImgE", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900];
    world();
    write_image("e.bin", size, 5, NULL, 0);
    path_in(img, sizeof img, g_img, "/e.bin");
    shim_storage_set(30, img);
    shim_fail_write_at(C, RC_WRITE_FAIL, 2);

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error, "status %d", (int)s);
    CHECK(strcmp(r.stage, "write") == 0, "stage %s", r.stage);
    CHECK(r.rc == RC_WRITE_FAIL, "rc %08x", (unsigned)r.rc);
    CHECK(r.fail_offset == P + C && r.part_index == 1, "offset %lld (want %lld) part %d",
          (long long)r.fail_offset, (long long)(P + C), r.part_index);
    check_no_outputs("ImgE"); // part 0 was complete and must be gone too
}

static void test_no_space(void) {
    const s64 size = P + 5;
    BisPartition part = part_of(30, "ImgE", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900];
    world();
    write_image("e.bin", size, 5, NULL, 0);
    path_in(img, sizeof img, g_img, "/e.bin");
    shim_storage_set(30, img);
    shim_free_space(size / 2);

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "create") == 0, "status %d stage %s", (int)s, r.stage);
    CHECK(r.rc == 0x4E02, "rc %08x", (unsigned)r.rc);
    CHECK(ctx.calls == 0, "NAND was read although the file could not be allocated");
    check_no_outputs("ImgE");
}

static void touch(const char* dir, const char* rel, const char* text) {
    char full[1100];
    FILE* f;
    path_in(full, sizeof full, dir, "%s", rel);
    f = fopen(full, "wb");
    CHECK(f != NULL, "cannot create %s", full);
    if (f) { fputs(text, f); fclose(f); }
}

static void test_overwrite(void) {
    const s64 size = 2 * P + 9; // three parts, like the stale dump, plus one stale extra part
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    char full[1100];
    world();
    write_image("f.bin", size, 6, NULL, 0);

    touch(g_sd, OUT_DIR "/ImgF.bin.00", "stale");
    touch(g_sd, OUT_DIR "/ImgF.bin.01", "stale");
    touch(g_sd, OUT_DIR "/ImgF.bin.02", "stale");
    touch(g_sd, OUT_DIR "/ImgF.bin.09", "stale extra part");
    touch(g_sd, OUT_DIR "/ImgF.sha256", "stale");
    touch(g_sd, OUT_DIR "/ImgF.info", "stale");
    touch(g_sd, OUT_DIR "/keep.bin.extra", "not ours");
    touch(g_sd, OUT_DIR "/notes.txt", "not ours");
    CHECK(bis_outputs_exist(&g_fs, OUT_DIR, "ImgF"), "stale outputs not noticed");
    // "stale" x5 and "stale extra part"; keep.bin.extra and notes.txt are not ours.
    CHECK(bis_outputs_size(&g_fs, OUT_DIR, "ImgF") == 5 * 5 + 16, "stale outputs measure as %lld",
          (long long)bis_outputs_size(&g_fs, OUT_DIR, "ImgF"));
    CHECK(bis_outputs_size(&g_fs, OUT_DIR, "NeverDumped") == 0, "a name never dumped measures as %lld",
          (long long)bis_outputs_size(&g_fs, OUT_DIR, "NeverDumped"));

    dump_and_verify("f.bin", 31, "ImgF", size, true, 3, &ctx, &r);

    path_in(full, sizeof full, g_sd, "%s/ImgF.bin.09", OUT_DIR);
    CHECK(fsize(full) == -1, "stale part 09 survived");
    path_in(full, sizeof full, g_sd, "%s/keep.bin.extra", OUT_DIR);
    CHECK(fsize(full) >= 0, "keep.bin.extra (not an output of ours) was deleted");
    path_in(full, sizeof full, g_sd, "%s/notes.txt", OUT_DIR);
    CHECK(fsize(full) >= 0, "notes.txt was deleted");

    CHECK(bis_delete_outputs(&g_fs, OUT_DIR, "ImgF") == 0, "delete outputs");
    check_no_outputs("ImgF");
    path_in(full, sizeof full, g_sd, "%s/keep.bin.extra", OUT_DIR);
    CHECK(fsize(full) >= 0, "bis_delete_outputs removed keep.bin.extra");
    CHECK(bis_delete_outputs(&g_fs, OUT_DIR, "ImgF") == 0, "deleting nothing is not an error");
    CHECK(!bis_outputs_exist(&g_fs, OUT_DIR, "ImgF"), "outputs still reported");
    CHECK(bis_outputs_size(&g_fs, OUT_DIR, "ImgF") == 0, "deleted outputs still measure as %lld",
          (long long)bis_outputs_size(&g_fs, OUT_DIR, "ImgF"));
    CHECK(!bis_outputs_exist(&g_fs, OUT_DIR, "NeverDumped"), "outputs reported for a new name");
}

// A dump that is refused must not cost the backup that was already there.
//
// Fixture: partition 30 is an image of `size` bytes (host file `image`), and the card carries
// the four files of an earlier "ImgK" dump.
static void k_world(s64 size, const char* image, char* img, size_t cap) {
    static const char* const stale[] = {
        OUT_DIR "/ImgK.bin.00", OUT_DIR "/ImgK.bin.01", OUT_DIR "/ImgK.sha256", OUT_DIR "/ImgK.info"
    };
    world();
    write_image(image, size, 50, NULL, 0);
    path_in(img, cap, g_img, "/%s", image);
    for (size_t i = 0; i < 4; i++) touch(g_sd, stale[i], "previous dump");
}

static void test_previous_dump_survives_a_refusal(void) {
    const s64 size = C + 5;
    const BisPartition part = part_of(30, "ImgK", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900], full[1100];
    BisDumpStatus s;

    // OpenBisStorage refuses (for example PermissionDenied).
    k_world(size, "k.bin", img, sizeof img);
    shim_storage_deny(30, 0x320002);
    s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "open") == 0 && r.rc == 0x320002,
          "refused open: %d %s %08x", (int)s, r.stage, (unsigned)r.rc);
    CHECK(count_outputs("ImgK") == 4, "the previous dump did not survive a refused OpenBisStorage");

    // GetSize answers with an error.
    k_world(size, "k.bin", img, sizeof img);
    shim_storage_set(30, img);
    shim_storage_getsize(0x2EE002, 0);
    s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "open") == 0 && r.rc == 0x2EE002,
          "GetSize error: %d %s %08x", (int)s, r.stage, (unsigned)r.rc);
    CHECK(count_outputs("ImgK") == 4, "the previous dump did not survive a failed GetSize");

    // GetSize answers with a negative length.
    k_world(size, "k.bin", img, sizeof img);
    shim_storage_set(30, img);
    shim_storage_getsize(0, -5);
    s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "open") == 0 && r.rc == NBE_RC_BAD_SIZE,
          "negative size: %d %s %08x", (int)s, r.stage, (unsigned)r.rc);
    CHECK(count_outputs("ImgK") == 4, "the previous dump did not survive a negative GetSize");

    // The real size needs more part files than the tool supports.
    k_world(size, "k.bin", img, sizeof img);
    write_image("k-big.bin", 150 * P, 51, NULL, 0);
    path_in(img, sizeof img, g_img, "/k-big.bin");
    shim_storage_set(30, img);
    s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "open") == 0 && strstr(r.message, "150"),
          "too many parts from the real size: %d %s %s", (int)s, r.stage, r.message);
    CHECK(count_outputs("ImgK") == 4, "the previous dump did not survive a refused plan");

    // One of the old files cannot be removed: the dump stops before writing anything, and
    // that file is left exactly as it was.
    k_world(size, "k.bin", img, sizeof img);
    shim_storage_set(30, img);
    shim_fail_delete(".sha256", 0xE02);
    s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "replace") == 0 && r.rc == 0xE02,
          "undeletable old file: %d %s %08x", (int)s, r.stage, (unsigned)r.rc);
    CHECK(ctx.calls == 0, "NAND was read although the old dump could not be removed");
    CHECK(count_outputs("ImgK") == 1, "%d old files left: expected only the one that was locked",
          count_outputs("ImgK"));
    path_in(full, sizeof full, g_sd, "%s/ImgK.sha256", OUT_DIR);
    CHECK(fsize(full) == (s64)strlen("previous dump"), "the locked file was changed");
    CHECK(strstr(r.message, "removing the previous") != NULL, "message: %s", r.message);
    CHECK(strstr(r.message, "leftovers") == NULL, "a failed deletion was retried: %s", r.message);

    // A dump into another directory that cannot be created does not touch these files.
    k_world(size, "k.bin", img, sizeof img);
    shim_storage_set(30, img);
    s = bis_dump(&part, &g_fs, "/no/such/directory", true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "create") == 0, "missing output directory: %d %s",
          (int)s, r.stage);
    CHECK(count_outputs("ImgK") == 4, "a dump aimed elsewhere touched this directory");

    // And once the dump really runs here, the earlier files are replaced.
    k_world(size, "k.bin", img, sizeof img);
    dump_and_verify("k.bin", 30, "ImgK", size, true, 1, &ctx, &r);
}

static void test_size_mismatch(void) {
    const s64 size = C + 5;
    BisPartition part = part_of(30, "ImgE", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char img[900];
    world();
    write_image("e.bin", size, 5, NULL, 0);
    path_in(img, sizeof img, g_img, "/e.bin");
    shim_storage_set(30, img);
    shim_size_skew(1); // the card reports one byte more than was asked for

    const BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "verify") == 0, "status %d stage %s", (int)s, r.stage);
    CHECK(r.rc == NBE_RC_SIZE_MISMATCH, "rc %08x", (unsigned)r.rc);
    CHECK(R_MODULE(r.rc) == NBE_MODULE && nbe_result_hint(r.rc) != NULL, "hint for %08x is missing",
          (unsigned)r.rc);
    check_no_outputs("ImgE");
}

static void test_scan(void) {
    BisTable t;
    const BisPartition* p;
    char img[900];
    world();
    write_image("s0.bin", 4096, 7, NULL, 0);
    write_image("s30.bin", 100000, 8, NULL, 0);
    write_image("s77.bin", 512, 9, NULL, 0);
    path_in(img, sizeof img, g_img, "/s0.bin");
    shim_storage_set(0, img);
    path_in(img, sizeof img, g_img, "/s30.bin");
    shim_storage_set(30, img);
    path_in(img, sizeof img, g_img, "/s77.bin");
    shim_storage_set(77, img);
    shim_storage_deny(27, 0x320002);

    bis_scan(&t);
    const int known = 20; // ids 0, 10, 20..36, 39
    CHECK(t.count == known + 1, "%d rows, expected %d", t.count, known + 1);
    CHECK(!t.truncated, "truncated");
    CHECK(t.openable_count == 3, "%d openable", t.openable_count);
    CHECK(t.openable_total == 4096 + 100000 + 512, "total %lld", (long long)t.openable_total);
    CHECK(shim_open_bis_calls() == BIS_SCAN_MAX_ID + 1, "%d OpenBisStorage calls",
          shim_open_bis_calls());

    for (int i = 1; i < t.count; i++)
        CHECK(t.parts[i - 1].id < t.parts[i].id, "rows not ascending at %d", i);

    p = &t.parts[0];
    CHECK(p->id == 0 && p->openable && p->size == 4096, "id 0 row");
    CHECK(strcmp(p->name, "BootPartition1Root") == 0 && p->known && p->alias, "id 0 naming");

    int seen27 = 0, seen10 = 0, seen77 = 0, seen78 = 0;
    for (int i = 0; i < t.count; i++) {
        p = &t.parts[i];
        if (p->id == 27) {
            seen27++;
            CHECK(!p->openable && p->open_rc == 0x320002 && p->size == 0, "id 27 row");
            CHECK(strcmp(p->name, "CalibrationBinary") == 0, "id 27 name %s", p->name);
            CHECK(strcmp(nbe_result_hint(p->open_rc), "PermissionDenied") == 0, "hint for 0x320002");
        }
        if (p->id == 10) {
            seen10++;
            CHECK(!p->openable && p->open_rc == 0x2EE202, "id 10: known but absent here");
            CHECK(strcmp(nbe_result_hint(p->open_rc), "InvalidArgument") == 0, "hint for 0x2EE202");
        }
        if (p->id == 77) {
            seen77++;
            CHECK(p->openable && !p->known && p->alias == NULL && p->size == 512, "id 77 row");
            CHECK(strcmp(p->name, "Unknown_77") == 0, "id 77 name %s", p->name);
        }
        if (p->id == 78) seen78++;
    }
    CHECK(seen27 == 1 && seen10 == 1 && seen77 == 1, "rows 27/10/77 seen %d/%d/%d", seen27, seen10,
          seen77);
    CHECK(seen78 == 0, "id 78 (absent, unnamed) got a row");

    const char* h = nbe_result_hint(0x307202);
    CHECK(h && strcmp(h, "FileExtensionWithoutOpenModeAllowAppend") == 0, "hint for 0x307202");
    CHECK(nbe_result_hint(0xD401) != NULL, "hint for 0xD401");
    CHECK(nbe_result_hint(MAKERESULT(2, 6400)) != NULL && nbe_result_hint(MAKERESULT(2, 6449)) != NULL,
          "PermissionDenied range ends not recognised");
    CHECK(nbe_result_hint(MAKERESULT(2, 6399)) == NULL && nbe_result_hint(MAKERESULT(2, 6450)) == NULL,
          "a result outside the PermissionDenied range got its name");
    CHECK(nbe_result_hint(MAKERESULT(3, 6400)) == NULL, "another module's 6400 was named");
}

static void test_scan_negative_size(void) {
    BisTable t;
    char img[900];
    int seen = 0;
    world();
    write_image("n.bin", 4096, 12, NULL, 0);
    path_in(img, sizeof img, g_img, "/n.bin");
    shim_storage_set(30, img);
    shim_storage_getsize(0, -1); // opens fine, but the length reads back negative

    bis_scan(&t);
    CHECK(t.openable_count == 0 && t.openable_total == 0,
          "a negative length was counted: %d openable, %lld bytes", t.openable_count,
          (long long)t.openable_total);
    for (int i = 0; i < t.count; i++) {
        CHECK(!t.parts[i].openable && t.parts[i].size == 0, "row %u listed as openable",
              t.parts[i].id);
        if (t.parts[i].id == 30) {
            seen++;
            CHECK(t.parts[i].open_rc == NBE_RC_BAD_SIZE, "id 30: rc %08x",
                  (unsigned)t.parts[i].open_rc);
        }
    }
    CHECK(seen == 1 && t.count == 20, "%d rows (id 30 seen %d times)", t.count, seen);
}

static void test_hidden_on_switch2(void) {
    static const u32 absent[] = { 21, 22, 23, 24, 25, 26, 29, 32, 34, 36, 39 };
    static const u32 kept[] = { 0, 10, 20, 27, 28, 30, 31, 33, 35, 37, 38, 40, 77, 255 };
    BisTable t;
    char img[900];

    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; i++)
        CHECK(bis_absent_on_switch2(absent[i]), "id %u is not on the absent list", absent[i]);
    for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++)
        CHECK(!bis_absent_on_switch2(kept[i]), "id %u is on the absent list", kept[i]);

    // A Switch 2 answers 0x002EE202 for those ids: no row for them, the others are unaffected.
    world();
    write_image("h20.bin", 4096, 21, NULL, 0);
    path_in(img, sizeof img, g_img, "/h20.bin");
    shim_storage_set(20, img);
    shim_storage_deny(21, 0x2EE202);
    shim_storage_deny(29, 0x2EE202);
    shim_storage_deny(39, 0x2EE202);

    bis_scan_as(&t, true);
    CHECK(t.count == 9, "%d rows on a Switch 2, expected 9", t.count);
    CHECK(t.openable_count == 1 && t.openable_total == 4096, "openable: %d, %lld bytes",
          t.openable_count, (long long)t.openable_total);
    for (int i = 0; i < t.count; i++)
        CHECK(!bis_absent_on_switch2(t.parts[i].id), "id %u was listed on a Switch 2",
              t.parts[i].id);

    // The same world as a Switch 1: every named id is listed, refused or not.
    bis_scan_as(&t, false);
    CHECK(t.count == 20, "%d rows on a Switch 1, expected 20", t.count);
    int seen21 = 0;
    for (int i = 0; i < t.count; i++)
        if (t.parts[i].id == 21) {
            seen21++;
            CHECK(!t.parts[i].openable && t.parts[i].open_rc == 0x2EE202, "id 21 row");
        }
    CHECK(seen21 == 1, "id 21 seen %d times on a Switch 1", seen21);

    // An id on the list that opens is listed even on a Switch 2: the filter is for ids that
    // are not there, not for ids the table names.
    world();
    write_image("h21.bin", 512, 22, NULL, 0);
    path_in(img, sizeof img, g_img, "/h21.bin");
    shim_storage_set(21, img);
    bis_scan_as(&t, true);
    int seen = 0;
    for (int i = 0; i < t.count; i++)
        if (t.parts[i].id == 21) {
            seen++;
            CHECK(t.parts[i].openable && t.parts[i].size == 512, "id 21 that opens was not listed");
        }
    CHECK(seen == 1, "id 21 seen %d times when it opens", seen);
}

static void test_scan_truncated(void) {
    BisTable t;
    char img[900];
    world();
    write_image("t.bin", 512, 13, NULL, 0);
    path_in(img, sizeof img, g_img, "/t.bin");
    for (u32 id = 0; id < BIS_MAX_PARTITIONS + 10; id++) shim_storage_set(id, img);

    bis_scan(&t);
    CHECK(t.count == BIS_MAX_PARTITIONS && t.truncated, "%d rows, truncated=%d", t.count,
          t.truncated);
    CHECK(t.openable_count == BIS_MAX_PARTITIONS &&
              t.openable_total == (s64)BIS_MAX_PARTITIONS * 512,
          "%d openable, %lld bytes", t.openable_count, (long long)t.openable_total);
    CHECK(t.parts[t.count - 1].id == BIS_MAX_PARTITIONS - 1, "last row is id %u",
          t.parts[t.count - 1].id);
    for (int i = 1; i < t.count; i++)
        CHECK(t.parts[i - 1].id < t.parts[i].id, "rows not ascending at %d", i);
}

static void test_bounce(void) {
    const s64 size = C + 17; // one full bounce-sized series of reads, then a short one
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    world();
    shim_unusable_over(BIS_CHUNK_SIZE); // the engine's chunk buffer cannot be mapped
    write_image("g.bin", size, 11, NULL, 0);
    dump_and_verify("g.bin", 30, "ImgG", size, true, 1, &ctx, &r);
    CHECK(r.used_bounce, "bounce path was not reported as used");
}

static void test_plaintext(void) {
    static u8 head[0x400];
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    world();

    memset(head, 0, sizeof head);
    head[0] = 0xEB;
    head[0x1FE] = 0x55;
    head[0x1FF] = 0xAA;
    write_image("p30.bin", C + 100, 20, head, sizeof head);
    dump_and_verify("p30.bin", 30, "ImgP", C + 100, true, 1, &ctx, &r);
    CHECK(r.plaintext == 1, "FAT boot sector under id 30: plaintext=%d", r.plaintext);

    world();
    memset(head, 0, sizeof head);
    memcpy(head + 0x200, "EFI PART", 8);
    write_image("p20.bin", C + 100, 21, head, sizeof head);
    dump_and_verify("p20.bin", 20, "ImgQ", C + 100, true, 1, &ctx, &r);
    CHECK(r.plaintext == 1, "GPT signature under id 20: plaintext=%d", r.plaintext);

    world();
    memset(head, 0, sizeof head);
    memcpy(head + 0x40, "CAL0", 4);
    write_image("p27.bin", C + 100, 22, head, sizeof head);
    dump_and_verify("p27.bin", 27, "ImgR", C + 100, true, 1, &ctx, &r);
    CHECK(r.plaintext == 1, "CAL0 magic under id 27: plaintext=%d", r.plaintext);

    world();
    write_image("p31.bin", C + 100, 23, NULL, 0);
    dump_and_verify("p31.bin", 31, "ImgS", C + 100, true, 1, &ctx, &r);
    CHECK(r.plaintext == 0, "random data under id 31: plaintext=%d", r.plaintext);
}

static void test_commit_failure_is_not_fatal(void) {
    const s64 size = C + 3;
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    world();
    shim_fail_commit(0x313A02);
    write_image("h.bin", size, 30, NULL, 0);
    dump_and_verify("h.bin", 30, "ImgH", size, true, 1, &ctx, &r);
    CHECK(strstr(r.message, "Commit") != NULL, "commit failure not mentioned: %s", r.message);
}

static void test_refusals(void) {
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    char out_dir[1200], img[900];
    BisPartition part = part_of(30, "ImgI", 150 * P);
    world();
    write_image("i.bin", 150 * P, 40, NULL, 0);
    path_in(img, sizeof img, g_img, "/i.bin");
    shim_storage_set(30, img);

    BisDumpStatus s = bis_dump(&part, &g_fs, OUT_DIR, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "plan") == 0, "too many parts: %d %s", (int)s, r.stage);
    CHECK(strstr(r.message, "150") != NULL, "message: %s", r.message);
    CHECK(shim_open_bis_calls() == 0 && ctx.calls == 0, "NAND was touched before the plan check");
    check_no_outputs("ImgI");

    memset(out_dir, 'x', sizeof out_dir);
    out_dir[0] = '/';
    out_dir[sizeof out_dir - 1] = '\0';
    part = part_of(30, "ImgJ", C);
    s = bis_dump(&part, &g_fs, out_dir, true, &cb, &r);
    CHECK(s == BisDump_Error && strcmp(r.stage, "plan") == 0, "path too long: %d %s", (int)s, r.stage);
    CHECK(strstr(r.message, "does not fit") != NULL, "message: %s", r.message);
}

static void test_fmt(void) {
    char b[64];
    world();
    nbe_fmt_size(b, sizeof b, 0);        CHECK(strcmp(b, "0 B") == 0, "size(0) = %s", b);
    nbe_fmt_size(b, sizeof b, 511);      CHECK(strcmp(b, "511 B") == 0, "size(511) = %s", b);
    nbe_fmt_size(b, sizeof b, 4096);     CHECK(strcmp(b, "4 KiB") == 0, "size(4096) = %s", b);
    nbe_fmt_size(b, sizeof b, 5 * 1024); CHECK(strcmp(b, "5 KiB") == 0, "size(5K) = %s", b);
    nbe_fmt_size(b, sizeof b, 5 * 1024 + 1);
    CHECK(strcmp(b, "5+ KiB") == 0, "size(5K+1) = %s", b);
    nbe_fmt_size(b, sizeof b, BIS_MIB);  CHECK(strcmp(b, "1.00 MiB") == 0, "size(1M) = %s", b);
    nbe_fmt_size(b, sizeof b, (s64)BIS_GIB * 5 / 2);
    CHECK(strcmp(b, "2.50 GiB") == 0, "size(2.5G) = %s", b);
    nbe_fmt_hms(b, sizeof b, 3725);      CHECK(strcmp(b, "01:02:05") == 0, "hms = %s", b);
    CHECK(strcmp(nbe_platform(), "host-test") == 0, "platform");
    CHECK(!nbe_is_switch2(), "the host test build calls itself a Switch 2");
}

// ---- partial dumps -----------------------------------------------------------------------------

static void read_text(const char* path, char* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    buf[0] = '\0';
    if (f) {
        const size_t n = fread(buf, 1, cap - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
}

// The part files of a dump called `stem` (`len` bytes, split) joined in order.
static const char* concat_dump(const char* stem, s64 len) {
    static char cat[1100];
    BisPlan plan;
    FILE* c;
    path_in(cat, sizeof cat, g_root, "/concat-range.bin");
    c = fopen(cat, "wb");
    CHECK(c != NULL, "cannot open the concatenation file");
    bis_plan(&plan, OUT_DIR, stem, len, true);
    if (c) {
        for (int i = 0; i < plan.parts; i++) {
            char rel[FS_MAX_PATH], full[1100];
            bis_part_path(&plan, i, rel);
            path_in(full, sizeof full, g_sd, "%s", rel);
            FILE* f = fopen(full, "rb");
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
    static char out[1100];
    path_in(out, sizeof out, g_root, "/slice.bin");
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

static void test_partial(void) {
    const s64 size = 3 * P + 5; // 0x60005
    const BisPartition part = part_of(20, "ImgR20", size);
    BisDumpResult r;
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    BisRange range;
    BisDumpStatus s;
    char img[900], stem[80], stem_b[80], full[1100], text[2048], key[96];
    const s64 end = 2 * P + 0x200;

    world();
    write_image("r.bin", size, 80, NULL, 0);
    path_in(img, sizeof img, g_img, "/r.bin");
    shim_storage_set(20, img);

    // From the start, stopping before the end: the range is in the name.
    bis_partial_stem(stem, sizeof stem, "ImgR20", 0, end);
    CHECK(strcmp(stem, "ImgR20+0x0-0x40200") == 0, "stem %s", stem);
    range.start = 0;
    range.length = end;
    s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
    CHECK(s == BisDump_Ok, "status %d stage %s: %s", (int)s, r.stage, r.message);
    CHECK(strcmp(r.stem, stem) == 0, "stem %s, expected %s", r.stem, stem);
    CHECK(r.parts == 3 && r.multi && r.size == end && r.start == 0 && r.source_size == size,
          "parts %d multi %d size %lld start %lld source %lld", r.parts, r.multi, (long long)r.size,
          (long long)r.start, (long long)r.source_size);
    path_in(full, sizeof full, g_sd, "%s/%s.bin.00", OUT_DIR, stem);
    CHECK(fsize(full) == P, "part 0 is %lld bytes", (long long)fsize(full));
    path_in(full, sizeof full, g_sd, "%s/%s.bin.01", OUT_DIR, stem);
    CHECK(fsize(full) == P, "part 1 is %lld bytes", (long long)fsize(full));
    path_in(full, sizeof full, g_sd, "%s/%s.bin.02", OUT_DIR, stem);
    CHECK(fsize(full) == 0x200, "part 2 is %lld bytes", (long long)fsize(full));
    CHECK(shim_files_equal(concat_dump(stem, end), image_slice(img, 0, end)),
          "the partial dump differs from the first %lld bytes of the image", (long long)end);
    CHECK(count_outputs(stem) == 5, "%d files for %s", count_outputs(stem), stem);
    path_in(full, sizeof full, g_sd, "%s/%s.info", OUT_DIR, stem);
    read_text(full, text, sizeof text);
    snprintf(key, sizeof key, "start=0\n");
    CHECK(strstr(text, key) != NULL, "%s.info lacks start", stem);
    snprintf(key, sizeof key, "source_size=%lld\n", (long long)size);
    CHECK(strstr(text, key) != NULL, "%s.info lacks the partition size", stem);
    snprintf(key, sizeof key, "size=%lld\n", (long long)end);
    CHECK(strstr(text, key) != NULL, "%s.info lacks the dumped length", stem);
    snprintf(key, sizeof key, "%s.sha256", stem);
    CHECK(sha256sum_check(key) == 0, "sha256sum -c rejects %s", key);
    // Nothing under the plain name: a partial dump is not the partition's dump.
    CHECK(count_outputs("ImgR20.bin") == 0, "%d files under the plain name", count_outputs("ImgR20.bin"));

    // Asking for the whole partition, as a range or with a length beyond the end: the plain name.
    range.start = 0;
    range.length = 0;
    s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
    CHECK(s == BisDump_Ok && strcmp(r.stem, "ImgR20") == 0 && r.parts == 4, "whole as a range: %d %s %d",
          (int)s, r.stem, r.parts);
    range.start = 0;
    range.length = 4 * P; // past the end of the partition: clamped to it
    s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
    CHECK(s == BisDump_Ok && strcmp(r.stem, "ImgR20") == 0 && r.size == size, "length past the end: %d %s",
          (int)s, r.stem);
    CHECK(shim_files_equal(concat_dump("ImgR20", size), img), "the whole dump differs from the image");
    CHECK(count_outputs("ImgR20+") == 5 && count_outputs("ImgR20.bin") == 4,
          "full %d, partial %d: the two dumps must leave each other alone",
          count_outputs("ImgR20.bin"), count_outputs("ImgR20+"));
    path_in(full, sizeof full, g_sd, "%s/ImgR20.info", OUT_DIR);
    read_text(full, text, sizeof text);
    CHECK(strstr(text, "start=") == NULL, "a whole dump's .info has a start line");

    // From an offset to the end.
    range.start = P;
    range.length = 0;
    s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
    bis_partial_stem(stem, sizeof stem, "ImgR20", P, size);
    CHECK(s == BisDump_Ok && strcmp(r.stem, stem) == 0 && r.parts == 3, "to the end: %d %s %d",
          (int)s, r.stem, r.parts);
    CHECK(shim_files_equal(concat_dump(stem, size - P), image_slice(img, P, size - P)),
          "the dump from 0x20000 differs from the image");

    // A range in the middle: one file.
    range.start = P;
    range.length = P;
    s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
    bis_partial_stem(stem, sizeof stem, "ImgR20", P, 2 * P);
    CHECK(s == BisDump_Ok && strcmp(r.stem, stem) == 0 && r.parts == 1 && !r.multi, "the middle: %d %s",
          (int)s, r.stem);
    path_in(full, sizeof full, g_sd, "%s/%s.bin", OUT_DIR, stem);
    CHECK(fsize(full) == P, "the middle dump is %lld bytes", (long long)fsize(full));
    CHECK(shim_files_equal(full, image_slice(img, P, P)), "the middle dump differs from the image");
    CHECK(r.start == P && r.size == P, "start %lld size %lld", (long long)r.start, (long long)r.size);

    // Ranges that are refused, before anything is opened or written.
    static const struct {
        s64 start, len;
        const char* stage;
    } bad[] = {
        { 513, 0, "plan" },       { 0, 513, "plan" },     { -512, 0, "plan" },   { 0, -512, "plan" },
        { 0x60200, 0, "open" },   { 0x60400, 512, "open" },  { 0, 1, "plan" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        range.start = bad[i].start;
        range.length = bad[i].len;
        s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
        CHECK(s == BisDump_Error && strcmp(r.stage, bad[i].stage) == 0,
              "range %lld +%lld: status %d stage %s, expected %s", (long long)bad[i].start,
              (long long)bad[i].len, (int)s, r.stage, bad[i].stage);
    }

    // A partial dump that is cancelled removes its own files and nobody else's.
    bis_partial_stem(stem_b, sizeof stem, "ImgR20", 2 * P, 3 * P);
    range.start = 2 * P;
    range.length = P;
    ctx.calls = 0;
    ctx.cancel_at = 1;
    s = bis_dump_range(&part, &g_fs, OUT_DIR, true, &range, &cb, &r);
    ctx.cancel_at = -1;
    CHECK(s == BisDump_Cancelled && count_outputs(stem_b) == 0, "a cancelled partial dump left files");
    // 5 + 5 + 3 files of the three partial dumps that were kept, and the full one's 4 parts.
    CHECK(count_outputs("ImgR20+") == 13 && count_outputs("ImgR20.bin") == 4,
          "after a cancelled partial dump: partial %d files (expected 13), full %d", count_outputs("ImgR20+"),
          count_outputs("ImgR20.bin"));
}

// ---- the network reader ------------------------------------------------------------------------

// A connection to a reader, in memory: `in` holds the request lines, `out` what was sent.
typedef struct {
    const char* in;
    size_t in_pos;
    char out[1 << 18];
    size_t out_len;
    size_t fail_after; // make send_all fail once this much was sent in all (0: never)
    int closes;        // how often recv_line reported the other side closed
} FakeLink;

static Result fake_send(void* user, const void* data, size_t len) {
    FakeLink* f = user;
    if (f->fail_after != 0 && f->out_len + len > f->fail_after) return NBE_RC_NET;
    if (f->out_len + len >= sizeof f->out) {
        CHECK(0, "the fake reader's buffer is too small for %zu more bytes", len);
        return NBE_RC_NET;
    }
    memcpy(f->out + f->out_len, data, len);
    f->out_len += len;
    return 0;
}

static int fake_recv(void* user, char* line, size_t cap) {
    FakeLink* f = user;
    if (f->in == NULL || f->in_pos >= strlen(f->in)) {
        f->closes++;
        return 0;
    }
    const char* rest = f->in + f->in_pos;
    const char* nl = strchr(rest, '\n');
    const size_t n = nl ? (size_t)(nl - rest) : strlen(rest);
    if (n + 1 > cap) return -1;
    memcpy(line, rest, n);
    line[n] = '\0';
    f->in_pos += n + (nl ? 1 : 0);
    return 1;
}

static void fake_link(FakeLink* f, NbeLink* link, const char* requests) {
    memset(f, 0, sizeof *f);
    f->in = requests;
    link->user = f;
    link->send_all = fake_send;
    link->recv_line = fake_recv;
}

// The reader's first answer is `head`, then `len` bytes of the image from `img_off`, then the
// hash of those bytes.
static void check_read_answer(FakeLink* f, const char* head, const char* img, s64 img_off,
                              s64 len) {
    char hex[65], got[1100];
    const size_t hlen = strlen(head);
    f->out[f->out_len] = '\0';
    if (strncmp(f->out, head, hlen) != 0) {
        CHECK(0, "the answer does not start with %s", head);
        return;
    }
    if ((s64)f->out_len < (s64)hlen + len) {
        CHECK(0, "%zu bytes for a %lld-byte answer", f->out_len, (long long)len);
        return;
    }
    path_in(got, sizeof got, g_root, "/got-over-the-network.bin");
    FILE* o = fopen(got, "wb");
    CHECK(o != NULL, "cannot write what the reader got");
    if (o) {
        fwrite(f->out + hlen, 1, (size_t)len, o);
        fclose(o);
    }
    CHECK(shim_files_equal(got, image_slice(img, img_off, len)), "the bytes sent differ from the image");
    sha256sum_of(got, hex);
    const char* tail = f->out + hlen + len;
    CHECK(strncmp(tail, "SHA256 ", 7) == 0 && strncmp(tail + 7, hex, 64) == 0,
          "the trailer is not the hash of the bytes sent");
}

static void test_serve(void) {
    const s64 SIZE = 3 * C + 5; // 196613
    static FakeLink f;          // a quarter MiB: not on the stack
    NbeLink link;
    BisTable t;
    BisServeResult r;
    char img[900], hex[65], want[200];
    Ctx ctx = { 0, -1, 0 };
    BisCallbacks cb = { &ctx, on_progress, NULL };
    const BisCallbacks nocb = { NULL, NULL, NULL };

    world();
    write_image("n30.bin", SIZE, 120, NULL, 0);
    path_in(img, sizeof img, g_img, "/n30.bin");
    shim_storage_set(30, img);
    shim_storage_deny(28, 0xE02);
    bis_scan(&t);

    // PING, LIST and a READ on one connection; then the reader closes it.
    fake_link(&f, &link, "PING\nLIST\nREAD 30 0 0\n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(f.closes == 1 && r.rc == 0 && strcmp(r.stage, "done") == 0,
          "after three requests: closes %d rc %08x stage %s", f.closes, (unsigned)r.rc, r.stage);
    CHECK(strncmp(f.out, "PONG\n", 5) == 0, "no PONG first");
    f.out[f.out_len] = '\0';
    CHECK(strstr(f.out, "\nPART 30 1 196613 00000000 User\n") != NULL, "LIST has no row for id 30");
    CHECK(strstr(f.out, "\nPART 28 0 0 00000e02 CalibrationFile\n") != NULL,
          "LIST has no row for the partition that cannot be opened");
    CHECK(strstr(f.out, "\nPART 0 0 0 002ee202 BootPartition1Root\n") != NULL,
          "LIST has no row for an id that is not there");
    CHECK(strstr(f.out, "\nEND\n") != NULL, "LIST does not end with END");
    {
        const char* ok = strstr(f.out, "\nOK 0 196613 196613\n");
        CHECK(ok != NULL, "no OK line for the READ");
        if (ok) { // move the READ's answer to the front, for check_read_answer
            const size_t at = (size_t)(ok + 1 - f.out);
            memmove(f.out, f.out + at, f.out_len - at);
            f.out_len -= at;
            check_read_answer(&f, "OK 0 196613 196613\n", img, 0, SIZE);
        }
    }
    CHECK(strcmp(r.request, "READ 30 0 0") == 0 && r.start == 0 && r.length == SIZE && r.sent == SIZE,
          "last request %s: start %lld length %lld sent %lld", r.request, (long long)r.start,
          (long long)r.length, (long long)r.sent);
    sha256sum_of(img, hex);
    nbe_fmt_hex32(r.sha256, want);
    CHECK(strcmp(hex, want) == 0, "the result's hash is not the partition's");

    // HASH: the same answer, without the bytes.
    fake_link(&f, &link, "HASH 30 1000 2000\n");
    bis_serve(&link, &t, &nocb, &r);
    sha256sum_of(image_slice(img, 1000, 2000), hex);
    snprintf(want, sizeof want, "OK 1000 2000 196613\nSHA256 %s\n", hex);
    f.out[f.out_len] = '\0';
    CHECK(strcmp(f.out, want) == 0, "HASH answered %.60s", f.out);
    CHECK(r.rc == 0 && r.sent == 0 && !r.cancelled, "HASH: rc %08x sent %lld", (unsigned)r.rc,
          (long long)r.sent);

    // A range in the middle.
    fake_link(&f, &link, "READ 30 1000 2000\n");
    bis_serve(&link, &t, &nocb, &r);
    check_read_answer(&f, "OK 1000 2000 196613\n", img, 1000, 2000);
    CHECK(r.rc == 0 && r.start == 1000 && r.length == 2000 && r.sent == 2000,
          "range: start %lld length %lld sent %lld", (long long)r.start, (long long)r.length,
          (long long)r.sent);

    // A length past the end of the partition is cut back to the end.
    fake_link(&f, &link, "READ 30 196600 999999\n");
    bis_serve(&link, &t, &nocb, &r);
    check_read_answer(&f, "OK 196600 13 196613\n", img, 196600, 13);

    // Refused requests: one ERR line each, and the connection stays usable.
    fake_link(&f, &link,
              "READ 28 0 0\nREAD 99 0 0\nREAD 30 196613 0\nREAD 30 abc\nWHAT\nREAD 30 0 512\n");
    bis_serve(&link, &t, &nocb, &r);
    snprintf(want, sizeof want,
             "ERR 00000e02 open\nERR 002ee202 open\nERR %08x open\nERR %08x parse\nERR %08x parse\n",
             (unsigned)R_VALUE(NBE_RC_NET_RANGE), (unsigned)R_VALUE(NBE_RC_NET_REQUEST),
             (unsigned)R_VALUE(NBE_RC_NET_REQUEST));
    f.out[f.out_len] = '\0';
    CHECK(strncmp(f.out, want, strlen(want)) == 0, "the refusals read %.70s", f.out);
    {
        FakeLink one;
        NbeLink l1;
        fake_link(&one, &l1, "READ 30 0 512\n");
        bis_serve(&l1, &t, &nocb, &r);
        f.out[f.out_len] = '\0';
        CHECK(f.out_len == strlen(want) + one.out_len &&
                  memcmp(f.out + strlen(want), one.out, one.out_len) == 0,
              "after the refusals the READ's answer differs from the same READ alone");
    }
    CHECK(r.rc == 0 && !r.refused && r.sent == 512 && strcmp(r.request, "READ 30 0 512") == 0,
          "the request after the refusals: rc %08x sent %lld", (unsigned)r.rc, (long long)r.sent);

    // net_pull.py sends one request per connection. When the reader closes the connection right
    // after a refusal, the result still has to say so: "answered" would read as success.
    fake_link(&f, &link, "READ 28 0 0\n");
    bis_serve(&link, &t, &nocb, &r);
    f.out[f.out_len] = '\0';
    CHECK(strcmp(f.out, "ERR 00000e02 open\n") == 0, "the refusal reads %.60s", f.out);
    CHECK(f.closes == 1 && r.refused && r.rc == 0xE02 && strcmp(r.stage, "open") == 0 &&
              r.sent == 0 && strcmp(r.request, "READ 28 0 0") == 0,
          "a refusal, then the reader closes: refused %d rc %08x stage %s sent %lld", r.refused,
          (unsigned)r.rc, r.stage, (long long)r.sent);
    fake_link(&f, &link, "WHAT\n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(f.closes == 1 && r.refused && r.rc == NBE_RC_NET_REQUEST && strcmp(r.stage, "parse") == 0,
          "a request that does not parse, then the reader closes: refused %d rc %08x stage %s",
          r.refused, (unsigned)r.rc, r.stage);
    fake_link(&f, &link, "PING\n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(f.closes == 1 && !r.refused && r.rc == 0 && strcmp(r.stage, "done") == 0,
          "an answered request, then the reader closes: refused %d rc %08x stage %s", r.refused,
          (unsigned)r.rc, r.stage);

    // Only decimal numbers are numbers: "0x1000" is not 0, and does not mean "to the end".
    fake_link(&f, &link,
              "READ 30 0 0x1000\nREAD 30 0 100x\nREAD 30 0 -5\nREAD 30 -1 0\nREAD 30 0 1 2\nREAD\n"
              "READ \n");
    bis_serve(&link, &t, &nocb, &r);
    snprintf(want, sizeof want, "ERR %08x parse\n", (unsigned)R_VALUE(NBE_RC_NET_REQUEST));
    char seven[8 * 32];
    seven[0] = '\0';
    for (int i = 0; i < 7; i++) strcat(seven, want);
    f.out[f.out_len] = '\0';
    CHECK(f.out_len == strlen(seven) && strncmp(f.out, seven, strlen(seven)) == 0,
          "seven requests that do not parse were answered with %zu bytes, starting %.60s",
          f.out_len, f.out);
    CHECK(r.refused && r.rc == NBE_RC_NET_REQUEST, "the last of them: refused %d rc %08x",
          r.refused, (unsigned)r.rc);

    // What is accepted: blanks may be tabs, the length may be left out, and a length past the
    // end of the partition (or past what a 64-bit number can hold) is cut back to the end.
    fake_link(&f, &link, "READ 30\t1000 50 \n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(!r.refused && r.rc == 0 && r.sent == 50, "tabs and a trailing blank: refused %d rc %08x sent %lld",
          r.refused, (unsigned)r.rc, (long long)r.sent);
    check_read_answer(&f, "OK 1000 50 196613\n", img, 1000, 50);
    fake_link(&f, &link, "READ 30 1000\n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(!r.refused && r.rc == 0 && r.sent == SIZE - 1000, "no length: refused %d rc %08x sent %lld",
          r.refused, (unsigned)r.rc, (long long)r.sent);
    check_read_answer(&f, "OK 1000 195613 196613\n", img, 1000, SIZE - 1000);
    fake_link(&f, &link, "READ 30 0 99999999999999999999999999\n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(!r.refused && r.rc == 0 && r.sent == SIZE, "an endless length: refused %d rc %08x sent %lld",
          r.refused, (unsigned)r.rc, (long long)r.sent);
    check_read_answer(&f, "OK 0 196613 196613\n", img, 0, SIZE);

    // A reader that closes at once.
    fake_link(&f, &link, "");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(f.closes == 1 && r.rc == 0 && r.request[0] == '\0' && strcmp(r.stage, "done") == 0,
          "a reader that closes at once: rc %08x stage %s", (unsigned)r.rc, r.stage);

    // The progress callback sees the request and the range.
    ctx.calls = 0;
    memset(&g_last_pg, 0, sizeof g_last_pg);
    fake_link(&f, &link, "READ 30 0 0\n");
    bis_serve(&link, &t, &cb, &r);
    const int chunks = (int)(SIZE / C) + (SIZE % C != 0); // 3 * C + 5: three whole chunks, and 5 bytes
    CHECK(ctx.calls == chunks, "progress was called %d times, once per chunk would be %d", ctx.calls,
          chunks);
    CHECK(g_last_pg.network && g_last_pg.note && strcmp(g_last_pg.note, "READ 30 0 0") == 0,
          "progress does not say it is a network transfer with its request");
    CHECK(g_last_pg.part && g_last_pg.part->id == 30 && strcmp(g_last_pg.part->name, "User") == 0 &&
              g_last_pg.total == SIZE && g_last_pg.done == SIZE && g_last_pg.parts == 1 &&
              g_last_pg.part_index == 0,
          "progress: part %s total %lld done %lld", g_last_pg.part ? g_last_pg.part->name : "(none)",
          (long long)g_last_pg.total, (long long)g_last_pg.done);
    check_read_answer(&f, "OK 0 196613 196613\n", img, 0, SIZE);

    // An id that is not in the table can be read all the same.
    world();
    bis_scan(&t); // before the partition exists for this test
    write_image("n77.bin", 4096, 121, NULL, 0);
    path_in(img, sizeof img, g_img, "/n77.bin");
    shim_storage_set(77, img);
    memset(&g_last_pg, 0, sizeof g_last_pg);
    ctx.calls = 0;
    fake_link(&f, &link, "READ 77 0 0\n");
    bis_serve(&link, &t, &cb, &r);
    CHECK(r.rc == 0 && r.sent == 4096 && ctx.calls == 1, "id 77: rc %08x sent %lld",
          (unsigned)r.rc, (long long)r.sent);
    CHECK(g_last_pg.part && strcmp(g_last_pg.part->name, "Id_77") == 0 && g_last_pg.part->id == 77,
          "an id outside the table is shown as %s",
          g_last_pg.part ? g_last_pg.part->name : "(none)");
    check_read_answer(&f, "OK 0 4096 4096\n", img, 0, 4096);

    // Stopped by the progress callback: the bytes so far, no hash, and no more requests.
    world();
    write_image("n30.bin", SIZE, 120, NULL, 0);
    path_in(img, sizeof img, g_img, "/n30.bin");
    shim_storage_set(30, img);
    bis_scan(&t);
    ctx.calls = 0;
    ctx.cancel_at = 1;
    fake_link(&f, &link, "READ 30 0 0\nREAD 30 0 512\n");
    bis_serve(&link, &t, &cb, &r);
    ctx.cancel_at = -1;
    f.out[f.out_len] = '\0';
    CHECK(r.cancelled && r.rc == 0 && r.sent == C && f.closes == 0, "stopped: cancelled %d rc %08x sent %lld",
          r.cancelled, (unsigned)r.rc, (long long)r.sent);
    CHECK(f.out_len == strlen("OK 0 196613 196613\n") + (size_t)C && strstr(f.out, "SHA256") == NULL,
          "a stopped READ left %zu bytes", f.out_len);

    // A send failure: the reader has the header and nothing of the chunk that failed.
    fake_link(&f, &link, "READ 30 0 0\nREAD 30 0 512\n");
    f.fail_after = strlen("OK 0 196613 196613\n") + 100;
    bis_serve(&link, &t, &nocb, &r);
    CHECK(R_FAILED(r.rc) && strcmp(r.stage, "send") == 0 && r.sent == 0 && f.closes == 0,
          "send failure: rc %08x stage %s sent %lld", (unsigned)r.rc, r.stage, (long long)r.sent);
    CHECK(f.out_len == strlen("OK 0 196613 196613\n"), "%zu bytes went out before the failure",
          f.out_len);

    // A read failure after one chunk: that chunk went out, and nothing claims to be a hash.
    shim_fail_read_at(C, RC_READ_FAIL, 1);
    fake_link(&f, &link, "READ 30 0 0\n");
    bis_serve(&link, &t, &nocb, &r);
    CHECK(r.rc == RC_READ_FAIL && strcmp(r.stage, "read") == 0 && r.sent == C,
          "read failure: rc %08x stage %s sent %lld", (unsigned)r.rc, r.stage, (long long)r.sent);
    f.out[f.out_len] = '\0';
    CHECK(f.out_len == strlen("OK 0 196613 196613\n") + (size_t)C && strstr(f.out, "SHA256") == NULL,
          "a failed READ left %zu bytes", f.out_len);
    CHECK(nbe_result_hint(NBE_RC_NET) != NULL && nbe_result_hint(NBE_RC_NET_REQUEST) != NULL &&
              nbe_result_hint(NBE_RC_NET_RANGE) != NULL,
          "the network result codes have no names");
}

int main(void) {
    struct { const char* name; void (*fn)(void); } tests[] = {
        { "plan", test_plan },
        { "single file", test_single_file },
        { "multi part", test_multi_part },
        { "boundaries", test_boundaries },
        { "split off", test_split_off },
        { "cancel", test_cancel },
        { "read failure", test_read_failure },
        { "read failure p0", test_read_failure_in_first_part },
        { "write failure", test_write_failure },
        { "no space", test_no_space },
        { "refusal keeps old", test_previous_dump_survives_a_refusal },
        { "overwrite", test_overwrite },
        { "size mismatch", test_size_mismatch },
        { "scan", test_scan },
        { "scan negative", test_scan_negative_size },
        { "scan truncated", test_scan_truncated },
        { "hidden on switch2", test_hidden_on_switch2 },
        { "bounce", test_bounce },
        { "plaintext", test_plaintext },
        { "commit failure", test_commit_failure_is_not_fatal },
        { "refusals", test_refusals },
        { "formatting", test_fmt },

        { "partial", test_partial },
        { "serve", test_serve },
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
