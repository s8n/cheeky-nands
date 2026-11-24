#include "bis.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "nbe_fmt.h"

// ---------------------------------------------------------------------------------------------
// Static partition table
//
// Names are libnx's FsBisPartitionId identifiers (libounce, the Switch 2 SDK, uses the same
// enum and adds id 39). "alias" is the familiar Switch 1 GPT label where one is in common use;
// Switch 2 may lay its storage out differently, which is why the table is only a starting
// point: bis_scan() asks the system which ids actually open.
// ---------------------------------------------------------------------------------------------

typedef struct {
    u32 id;
    const char* name;
    const char* alias;
} BisKnown;

static const BisKnown g_known[] = {
    {  0, "BootPartition1Root",              "BOOT0" },
    { 10, "BootPartition2Root",              "BOOT1" },
    { 20, "UserDataRoot",                    NULL },
    { 21, "BootConfigAndPackage2Part1",      "BCPKG2-1-Normal-Main" },
    { 22, "BootConfigAndPackage2Part2",      "BCPKG2-2-Normal-Sub" },
    { 23, "BootConfigAndPackage2Part3",      "BCPKG2-3-SafeMode-Main" },
    { 24, "BootConfigAndPackage2Part4",      "BCPKG2-4-SafeMode-Sub" },
    { 25, "BootConfigAndPackage2Part5",      "BCPKG2-5-Repair-Main" },
    { 26, "BootConfigAndPackage2Part6",      "BCPKG2-6-Repair-Sub" },
    { 27, "CalibrationBinary",               "PRODINFO" },
    { 28, "CalibrationFile",                 "PRODINFOF" },
    { 29, "SafeMode",                        "SAFE" },
    { 30, "User",                            "USER" },
    { 31, "System",                          "SYSTEM" },
    { 32, "SystemProperEncryption",          NULL },
    { 33, "SystemProperPartition",           NULL },
    { 34, "SignedSystemPartitionOnSafeMode", NULL },
    { 35, "DeviceTreeBlob",                  NULL },
    { 36, "System0",                         NULL },
    { 39, "Switch2Unidentified39",           NULL },
};

static const BisKnown* find_known(u32 id) {
    for (size_t i = 0; i < sizeof g_known / sizeof g_known[0]; i++)
        if (g_known[i].id == id) return &g_known[i];
    return NULL;
}

// libnx sends FS_MAX_PATH bytes from every path pointer it is handed, so each path that
// reaches an fsFs* call lives in a buffer of that size.
// Each buffer is FS_MAX_PATH bytes; a path that does not fit becomes an empty string.
static void name_paths(char* base, char* sha, char* info, const char* out_dir, const char* name) {
    const int b = snprintf(base, FS_MAX_PATH, "%s/%s.bin", out_dir, name);
    const int s = snprintf(sha, FS_MAX_PATH, "%s/%s.sha256", out_dir, name);
    const int i = snprintf(info, FS_MAX_PATH, "%s/%s.info", out_dir, name);
    if (b <= 0 || b >= FS_MAX_PATH) base[0] = '\0';
    if (s <= 0 || s >= FS_MAX_PATH) sha[0] = '\0';
    if (i <= 0 || i >= FS_MAX_PATH) info[0] = '\0';
}

// "<base>.NN" for a split part (0 <= index <= 99); empty if it does not fit.
static void with_part_suffix(char* out, const char* base, int index) {
    const size_t n = strlen(base);
    if (n == 0 || n + 4 >= FS_MAX_PATH || index < 0 || index > 99) {
        out[0] = '\0';
        return;
    }
    memcpy(out, base, n);
    out[n] = '.';
    out[n + 1] = (char)('0' + index / 10);
    out[n + 2] = (char)('0' + index % 10);
    out[n + 3] = '\0';
}

bool bis_file_exists(FsFileSystem* sd, const char* path) {
    FsDirEntryType type = FsDirEntryType_Dir;
    return R_SUCCEEDED(fsFsGetEntryType(sd, path, &type));
}

// What a walk over a dump's output files found.
typedef struct {
    bool remove;   // in: delete each file that exists
    bool measure;  // in: add each existing file's length to total
    bool any;      // out: at least one of the files exists
    s64 total;     // out: summed length of the existing files
    Result error;  // out: first failing deletion or measurement, or 0
} OutputWalk;

// Visit every file this tool writes for one dump name: the un-split file, the .sha256 and the
// .info, then parts 00..99. The metadata files come first on purpose: if a deletion fails, the
// part files that hold the data are the ones still there.
static void walk_outputs(FsFileSystem* sd, const char* base, const char* sha, const char* info,
                         OutputWalk* w) {
    char path[FS_MAX_PATH];
    const char* fixed[3] = { base, sha, info };

    for (int i = 0; i < 3 + BIS_MAX_PART_FILES; i++) {
        if (i < 3) snprintf(path, sizeof path, "%.*s", (int)sizeof path - 1, fixed[i]);
        else with_part_suffix(path, base, i - 3);
        if (!path[0]) continue;
        if (!bis_file_exists(sd, path)) continue;

        w->any = true;
        if (w->remove) {
            const Result rc = fsFsDeleteFile(sd, path);
            if (R_FAILED(rc) && !w->error) w->error = rc;
        }
        if (w->measure) {
            FsFile f;
            s64 len = 0;
            Result rc = fsFsOpenFile(sd, path, FsOpenMode_Read, &f);
            if (R_SUCCEEDED(rc)) {
                rc = fsFileGetSize(&f, &len);
                fsFileClose(&f);
            }
            if (R_SUCCEEDED(rc) && len < 0) rc = NBE_RC_BAD_SIZE;
            if (R_FAILED(rc)) {
                if (!w->error) w->error = rc;
            } else {
                w->total += len;
            }
        }
    }
}

static OutputWalk run_walk(FsFileSystem* sd, const char* out_dir, const char* name, bool remove,
                           bool measure) {
    char base[FS_MAX_PATH], sha[FS_MAX_PATH], info[FS_MAX_PATH];
    OutputWalk w = { remove, measure, false, 0, 0 };
    name_paths(base, sha, info, out_dir, name);
    walk_outputs(sd, base, sha, info, &w);
    return w;
}

bool bis_outputs_exist(FsFileSystem* sd, const char* out_dir, const char* name) {
    return run_walk(sd, out_dir, name, false, false).any;
}

s64 bis_outputs_size(FsFileSystem* sd, const char* out_dir, const char* name) {
    const OutputWalk w = run_walk(sd, out_dir, name, false, true);
    return R_FAILED(w.error) ? -1 : w.total;
}

Result bis_delete_outputs(FsFileSystem* sd, const char* out_dir, const char* name) {
    return run_walk(sd, out_dir, name, true, false).error;
}

// ---------------------------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------------------------

void bis_scan(BisTable* t) {
    memset(t, 0, sizeof *t);

    for (u32 id = 0; id <= BIS_SCAN_MAX_ID; id++) {
        const BisKnown* k = find_known(id);
        FsStorage st;
        s64 size = -1;

        Result rc = fsOpenBisStorage(&st, (FsBisPartitionId)id);
        if (R_SUCCEEDED(rc)) {
            rc = fsStorageGetSize(&st, &size);
            fsStorageClose(&st);
            if (R_SUCCEEDED(rc) && size < 0) rc = NBE_RC_BAD_SIZE;
        }

        const bool ok = R_SUCCEEDED(rc);
        if (!ok && !k) continue; // nothing here and nothing named: not worth a row
        if (t->count >= BIS_MAX_PARTITIONS) { t->truncated = true; break; }

        BisPartition* p = &t->parts[t->count++];
        p->id = id;
        p->known = k != NULL;
        p->openable = ok;
        p->open_rc = rc;
        p->size = ok ? size : 0;
        if (k) {
            snprintf(p->name, sizeof p->name, "%s", k->name);
            p->alias = k->alias;
        } else {
            snprintf(p->name, sizeof p->name, "Unknown_%u", id);
            p->alias = NULL;
        }
        if (ok) {
            t->openable_count++;
            t->openable_total += size;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Output plan
// ---------------------------------------------------------------------------------------------

int bis_part_count(s64 size, bool split) {
    if (!split || size <= (s64)BIS_PART_SIZE) return 1;
    const u64 s = (u64)size;
    return (int)(s / BIS_PART_SIZE + (s % BIS_PART_SIZE != 0));
}

s64 bis_part_length(s64 size, int index, bool split) {
    if (!split || size <= (s64)BIS_PART_SIZE) return index == 0 ? size : 0;
    const s64 base = (s64)index * (s64)BIS_PART_SIZE;
    if (base >= size) return 0;
    const s64 rest = size - base;
    return rest > (s64)BIS_PART_SIZE ? (s64)BIS_PART_SIZE : rest;
}

void bis_plan(BisPlan* plan, const char* out_dir, const char* name, s64 size, bool split) {
    memset(plan, 0, sizeof *plan);
    plan->size = size;
    plan->multi = split && size > (s64)BIS_PART_SIZE;
    plan->parts = bis_part_count(size, split);
    name_paths(plan->base, plan->sha, plan->info, out_dir, name);
}

void bis_part_path(const BisPlan* plan, int index, char* out) {
    if (plan->multi) with_part_suffix(out, plan->base, index);
    else snprintf(out, FS_MAX_PATH, "%.*s", FS_MAX_PATH - 1, plan->base);
}

// ---------------------------------------------------------------------------------------------
// Low-level I/O
// ---------------------------------------------------------------------------------------------

// Result the FS sysmodule returns when it cannot map the caller's buffer for IPC. libnx's stdio
// layer works around it with a stack buffer (fsdev_write_safe/fsdev_read_safe in fs_dev.c);
// the same fallback is used here so the static chunk buffer is not load-bearing.
#define RC_BUFFER_UNUSABLE 0xD401

static u8 g_chunk[BIS_CHUNK_SIZE] __attribute__((aligned(0x1000)));
static bool g_used_bounce; // set when the fallback below had to be used at least once

static Result io_read(FsStorage* st, s64 off, u8* buf, size_t n) {
    Result rc = fsStorageRead(st, off, buf, n);
    if (R_VALUE(rc) != RC_BUFFER_UNUSABLE) return rc;

    g_used_bounce = true;
    u8 tmp[0x4000];
    for (size_t d = 0; d < n; d += sizeof tmp) {
        const size_t m = n - d < sizeof tmp ? n - d : sizeof tmp;
        rc = fsStorageRead(st, off + (s64)d, tmp, m);
        if (R_FAILED(rc)) return rc;
        memcpy(buf + d, tmp, m);
    }
    return 0;
}

static Result io_write(FsFile* f, s64 off, const u8* buf, size_t n) {
    Result rc = fsFileWrite(f, off, buf, n, FsWriteOption_None);
    if (R_VALUE(rc) != RC_BUFFER_UNUSABLE) return rc;

    g_used_bounce = true;
    u8 tmp[0x4000];
    for (size_t d = 0; d < n; d += sizeof tmp) {
        const size_t m = n - d < sizeof tmp ? n - d : sizeof tmp;
        memcpy(tmp, buf + d, m);
        rc = fsFileWrite(f, off + (s64)d, tmp, m, FsWriteOption_None);
        if (R_FAILED(rc)) return rc;
    }
    return 0;
}

// Does the first chunk look like plain data? The encrypted BIS partitions are mounted by FS on
// top of its own decryption layer, so an OpenBisStorage read of them is expected to yield
// plaintext; this records whether it did, because dumps taken through FS then differ from raw
// (hekate-style) dumps that are decrypted offline.
static int looks_plaintext(u32 id, const u8* b, size_t n) {
    if (n < 0x400) return -1;
    switch (id) {
    case 20: return memcmp(b + 0x200, "EFI PART", 8) == 0; // UserDataRoot: GPT header, LBA 1
    case 27: return memcmp(b + 0x40, "CAL0", 4) == 0;      // CalibrationBinary body magic
    case 28: case 29: case 30: case 31:                    // FAT volumes: BPB / boot signature
        return b[0] == 0xEB || b[0] == 0xE9 || (b[0x1FE] == 0x55 && b[0x1FF] == 0xAA);
    default: return -1;
    }
}

// The file name part of a path, copied into a bounded buffer.
static void base_name(char* dst, size_t cap, const char* path) {
    const char* slash = strrchr(path, '/');
    snprintf(dst, cap, "%.*s", (int)cap - 1, slash ? slash + 1 : path);
}

static void setmsg(char* dst, size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
}

// Text file writer: creates `path` with length 0 and appends lines at the end of file.
//
// FsOpenMode_Append is required: without it the FS sysmodule rejects any write for which
// file_size < offset + size (Result 0x307202), which includes a write exactly at the end of
// an empty file. The bit permits growth; the position still comes from the write offset.
// (libnx's stdio layer passes it for every writable open; the dump's part files, which are
// pre-sized and only written inside their bounds, deliberately do not use it.)
typedef struct {
    FsFile f;
    s64 pos;
    Result rc;
} TextWriter;

static void tw_open(TextWriter* w, FsFileSystem* sd, const char* path) {
    memset(w, 0, sizeof *w);
    w->rc = fsFsCreateFile(sd, path, 0, 0);
    if (R_SUCCEEDED(w->rc))
        w->rc = fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append, &w->f);
}

static void tw_printf(TextWriter* w, const char* fmt, ...) {
    if (R_FAILED(w->rc)) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof line - 1) n = (int)sizeof line - 1; // vsnprintf counts the untruncated text
    w->rc = io_write(&w->f, w->pos, (const u8*)line, (size_t)n);
    w->pos += n;
}

static Result tw_close(TextWriter* w) {
    if (R_SUCCEEDED(w->rc)) w->rc = fsFileFlush(&w->f);
    fsFileClose(&w->f);
    return w->rc;
}

// ---------------------------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------------------------

u64 bis_now_ns(void) {
#ifdef NBE_HOST_TEST
    return shim_now_ns(); // the real clock, or a scripted one (test/ui_shim.c)
#else
    return armTicksToNs(armGetSystemTick());
#endif
}

BisDumpStatus bis_dump_range(const BisPartition* part, FsFileSystem* sd, const char* out_dir,
                             bool split, const BisRange* range, const BisCallbacks* cb,
                             BisDumpResult* r) {
    FsStorage st;
    FsFile f;
    BisPlan plan;
    Sha256Context whole;
    char path[FS_MAX_PATH];
    char name[64];
    char stem[80]; // the name of the files: the partition name, or that plus the range
    s64 rng_start = 0, rng_length = 0, source_size = 0;
    bool is_whole = true; // the dump covers the whole partition
    bool st_open = false, f_open = false, ok = false, replacing = false;
    s64 done = 0;
    const u64 t0 = bis_now_ns();

    memset(&st, 0, sizeof st);
    memset(&f, 0, sizeof f);
    memset(r, 0, sizeof *r);
    g_used_bounce = false;
    r->status = BisDump_Error;
    r->stage = "plan";
    r->fail_offset = -1;
    r->part_index = -1;
    r->plaintext = -1;

    {
        const s64 start = range ? range->start : 0, length = range ? range->length : 0;
        if (start < 0 || (start & 511) || length < 0 || (length & 511)) {
            setmsg(r->message, sizeof r->message,
                   "the start and the length of a partial dump must be multiples of 512");
            goto cleanup;
        }
        rng_start = start;
        rng_length = length;
        is_whole = start == 0 && length == 0;
    }
    snprintf(stem, sizeof stem, "%s", part->name);
    bis_plan(&plan, out_dir, stem, part->size, split);
    if (!plan.base[0] || !plan.sha[0] || !plan.info[0]) {
        setmsg(r->message, sizeof r->message, "output path for %s does not fit", part->name);
        goto cleanup;
    }
    if (is_whole && bis_part_count(part->size, split) > BIS_MAX_PART_FILES) {
        setmsg(r->message, sizeof r->message, "%s needs %d part files; this tool supports %d",
               part->name, bis_part_count(part->size, split), BIS_MAX_PART_FILES);
        goto cleanup;
    }

    r->stage = "open";
    r->rc = fsOpenBisStorage(&st, (FsBisPartitionId)part->id);
    if (R_FAILED(r->rc)) {
        setmsg(r->message, sizeof r->message, "OpenBisStorage(%u) failed", part->id);
        goto cleanup;
    }
    st_open = true;

    r->rc = fsStorageGetSize(&st, &source_size);
    if (R_SUCCEEDED(r->rc) && source_size < 0) r->rc = NBE_RC_BAD_SIZE;
    if (R_FAILED(r->rc)) {
        setmsg(r->message, sizeof r->message, "GetSize on partition %u failed", part->id);
        goto cleanup;
    }

    // The range, against the size the storage reports now (not the scan's).
    if (!is_whole && rng_start >= source_size) { // a whole dump of an empty partition is fine
        r->rc = 0;
        setmsg(r->message, sizeof r->message,
               "start 0x%llx is at or beyond the end of the partition (0x%llx bytes)",
               (unsigned long long)rng_start, (unsigned long long)source_size);
        goto cleanup;
    }
    if (rng_length == 0 || rng_length > source_size - rng_start)
        rng_length = source_size - rng_start;
    if (rng_start == 0 && rng_length == source_size) is_whole = true;
    if (is_whole)
        snprintf(stem, sizeof stem, "%s", part->name);
    else
        bis_partial_stem(stem, sizeof stem, part->name, rng_start, rng_start + rng_length);
    r->start = rng_start;
    r->source_size = source_size;
    snprintf(r->stem, sizeof r->stem, "%s", stem);

    // Plan from the length that is actually dumped.
    bis_plan(&plan, out_dir, stem, rng_length, split);
    if (plan.parts > BIS_MAX_PART_FILES) {
        r->rc = 0;
        setmsg(r->message, sizeof r->message, "%s needs %d part files; this tool supports %d",
               stem, plan.parts, BIS_MAX_PART_FILES);
        goto cleanup;
    }
    r->parts = plan.parts;
    r->multi = plan.multi;
    r->size = rng_length;

    // Everything that can be refused has been refused by now (path, part count, the storage
    // itself). Only now is the earlier dump of this name removed, so a refused dump never
    // costs a backup that already existed.
    r->stage = "replace";
    r->rc = bis_delete_outputs(sd, out_dir, stem);
    if (R_FAILED(r->rc)) {
        // Whatever could not be deleted is left alone; nothing new has been written yet.
        setmsg(r->message, sizeof r->message, "removing the previous %s dump failed", part->name);
        goto cleanup;
    }
    replacing = true; // from here on the new dump owns the name

    sha256ContextCreate(&whole);

    for (int i = 0; i < plan.parts; i++) {
        const s64 base = plan.multi ? (s64)i * (s64)BIS_PART_SIZE : 0;
        const s64 len = bis_part_length(plan.size, i, plan.multi);
        Sha256Context pc;

        bis_part_path(&plan, i, path);
        base_name(name, sizeof name, path);

        if (cb && cb->message) {
            char msg[128];
            char sz[24];
            nbe_fmt_size(sz, sizeof sz, len);
            snprintf(msg, sizeof msg, "allocating %s (%s)", name, sz);
            cb->message(cb->user, msg);
        }

        // Pre-size the part so a full SD card fails here, before any NAND is read.
        r->stage = "create";
        r->part_index = i;
        r->rc = fsFsCreateFile(sd, path, len, 0);
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "CreateFile(%s, %lld bytes) failed", name,
                   (long long)len);
            goto cleanup;
        }
        r->rc = fsFsOpenFile(sd, path, FsOpenMode_Write, &f);
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "OpenFile(%s) for writing failed", name);
            goto cleanup;
        }
        f_open = true;
        sha256ContextCreate(&pc);

        for (s64 off = 0; off < len; ) {
            const size_t n = len - off < (s64)BIS_CHUNK_SIZE ? (size_t)(len - off)
                                                             : (size_t)BIS_CHUNK_SIZE;

            r->stage = "read";
            r->fail_offset = base + off;
            r->rc = io_read(&st, rng_start + base + off, g_chunk, n);
            if (R_FAILED(r->rc)) {
                setmsg(r->message, sizeof r->message, "read at partition offset 0x%llx failed",
                       (unsigned long long)(rng_start + base + off));
                goto cleanup;
            }
            if (done == 0) r->plaintext = looks_plaintext(part->id, g_chunk, n);

            r->stage = "write";
            r->rc = io_write(&f, off, g_chunk, n);
            if (R_FAILED(r->rc)) {
                setmsg(r->message, sizeof r->message, "write to %s at 0x%llx failed", name,
                       (unsigned long long)off);
                goto cleanup;
            }

            sha256ContextUpdate(&pc, g_chunk, n);
            sha256ContextUpdate(&whole, g_chunk, n);
            off += (s64)n;
            done += (s64)n;

            if (cb && cb->progress) {
                BisProgress pg;
                memset(&pg, 0, sizeof pg);
                pg.part = part;
                pg.part_index = i;
                pg.parts = plan.parts;
                pg.path = path;
                pg.done = done;
                pg.total = plan.size;
                pg.elapsed_ns = bis_now_ns() - t0;
                r->stage = "progress";
                if (!cb->progress(cb->user, &pg)) {
                    r->status = BisDump_Cancelled;
                    r->rc = 0;
                    char sz[24];
                    nbe_fmt_size(sz, sizeof sz, done);
                    setmsg(r->message, sizeof r->message, "cancelled after %s", sz);
                    goto cleanup;
                }
            }
        }

        r->stage = "close";
        r->rc = fsFileFlush(&f);
        fsFileClose(&f);
        f_open = false;
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "Flush(%s) failed", name);
            goto cleanup;
        }
        sha256ContextGetHash(&pc, r->part_sha256[i]);
    }

    // The files must have exactly the lengths that were asked for. A filesystem that cannot
    // hold a file of the requested size must not pass silently as a successful dump.
    r->stage = "verify";
    r->part_index = -1;
    r->fail_offset = -1;
    for (int i = 0; i < plan.parts; i++) {
        const s64 want = bis_part_length(plan.size, i, plan.multi);
        s64 got = -1;
        bis_part_path(&plan, i, path);
        base_name(name, sizeof name, path);

        r->rc = fsFsOpenFile(sd, path, FsOpenMode_Read, &f);
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "OpenFile(%s) for verification failed", name);
            goto cleanup;
        }
        r->rc = fsFileGetSize(&f, &got);
        fsFileClose(&f);
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "GetSize(%s) for verification failed", name);
            goto cleanup;
        }
        if (got != want) {
            r->rc = NBE_RC_SIZE_MISMATCH;
            setmsg(r->message, sizeof r->message,
                   "%s holds %lld bytes, expected %lld: the SD card cannot store this file size",
                   name, (long long)got, (long long)want);
            goto cleanup;
        }
    }
    sha256ContextGetHash(&whole, r->sha256);

    r->stage = "sha256";
    {
        TextWriter w;
        char hex[65], line_name[FS_MAX_PATH];
        tw_open(&w, sd, plan.sha);
        tw_printf(&w, "# SHA-256 of each file, sha256sum -c format\n");
        nbe_fmt_hex32(r->sha256, hex);
        tw_printf(&w, "# whole partition (all files concatenated in order): %s\n", hex);
        for (int i = 0; i < plan.parts; i++) {
            bis_part_path(&plan, i, path);
            base_name(line_name, sizeof line_name, path);
            nbe_fmt_hex32(r->part_sha256[i], hex);
            tw_printf(&w, "%s  %s\n", hex, line_name);
        }
        r->rc = tw_close(&w);
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "writing %s.sha256 failed", part->name);
            goto cleanup;
        }
    }

    r->stage = "info";
    {
        TextWriter w;
        char hex[65];
        tw_open(&w, sd, plan.info);
        tw_printf(&w, "# written by %s %s (%s)\n", NBE_APP_NAME, NBE_APP_VERSION, nbe_platform());
        tw_printf(&w, "timestamp_unix=%lld\n", (long long)time(NULL));
        tw_printf(&w, "partition_id=%u\n", part->id);
        tw_printf(&w, "partition_name=%s\n", part->name);
        tw_printf(&w, "size=%lld\n", (long long)plan.size);
        if (!is_whole) {
            tw_printf(&w, "start=%lld\n", (long long)rng_start);
            tw_printf(&w, "source_size=%lld\n", (long long)source_size);
        }
        tw_printf(&w, "parts=%d\n", plan.parts);
        tw_printf(&w, "part_size_max=%llu\n", (unsigned long long)BIS_PART_SIZE);
        nbe_fmt_hex32(r->sha256, hex);
        tw_printf(&w, "sha256=%s\n", hex);
        tw_printf(&w, "looks_plaintext=%d\n", r->plaintext);
        r->rc = tw_close(&w);
        if (R_FAILED(r->rc)) {
            setmsg(r->message, sizeof r->message, "writing %s.info failed", part->name);
            goto cleanup;
        }
    }

    // A no-op on the SD card; harmless, and keeps the code honest if the output ever moves
    // to a filesystem that needs it. Not a reason to fail a dump whose files verified.
    r->stage = "commit";
    const Result commit_rc = fsFsCommit(sd);

    ok = true;
    r->status = BisDump_Ok;
    r->rc = 0;
    r->stage = "done";
    {
        char sz[24];
        nbe_fmt_size(sz, sizeof sz, done);
        setmsg(r->message, sizeof r->message, "dumped %s as %d file(s)", sz, plan.parts);
        if (R_FAILED(commit_rc)) {
            size_t n = strlen(r->message);
            snprintf(r->message + n, sizeof r->message - n, " (Commit: %08x)", (unsigned)commit_rc);
        }
    }

cleanup:
    if (f_open) fsFileClose(&f);
    if (st_open) fsStorageClose(&st);
    r->used_bounce = g_used_bounce;
    if (!ok && replacing) {
        Result rc = bis_delete_outputs(sd, out_dir, stem);
        if (R_FAILED(rc)) {
            size_t n = strlen(r->message);
            snprintf(r->message + n, sizeof r->message - n, " (and deleting the leftovers failed: %08x)",
                     (unsigned)rc);
        }
    }
    r->elapsed_ns = bis_now_ns() - t0;
    return r->status;
}

BisDumpStatus bis_dump(const BisPartition* part, FsFileSystem* sd, const char* out_dir,
                       bool split, const BisCallbacks* cb, BisDumpResult* r) {
    const BisRange all = { 0, 0 };
    return bis_dump_range(part, sd, out_dir, split, &all, cb, r);
}

void bis_partial_stem(char* out, size_t cap, const char* name, s64 start, s64 end) {
    snprintf(out, cap, "%s+0x%llx-0x%llx", name, (unsigned long long)start,
             (unsigned long long)end);
}

// ---------------------------------------------------------------------------------------------
// Partition table (GPT) at the start of a partition
// ---------------------------------------------------------------------------------------------

static u8 g_gpt_buf[0x10000] __attribute__((aligned(0x1000))); // the first 64 KiB, or the entries

static u64 rd_le(const u8* p, int n) {
    u64 v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void bis_read_gpt(u32 id, BisGpt* g) {
    FsStorage st;
    s64 size = 0, want = 0;
    const u8* h = NULL;
    const u8* e;
    s64 at, bytes;
    u64 entry_lba;
    u32 count, esz;
    Result rc;

    memset(g, 0, sizeof *g);
    rc = fsOpenBisStorage(&st, (FsBisPartitionId)id);
    if (R_FAILED(rc)) {
        g->rc = rc;
        return;
    }
    rc = fsStorageGetSize(&st, &size);
    if (R_SUCCEEDED(rc) && size < 0x200) rc = NBE_RC_BAD_SIZE;
    if (R_SUCCEEDED(rc)) {
        want = size < (s64)sizeof g_gpt_buf ? size & ~(s64)0x1FF : (s64)sizeof g_gpt_buf;
        rc = io_read(&st, 0, g_gpt_buf, (size_t)want);
    }
    if (R_FAILED(rc)) {
        fsStorageClose(&st);
        g->rc = rc;
        return;
    }

    // The header sits in block 1: 512 bytes in, or 4096 on storage with 4 KiB blocks.
    for (s64 off = 0x200; off + 0x5C <= want; off += 0x200) {
        if (memcmp(g_gpt_buf + off, "EFI PART", 8) == 0 &&
            rd_le(g_gpt_buf + off + 0x0C, 4) == 0x5C && rd_le(g_gpt_buf + off + 0x18, 8) == 1) {
            g->sector_size = (u64)off;
            h = g_gpt_buf + off;
            break;
        }
    }
    if (!h) { // no table here; not an error
        fsStorageClose(&st);
        return;
    }
    g->found = true;

    entry_lba = rd_le(h + 0x48, 8);
    count = (u32)rd_le(h + 0x50, 4);
    esz = (u32)rd_le(h + 0x54, 4);
    if (count == 0 || count > BIS_GPT_MAX_ENTRIES || esz < 0x80 ||
        (esz & (esz - 1)) != 0) {
        fsStorageClose(&st); // a header whose entry list makes no sense: report the header only
        return;
    }
    at = (s64)entry_lba * (s64)g->sector_size;
    bytes = (s64)count * (s64)esz;
    if (bytes > (s64)sizeof g_gpt_buf - 0x200) {
        fsStorageClose(&st);
        return;
    }
    if (at >= 0 && at + bytes <= want) {
        e = g_gpt_buf + at; // the entries were in what was read
    } else {
        const s64 n = (bytes + 0x1FF) & ~(s64)0x1FF; // whole blocks
        if (at < 0 || at + n > size) {
            fsStorageClose(&st);
            return;
        }
        rc = io_read(&st, at, g_gpt_buf, (size_t)n);
        if (R_FAILED(rc)) {
            fsStorageClose(&st);
            g->rc = rc;
            g->entries = 0;
        }
        e = g_gpt_buf;
    }

    for (u32 i = 0; R_SUCCEEDED(g->rc) && i < count; i++) {
        const u8* p = e + (size_t)i * esz;
        BisGptEntry* x = &g->table[i];
        bool used = false;
        size_t n = 0;
        g->entries = i + 1;
        for (int k = 0; k < 16; k++) used = used || p[k] != 0; // the type GUID
        if (!used) continue;
        x->first_lba = rd_le(p + 0x20, 8);
        x->last_lba = rd_le(p + 0x28, 8);
        for (; n < 36; n++) { // the name is UTF-16LE, 36 characters at most
            const u32 c = (u32)rd_le(p + 0x38 + 2 * n, 2);
            if (c == 0) break;
            x->name[n] = (c < 0x20 || c > 0x7E) ? '?' : (char)c;
        }
        x->name[n] = '\0';
    }
    fsStorageClose(&st);
}

static bool ci_equal(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return false;
    }
    return *a == *b;
}

s64 bis_gpt_start_of(const BisGpt* g, const char* name) {
    for (u32 i = 0; i < g->entries; i++)
        if (g->table[i].name[0] && ci_equal(g->table[i].name, name))
            return (s64)(g->table[i].first_lba * g->sector_size);
    return -1;
}

s64 bis_gpt_size_of(const BisGpt* g, int i) {
    if (i < 0 || (u32)i >= g->entries || !g->table[i].name[0]) return 0;
    const BisGptEntry* e = &g->table[i];
    if (e->last_lba < e->first_lba) return 0;
    return (s64)((e->last_lba - e->first_lba + 1) * g->sector_size);
}

Result bis_write_gpt(FsFileSystem* sd, const char* out_dir, const char* name, const BisGpt* g) {
    char path[FS_MAX_PATH];
    TextWriter w;
    snprintf(path, sizeof path, "%s/%s.gpt.txt", out_dir, name);
    fsFsDeleteFile(sd, path); // an earlier copy: fine if there is none
    tw_open(&w, sd, path);
    tw_printf(&w, "# GUID partition table read from the start of %s\n", name);
    tw_printf(&w, "# block_size=%llu entries=%llu\n", (unsigned long long)g->sector_size,
              (unsigned long long)g->entries);
    tw_printf(&w, "# name first_lba last_lba start_bytes size_bytes\n");
    for (u32 i = 0; i < g->entries; i++) {
        const BisGptEntry* e = &g->table[i];
        if (!e->name[0]) continue;
        tw_printf(&w, "%s %llu %llu %lld %lld\n", e->name, (unsigned long long)e->first_lba,
                  (unsigned long long)e->last_lba, (long long)(e->first_lba * g->sector_size),
                  (long long)bis_gpt_size_of(g, (int)i));
    }
    return tw_close(&w);
}

// ---------------------------------------------------------------------------------------------
// Network reader (see bis.h for the requests)
// ---------------------------------------------------------------------------------------------

static const BisPartition* bis_table_find(const BisTable* t, u32 id) {
    for (int i = 0; i < t->count; i++)
        if (t->parts[i].id == id) return &t->parts[i];
    return NULL;
}

static Result link_line(NbeLink* link, const char* text) {
    return link->send_all(link->user, text, strlen(text));
}

// "ERR <code> <stage>": the request was refused, and the connection stays usable.
static Result link_err(NbeLink* link, Result rc, const char* stage) {
    char line[96];
    snprintf(line, sizeof line, "ERR %08x %s\n", (unsigned)R_VALUE(rc), stage);
    return link_line(link, line);
}

static Result serve_list(NbeLink* link, const BisTable* t) {
    char line[160];
    for (int i = 0; i < t->count; i++) {
        const BisPartition* p = &t->parts[i];
        snprintf(line, sizeof line, "PART %u %d %lld %08x %s\n", p->id, p->openable ? 1 : 0,
                 (long long)p->size, (unsigned)R_VALUE(p->open_rc), p->name);
        const Result rc = link_line(link, line);
        if (R_FAILED(rc)) return rc;
    }
    return link_line(link, t->truncated ? "END truncated\n" : "END\n");
}

// READ and HASH: "OK <start> <length> <size>", the bytes (READ only), "SHA256 <hex>".
// Returns false when the reader is left with an incomplete READ: the connection has to be
// closed then, so that nothing can be taken for the rest of the data.

// A decimal number at *s: digits only, no sign. Past 2^64 - 1 it stays there ("endless").
static bool take_number(const char** s, unsigned long long* out) {
    if (**s < '0' || **s > '9') return false;
    unsigned long long v = 0;
    while (**s >= '0' && **s <= '9') {
        const unsigned d = (unsigned)(**s - '0');
        if (v > (~0ULL - d) / 10) v = ~0ULL;
        else v = v * 10 + d;
        (*s)++;
    }
    *out = v;
    return true;
}

static void take_spaces(const char** s) {
    while (**s == ' ' || **s == '\t') (*s)++;
}

static bool serve_range(NbeLink* link, const char* args, const BisTable* t, bool sending,
                        const BisCallbacks* cb, BisServeResult* r) {
    unsigned long long id = 0, start = 0, length = 0;
    char head[96], tail[80], hex[65];
    FsStorage st;
    Sha256Context sha;
    BisPartition anon;
    const BisPartition* shown;
    const u64 t0 = bis_now_ns();
    s64 size = 0, sent = 0;
    Result rc;

    r->start = r->length = r->sent = 0;
    r->refused = false;
    r->stage = "parse";
    // "<id> <start> [<length>]": decimal numbers, blanks between them, and nothing else. A
    // length that is not a number is refused, not read as 0 (which would mean "to the end").
    const char* p = args;
    take_spaces(&p);
    bool ok = take_number(&p, &id);
    take_spaces(&p);
    ok = ok && take_number(&p, &start);
    take_spaces(&p);
    if (ok && *p != '\0') {
        ok = take_number(&p, &length);
        take_spaces(&p);
    }
    if (!ok || *p != '\0' || id > 0xFFFFFFFFULL || start > 0x7FFFFFFFFFFFFFFFULL) {
        rc = NBE_RC_NET_REQUEST;
        (void)link_err(link, rc, "parse");
        r->rc = rc;
        r->refused = true;
        return true;
    }
    r->start = (s64)start;
    r->length = (s64)length;

    r->stage = "open";
    rc = fsOpenBisStorage(&st, (FsBisPartitionId)(u32)id);
    if (R_FAILED(rc)) goto refused;
    rc = fsStorageGetSize(&st, &size);
    if (R_SUCCEEDED(rc) && size < 0) rc = NBE_RC_BAD_SIZE;
    if (R_FAILED(rc)) {
        fsStorageClose(&st);
        goto refused;
    }
    if (r->start >= size) {
        rc = NBE_RC_NET_RANGE;
        fsStorageClose(&st);
        goto refused;
    }
    // 0 means "to the end"; so does a length past the end, or one that wrapped past 2^63.
    if (r->length <= 0 || r->length > size - r->start) r->length = size - r->start;

    snprintf(head, sizeof head, "OK %lld %lld %lld\n", (long long)r->start, (long long)r->length,
             (long long)size);
    r->stage = "send";
    rc = link_line(link, head);
    if (R_FAILED(rc)) goto dropped;

    shown = t ? bis_table_find(t, (u32)id) : NULL;
    if (!shown) { // not in the table (it was scanned before, or the id never answered)
        memset(&anon, 0, sizeof anon);
        anon.id = (u32)id;
        anon.size = size;
        snprintf(anon.name, sizeof anon.name, "Id_%u", anon.id);
        shown = &anon;
    }

    sha256ContextCreate(&sha);
    while (sent < r->length) {
        const s64 rest = r->length - sent;
        const size_t n = rest < (s64)BIS_CHUNK_SIZE ? (size_t)rest : (size_t)BIS_CHUNK_SIZE;

        r->stage = "read";
        rc = io_read(&st, r->start + sent, g_chunk, n);
        if (R_FAILED(rc)) goto dropped; // nothing of this chunk was sent
        sha256ContextUpdate(&sha, g_chunk, n);
        if (sending) {
            r->stage = "send";
            rc = link->send_all(link->user, g_chunk, n);
            if (R_FAILED(rc)) goto dropped;
            r->sent = sent + (s64)n;
        }
        sent += (s64)n;
        if (cb && cb->progress) {
            BisProgress pg;
            memset(&pg, 0, sizeof pg);
            pg.part = shown;
            pg.part_index = 0;
            pg.parts = 1;
            pg.path = "network";
            pg.done = sent;
            pg.total = r->length;
            pg.elapsed_ns = bis_now_ns() - t0;
            pg.network = true;
            pg.note = r->request;
            if (!cb->progress(cb->user, &pg)) {
                r->cancelled = true;
                rc = 0;
                goto dropped;
            }
        }
    }
    sha256ContextGetHash(&sha, r->sha256);
    nbe_fmt_hex32(r->sha256, hex);
    snprintf(tail, sizeof tail, "SHA256 %s\n", hex);
    r->stage = "send";
    rc = link_line(link, tail);
    if (R_FAILED(rc)) goto dropped;

    fsStorageClose(&st);
    r->rc = 0;
    r->stage = "done";
    r->elapsed_ns = bis_now_ns() - t0;
    return true;

dropped:
    fsStorageClose(&st);
    r->rc = rc;
    r->elapsed_ns = bis_now_ns() - t0;
    return false;

refused:
    (void)link_err(link, rc, r->stage);
    r->rc = rc;
    r->refused = true;
    r->elapsed_ns = bis_now_ns() - t0;
    return true;
}

void bis_serve(NbeLink* link, const BisTable* table, const BisCallbacks* cb, BisServeResult* r) {
    char line[256];

    memset(r, 0, sizeof *r);
    for (;;) {
        // At the top of every pass `r` tells of the last request in full: rc, stage, refused.
        const int got = link->recv_line(link->user, line, sizeof line);
        if (got < 0) { // the connection failed while a request was awaited
            r->rc = NBE_RC_NET;
            r->stage = "recv";
            return;
        }
        if (got == 0) { // the reader closed the connection: `r` stays as it is. When there was
                        // no request, nothing went wrong.
            if (r->request[0] == '\0') r->stage = "done";
            return;
        }
        snprintf(r->request, sizeof r->request, "%.95s", line);
        r->rc = 0;
        r->refused = false;
        r->stage = "done";
        if (strcmp(line, "PING") == 0) {
            r->rc = link_line(link, "PONG\n");
            if (R_FAILED(r->rc)) {
                r->stage = "send";
                return;
            }
        } else if (strcmp(line, "LIST") == 0) {
            r->rc = serve_list(link, table);
            if (R_FAILED(r->rc)) {
                r->stage = "send";
                return;
            }
        } else if (strncmp(line, "READ ", 5) == 0) {
            if (!serve_range(link, line + 5, table, true, cb, r)) return;
        } else if (strncmp(line, "HASH ", 5) == 0) {
            if (!serve_range(link, line + 5, table, false, cb, r)) return;
        } else {
            r->rc = NBE_RC_NET_REQUEST;
            r->stage = "parse";
            r->refused = true;
            if (R_FAILED(link_err(link, r->rc, "parse"))) {
                r->stage = "send";
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// On-console experiment: how does this SD card treat a file of `size` bytes?
// ---------------------------------------------------------------------------------------------

void bis_bigfile_test(FsFileSystem* sd, const char* out_dir, s64 size, BisBigFileResult* out) {
    char path[FS_MAX_PATH];
    memset(out, 0, sizeof *out);
    out->requested = size;
    out->stored_size = -1;

    snprintf(path, sizeof path, "%s/_bigfile_test.bin", out_dir);
    fsFsDeleteFile(sd, path); // leftover of an interrupted earlier test

    out->create_rc = fsFsCreateFile(sd, path, size, 0);
    if (R_SUCCEEDED(out->create_rc)) {
        FsFile f;
        out->reopen_rc = fsFsOpenFile(sd, path, FsOpenMode_Read, &f);
        if (R_SUCCEEDED(out->reopen_rc)) {
            out->reopen_rc = fsFileGetSize(&f, &out->stored_size);
            fsFileClose(&f);
        }
    }
    out->delete_rc = fsFsDeleteFile(sd, path);
    out->accepts = R_SUCCEEDED(out->create_rc) && R_SUCCEEDED(out->reopen_rc) &&
                   out->stored_size == size;
}
