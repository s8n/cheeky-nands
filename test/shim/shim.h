// Host stand-in for the slice of libnx that bis.c and nbe_fmt.c use, so the dump engine can be
// compiled unchanged (NBE_HOST_TEST) and exercised on a PC. Mirrors libnx 4.12.0's macros
// (result.h: success is exactly 0, R_VALUE masks off the severity bits) and the FS behaviour
// the engine relies on. Fault injection lives in the shim_ functions below.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t Result;
typedef int64_t s64;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res)    ((res) != 0)
#define R_MODULE(res)    ((res) & 0x1FF)
#define R_DESCRIPTION(res) (((res) >> 9) & 0x1FFF)
#define R_VALUE(res)     ((res) & 0x3FFFFF)
#define MAKERESULT(module, description) ((((module) & 0x1FF)) | ((description) & 0x1FFF) << 9)

#define FS_MAX_PATH 0x301

typedef enum { FsDirEntryType_Dir = 0, FsDirEntryType_File = 1 } FsDirEntryType;
typedef enum { FsOpenMode_Read = 1, FsOpenMode_Write = 2, FsOpenMode_Append = 4 } FsOpenMode;
typedef enum { FsWriteOption_None = 0, FsWriteOption_Flush = 1 } FsWriteOption;
typedef int FsBisPartitionId;

typedef struct { FILE* fp; s64 size; u32 id; } FsStorage;
typedef struct { FILE* fp; s64 size; u32 mode; char path[FS_MAX_PATH]; } FsFile;
typedef struct { const char* root; } FsFileSystem;

Result fsOpenBisStorage(FsStorage* out, FsBisPartitionId id);
Result fsStorageGetSize(FsStorage* s, s64* out);
Result fsStorageRead(FsStorage* s, s64 off, void* buf, u64 size);
void   fsStorageClose(FsStorage* s);

Result fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option);
Result fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out);
Result fsFileWrite(FsFile* f, s64 off, const void* buf, u64 size, u32 option);
Result fsFileRead(FsFile* f, s64 off, void* buf, u64 size, u32 option, u64* bytes_read);
Result fsFileFlush(FsFile* f);
Result fsFileGetSize(FsFile* f, s64* out);
void   fsFileClose(FsFile* f);
Result fsFsDeleteFile(FsFileSystem* fs, const char* path);
Result fsFsGetEntryType(FsFileSystem* fs, const char* path, FsDirEntryType* out);
Result fsFsGetFreeSpace(FsFileSystem* fs, const char* path, s64* out);
Result fsFsCommit(FsFileSystem* fs);

typedef struct { u32 state[8]; u64 bitlen; u8 data[64]; size_t datalen; } Sha256Context;
void sha256ContextCreate(Sha256Context* ctx);
void sha256ContextUpdate(Sha256Context* ctx, const void* src, size_t size);
void sha256ContextGetHash(Sha256Context* ctx, void* dst);

// ---- fault injection / world setup ---------------------------------------------------------
// Every function below resets the whole simulated world: storages, SD card, faults.
void shim_init(const char* sd_root);
void shim_storage_set(u32 id, const char* image_path); // OpenBisStorage(id) yields this file
void shim_storage_deny(u32 id, Result rc);             // OpenBisStorage(id) fails with rc
void shim_storage_getsize(Result rc, s64 reported);    // what fsStorageGetSize answers (rc 0: reported)
void shim_fail_delete(const char* path_ends_with, Result rc); // DeleteFile of such a path fails
// The nth OpenBisStorage(id) (1-based; the scan is the first) fails with rc.
void shim_storage_deny_nth(u32 id, Result rc, int nth);
u32  shim_reads_of(u32 id);                               // fsStorageRead calls so far

// ---- host-side helpers, shared by the tests --------------------------------------------
// A pseudo-random image of `size` bytes; its first `head_len` bytes are `head`.
void shim_make_image(const char* path, s64 size, u64 seed, const u8* head, size_t head_len);
bool shim_files_equal(const char* a, const char* b);      // same length and bytes

// The clock behind bis_now_ns(): the real one, or a virtual one that only moves when
// svcSleepThread() is called, so that a scripted run (test/ui_shim.c) is deterministic.
u64  shim_now_ns(void);
void shim_set_virtual_time(int on);
void shim_advance_ns(u64 ns);
void shim_free_space(s64 bytes);                       // < 0: unlimited (default: -1)
// Make the nth call (1-based) that starts at `offset` fail with rc.
void shim_fail_read_at(s64 offset, Result rc, int nth);
void shim_fail_write_at(s64 offset, Result rc, int nth);
void shim_fail_commit(Result rc);
void shim_size_skew(s64 delta);                        // GetSize on files reports size + delta
void shim_unusable_over(size_t bytes);                 // buffers of >= this many bytes -> 0xD401
int  shim_open_bis_calls(void);                        // how many times OpenBisStorage was called
