// POSIX implementation of the host shim. See shim.h.
#define _POSIX_C_SOURCE 200809L
#include "shim.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Result codes, as documented on switchbrew "Error codes".
#define RC_PATH_NOT_FOUND  0x202    // ResultPathNotFound
#define RC_ALREADY_EXISTS  0x402    // ResultPathAlreadyExists
#define RC_NO_SPACE        0x4E02   // ResultUsableSpaceNotEnoughSdCard
#define RC_INVALID_OFFSET  0x2F5A02 // ResultInvalidOffset
#define RC_BEYOND_END      0x307202 // ResultFileExtensionWithoutOpenModeAllowAppend
#define RC_NO_WRITE        0x307602 // observed in the FS binary: write on a read-only file
#define RC_INVALID_ARG     0x2EE202 // ResultInvalidArgument (OpenBisStorage: unknown id)
#define RC_UNSUPPORTED     0x313A02 // UnsupportedCommitTarget
#define RC_BUFFER          0xD401   // buffer memory not mappable for FS IPC
#define RC_IO              MAKERESULT(2, 5000)

#define MAX_IDS 256

static FsFileSystem g_fs;
static char g_images[MAX_IDS][1024]; // copied: callers may reuse their buffer
static Result g_deny[MAX_IDS];
static s64 g_free;
static s64 g_fail_read_off, g_fail_write_off;
static Result g_fail_read_rc, g_fail_write_rc, g_commit_rc;
static int g_fail_read_nth, g_fail_write_nth; // <= 0: not armed
static s64 g_size_skew;
static Result g_getsize_rc;
static s64 g_getsize_reported;
static bool g_getsize_forced;
static size_t g_unusable_over;
static int g_open_calls;
static const char* g_fail_delete_suffix; // DeleteFile fails for paths ending here
static Result g_fail_delete_rc;
static int g_open_count[MAX_IDS]; // OpenBisStorage calls per id
static int g_deny_nth[MAX_IDS];    // <= 0: never
static Result g_deny_nth_rc[MAX_IDS];
static u32 g_reads[MAX_IDS];       // fsStorageRead calls per id

static void full(char* out, size_t cap, const char* root, const char* path) {
    snprintf(out, cap, "%s%s", root, path);
}

static s64 file_size(const char* p) {
    struct stat st;
    return stat(p, &st) == 0 ? (s64)st.st_size : -1;
}

static bool unusable(const void* buf, size_t size) {
    (void)buf;
    return g_unusable_over != SIZE_MAX && size >= g_unusable_over;
}

void shim_init(const char* sd_root) {
    g_fs.root = sd_root;
    memset(g_images, 0, sizeof g_images);
    memset(g_deny, 0, sizeof g_deny);
    g_free = -1;
    g_fail_read_nth = g_fail_write_nth = 0;
    g_commit_rc = 0;
    g_size_skew = 0;
    g_getsize_forced = false;
    g_unusable_over = SIZE_MAX;
    g_open_calls = 0;
    g_fail_delete_suffix = NULL;
    memset(g_open_count, 0, sizeof g_open_count);
    memset(g_reads, 0, sizeof g_reads);
}
void shim_storage_set(u32 id, const char* image_path) {
    if (id >= MAX_IDS) return;
    snprintf(g_images[id], sizeof g_images[id], "%s", image_path);
}
void shim_storage_deny(u32 id, Result rc) { g_deny[id] = rc; }
void shim_storage_getsize(Result rc, s64 reported) {
    g_getsize_forced = true;
    g_getsize_rc = rc;
    g_getsize_reported = reported;
}
void shim_free_space(s64 bytes) { g_free = bytes; }
void shim_fail_read_at(s64 offset, Result rc, int nth) { g_fail_read_off = offset; g_fail_read_rc = rc; g_fail_read_nth = nth; }
void shim_fail_write_at(s64 offset, Result rc, int nth) { g_fail_write_off = offset; g_fail_write_rc = rc; g_fail_write_nth = nth; }
void shim_fail_commit(Result rc) { g_commit_rc = rc; }
void shim_size_skew(s64 delta) { g_size_skew = delta; } // applies to files (fsFileGetSize) only
void shim_unusable_over(size_t bytes) { g_unusable_over = bytes; }
int  shim_open_bis_calls(void) { return g_open_calls; }
void shim_storage_deny_nth(u32 id, Result rc, int nth) {
    if (id >= MAX_IDS) return;
    g_deny_nth[id] = nth;
    g_deny_nth_rc[id] = rc;
}
u32 shim_reads_of(u32 id) { return id < MAX_IDS ? g_reads[id] : 0; }
void shim_fail_delete(const char* path_ends_with, Result rc) {
    g_fail_delete_suffix = path_ends_with;
    g_fail_delete_rc = rc;
}

// ---- BIS storages ---------------------------------------------------------------------------

Result fsOpenBisStorage(FsStorage* out, FsBisPartitionId id) {
    memset(out, 0, sizeof *out);
    g_open_calls++;
    if (id < 0 || id >= MAX_IDS) return RC_INVALID_ARG;
    g_open_count[id]++;
    if (g_deny[id]) return g_deny[id];
    if (g_deny_nth[id] == g_open_count[id]) return g_deny_nth_rc[id];
    if (!g_images[id][0]) return RC_INVALID_ARG;
    out->fp = fopen(g_images[id], "rb");
    if (!out->fp) return RC_INVALID_ARG;
    out->size = file_size(g_images[id]);
    out->id = (u32)id;
    return 0;
}

Result fsStorageGetSize(FsStorage* s, s64* out) {
    if (g_getsize_forced) {
        *out = g_getsize_reported;
        return g_getsize_rc;
    }
    *out = s->size;
    return 0;
}

Result fsStorageRead(FsStorage* s, s64 off, void* buf, u64 size) {
    if (unusable(buf, size)) return RC_BUFFER;
    g_reads[s->id]++;
    if (off < 0 || off + (s64)size > s->size) return RC_INVALID_OFFSET;
    if (g_fail_read_nth > 0 && off == g_fail_read_off && --g_fail_read_nth == 0)
        return g_fail_read_rc;
    if (pread(fileno(s->fp), buf, size, (off_t)off) != (ssize_t)size) return RC_IO;
    return 0;
}

void fsStorageClose(FsStorage* s) {
    if (s->fp) fclose(s->fp);
    memset(s, 0, sizeof *s);
}

// ---- SD card filesystem ----------------------------------------------------------------------

Result fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option) {
    (void)option;
    char p[2048];
    full(p, sizeof p, fs->root, path);
    if (file_size(p) >= 0) return RC_ALREADY_EXISTS;
    if (size < 0) return RC_INVALID_ARG;
    if (g_free >= 0 && size > g_free) return RC_NO_SPACE;

    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return RC_PATH_NOT_FOUND;
    if (size > 0 && ftruncate(fd, (off_t)size) != 0) { close(fd); unlink(p); return RC_NO_SPACE; }
    close(fd);
    if (g_free >= 0) g_free -= size;
    return 0;
}

Result fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out) {
    memset(out, 0, sizeof *out);
    if (!path || !path[0]) return RC_INVALID_ARG;
    char p[2048];
    full(p, sizeof p, fs->root, path);
    out->fp = fopen(p, "r+b");
    if (!out->fp) return RC_PATH_NOT_FOUND;
    out->size = file_size(p);
    out->mode = mode;
    snprintf(out->path, sizeof out->path, "%s", path);
    return 0;
}

Result fsFileWrite(FsFile* f, s64 off, const void* buf, u64 size, u32 option) {
    (void)option;
    if (unusable(buf, size)) return RC_BUFFER;
    if (!(f->mode & FsOpenMode_Write)) return RC_NO_WRITE;
    if (off < 0) return RC_INVALID_OFFSET;
    if (size == 0) return 0;
    if (g_fail_write_nth > 0 && off == g_fail_write_off && --g_fail_write_nth == 0)
        return g_fail_write_rc;
    if (off + (s64)size > f->size) {
        if (!(f->mode & FsOpenMode_Append)) return RC_BEYOND_END;
        const s64 grow = off + (s64)size - f->size;
        if (g_free >= 0) {
            if (grow > g_free) return RC_NO_SPACE;
            g_free -= grow;
        }
        if (ftruncate(fileno(f->fp), (off_t)(off + (s64)size)) != 0) return RC_NO_SPACE;
        f->size = off + (s64)size;
    }
    if (fseeko(f->fp, (off_t)off, SEEK_SET) != 0) return RC_INVALID_OFFSET;
    if (fwrite(buf, 1, size, f->fp) != size) return RC_IO;
    return 0;
}

Result fsFileRead(FsFile* f, s64 off, void* buf, u64 size, u32 option, u64* bytes_read) {
    (void)option;
    if (bytes_read) *bytes_read = 0;
    if (unusable(buf, size)) return RC_BUFFER;
    if (!(f->mode & FsOpenMode_Read)) return MAKERESULT(2, 6200);
    if (off < 0 || off > f->size) return RC_INVALID_OFFSET;
    if (fseeko(f->fp, (off_t)off, SEEK_SET) != 0) return RC_INVALID_OFFSET;
    const size_t n = fread(buf, 1, size, f->fp);
    if (bytes_read) *bytes_read = n;
    return n == size ? 0 : RC_IO;
}

Result fsFileFlush(FsFile* f) { return fflush(f->fp) == 0 ? 0 : RC_IO; }
Result fsFileGetSize(FsFile* f, s64* out) { *out = f->size + g_size_skew; return 0; }
void   fsFileClose(FsFile* f) { if (f->fp) fclose(f->fp); memset(f, 0, sizeof *f); }

Result fsFsDeleteFile(FsFileSystem* fs, const char* path) {
    char p[2048];
    full(p, sizeof p, fs->root, path);
    const s64 size = file_size(p);
    if (size < 0) return RC_PATH_NOT_FOUND;
    if (g_fail_delete_suffix) {
        const size_t n = strlen(g_fail_delete_suffix);
        if (strlen(path) >= n && strcmp(path + strlen(path) - n, g_fail_delete_suffix) == 0)
            return g_fail_delete_rc;
    }
    if (unlink(p) != 0) return RC_PATH_NOT_FOUND;
    if (g_free >= 0) g_free += size;
    return 0;
}

Result fsFsGetEntryType(FsFileSystem* fs, const char* path, FsDirEntryType* out) {
    char p[2048];
    if (!path || !path[0]) return RC_INVALID_ARG;
    full(p, sizeof p, fs->root, path);
    struct stat st;
    if (stat(p, &st) != 0) return RC_PATH_NOT_FOUND;
    *out = S_ISDIR(st.st_mode) ? FsDirEntryType_Dir : FsDirEntryType_File;
    return 0;
}

Result fsFsGetFreeSpace(FsFileSystem* fs, const char* path, s64* out) {
    (void)fs; (void)path;
    if (g_free < 0) return RC_UNSUPPORTED;
    *out = g_free;
    return 0;
}

Result fsFsCommit(FsFileSystem* fs) { (void)fs; return g_commit_rc; }

// ---- SHA-256 (FIPS 180-4) --------------------------------------------------------------------

static const u32 K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_transform(Sha256Context* ctx, const u8* blk) {
    u32 m[64];
    for (int i = 0; i < 16; i++)
        m[i] = (u32)blk[i * 4] << 24 | (u32)blk[i * 4 + 1] << 16 | (u32)blk[i * 4 + 2] << 8 |
            blk[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const u32 s0 = ROTR(m[i - 15], 7) ^ ROTR(m[i - 15], 18) ^ (m[i - 15] >> 3);
        const u32 s1 = ROTR(m[i - 2], 17) ^ ROTR(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    u32 a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    u32 e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        const u32 S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 t1 = h + S1 + ch + K[i] + m[i];
        const u32 S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256ContextCreate(Sha256Context* ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85; ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a; ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

void sha256ContextUpdate(Sha256Context* ctx, const void* src, size_t size) {
    const u8* d = src;
    for (size_t i = 0; i < size; i++) {
        ctx->data[ctx->datalen++] = d[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

void sha256ContextGetHash(Sha256Context* ctx, void* dst) {
    u32 i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56) ctx->data[i++] = 0x00;
    } else {
        ctx->data[i++] = 0x80;
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }
    ctx->bitlen += 8 * ctx->datalen;
    for (i = 0; i < 8; i++) ctx->data[63 - i] = (u8)(ctx->bitlen >> (8 * i));
    sha256_transform(ctx, ctx->data);
    u8* out = dst;
    for (i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            out[j * 4 + i] = (u8)(ctx->state[j] >> (8 * (3 - i)));
    ctx->datalen = 0;
}

// ---- host-side helpers -----------------------------------------------------------------------

void shim_make_image(const char* path, s64 size, u64 seed, const u8* head, size_t head_len) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u8 buf[4096];
    u64 x = seed * 0x9E3779B97F4A7C15ULL + 12345;
    for (s64 off = 0; off < size; off += (s64)sizeof buf) {
        const size_t n = (size_t)(size - off < (s64)sizeof buf ? size - off : (s64)sizeof buf);
        for (size_t i = 0; i < n; i++, x = x * 6364136223846793005ULL + 1442695040888963407ULL)
            buf[i] = (u8)(x >> 33);
        if (head && off == 0 && head_len <= n)
            for (size_t i = 0; i < head_len; i++) buf[i] = head[i];
        fwrite(buf, 1, n, f);
    }
    fclose(f);
}

bool shim_files_equal(const char* a, const char* b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0 || !S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode))
        return false;
    if (sa.st_size != sb.st_size) return false;
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return false;
    }
    u8 ba[65536], bb[65536];
    bool same = true;
    s64 left = (s64)sa.st_size;
    while (left > 0 && same) {
        const size_t n = (size_t)(left < (s64)sizeof ba ? left : (s64)sizeof ba);
        if (fread(ba, 1, n, fa) != n || fread(bb, 1, n, fb) != n || memcmp(ba, bb, n) != 0)
            same = false;
        left -= (s64)n;
    }
    fclose(fa);
    fclose(fb);
    return same;
}

// ---- clock -----------------------------------------------------------------------------------

static bool g_virtual_time;
static u64 g_vnow;

u64 shim_now_ns(void) {
    if (g_virtual_time) return g_vnow;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

void shim_set_virtual_time(int on) {
    g_virtual_time = on != 0;
    g_vnow = 1000000000ULL;
}

void shim_advance_ns(u64 ns) { g_vnow += ns; }
