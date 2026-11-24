// BIS (NAND) partition discovery and SD dump engine.
//
// This code is READ-ONLY towards NAND. The only IStorage commands it ever sends to a Bis
// storage are GetSize (cmd 4) and Read (cmd 0). Nothing here calls fsStorageWrite/SetSize/
// OperateRange, so it cannot modify the console's NAND no matter how it fails.
#pragma once
#include "nbe_platform.h"

#define NBE_APP_NAME    "cheeky-nands"
#define NBE_APP_VERSION "0.1"

#define BIS_KIB (1024ULL)
#define BIS_MIB (1024ULL * BIS_KIB)
#define BIS_GIB (1024ULL * BIS_MIB)

// Largest file this tool will ever ask the SD card for.
//
// libnx has no handling for files >4 GiB: fsFileWrite/fsFileSetSize/fsFsCreateFile pass the
// 64-bit size straight to the FS sysmodule (nx/source/services/fs.c in libnx 4.12.0), and the
// stdio layer (fs_dev.c) only maps a failure to errno. Whether a single file may exceed
// 4 GiB is decided entirely by the SD card's filesystem: exFAT stores 64-bit lengths, FAT32
// stores a 32-bit one (max 0xFFFFFFFF bytes per file). So the splitting is done here.
#ifndef BIS_PART_SIZE
#define BIS_PART_SIZE 0xFFFF8000ULL // 4 GiB - 32 KiB, safely below FAT32's per-file maximum
#endif
#ifndef BIS_CHUNK_SIZE
#define BIS_CHUNK_SIZE (4ULL * BIS_MIB) // bytes per fsStorageRead + fsFileWrite pair
#endif
#define BIS_MAX_PART_FILES 100 // ".NN" suffixes are two digits
#define BIS_SCAN_MAX_ID    255 // highest BisPartitionId tried during a scan
#define BIS_MAX_PARTITIONS 64

// First size a FAT32 directory entry cannot represent. Used by the optional on-console test.
#define BIS_BIGFILE_TEST_SIZE 0x100000000ULL

// App-internal Result module (not a Horizon module): lets the UI tell "our own check failed"
// apart from "the FS sysmodule returned this".
#define NBE_MODULE 436
#define NBE_RC_SIZE_MISMATCH MAKERESULT(NBE_MODULE, 1) // a part file has the wrong length
#define NBE_RC_BAD_SIZE      MAKERESULT(NBE_MODULE, 2) // GetSize returned a negative length
#define NBE_RC_NET           MAKERESULT(NBE_MODULE, 10) // the connection failed
#define NBE_RC_NET_REQUEST   MAKERESULT(NBE_MODULE, 11) // a request that does not parse
#define NBE_RC_NET_RANGE     MAKERESULT(NBE_MODULE, 12) // a range outside the partition

typedef struct {
    u32 id;
    char name[32];       // libnx's FsBisPartitionId name, or "Unknown_NN"
    const char* alias;   // familiar name (Switch 1 GPT label), or NULL
    bool known;          // id is in the static table below
    bool openable;       // OpenBisStorage succeeded and GetSize returned a non-negative size
    Result open_rc;      // last OpenBisStorage/GetSize result (0 when openable)
    s64 size;            // bytes, valid when openable
} BisPartition;

typedef struct {
    BisPartition parts[BIS_MAX_PARTITIONS];
    int count;           // entries in parts[] (openable ones plus known-but-failing ones)
    int openable_count;
    s64 openable_total;  // sum of openable sizes
    bool truncated;      // more entries found than parts[] holds
} BisTable;

// Probe BisPartitionId 0..BIS_SCAN_MAX_ID. Read-only: OpenBisStorage, GetSize, Close.
// Calls bis_scan_as() with the platform of this build.
void bis_scan(BisTable* t);

// bis_scan(), with the platform given. With `switch2` true, an id that a Switch 2 has no Bis
// storage for and that refuses to open is left out of the table, so neither the list, ALL nor
// the network LIST shows it. An id that opens is always listed.
void bis_scan_as(BisTable* t, bool switch2);

// True for the ids a Switch 2 answers 0x002EE202 ("no such storage") for: 21 to 26 (the
// BootConfigAndPackage2 storages), 29, 32, 34, 36 and 39.
bool bis_absent_on_switch2(u32 id);

// Output file layout for one dump.
typedef struct {
    s64 size;
    int parts;           // number of part files
    bool multi;          // true when the parts carry ".NN" suffixes
    char base[FS_MAX_PATH]; // "<dir>/<name>.bin" (part 0 when not multi)
    char sha[FS_MAX_PATH];  // "<dir>/<name>.sha256"
    char info[FS_MAX_PATH]; // "<dir>/<name>.info"
} BisPlan;

// Fill plan for a dump of `size` bytes. Always fills base/sha/info, whatever the size.
void bis_plan(BisPlan* plan, const char* out_dir, const char* name, s64 size, bool split);

// Path of part `index` (0 <= index < plan->parts). `out` must hold FS_MAX_PATH bytes.
void bis_part_path(const BisPlan* plan, int index, char* out);

// Number of part files that would be written, and their length (the last one may be shorter).
int   bis_part_count(s64 size, bool split);
s64   bis_part_length(s64 size, int index, bool split);

// true when any output of this dump (any part, .sha256 or .info) already exists.
bool bis_outputs_exist(FsFileSystem* sd, const char* out_dir, const char* name);

// Total length of those files, or -1 when one of them cannot be measured.
s64 bis_outputs_size(FsFileSystem* sd, const char* out_dir, const char* name);

// Delete every file this tool would write for `name`: parts 0..99, .sha256, .info.
// Missing files are not an error. Returns the first deletion error, or 0.
Result bis_delete_outputs(FsFileSystem* sd, const char* out_dir, const char* name);

// true when a file exists at path (false on any error too).
bool bis_file_exists(FsFileSystem* sd, const char* path);

typedef enum {
    BisDump_Ok = 0,
    BisDump_Cancelled, // user cancelled; the partial files were removed
    BisDump_Error,     // a call failed; the partial files were removed
} BisDumpStatus;

// What the progress callback is told after a chunk. part, path and note are valid only for the
// duration of that call: a callback that needs them later copies them.
typedef struct {
    const BisPartition* part;
    int part_index;     // part being written (0-based)
    int parts;
    const char* path;   // output file being written
    s64 done;           // bytes of the partition written so far
    s64 total;          // partition size
    u64 elapsed_ns;
    bool network;         // the bytes go to a reader on the PC, not to a file
    const char* note;     // free text for the screen (a request line), or NULL
} BisProgress;

typedef struct {
    void* user;
    // Called after every chunk. Return false to cancel the dump.
    bool (*progress)(void* user, const BisProgress* pg);
    // Called with short status text such as "allocating User.bin.01". May be NULL.
    void (*message)(void* user, const char* text);
} BisCallbacks;

typedef struct {
    BisDumpStatus status;
    Result rc;              // failing call's result (0 if none or not applicable)
    // Where it stopped: "plan", "open", "replace", "create", "read", "write", "close",
    // "verify", "sha256", "info", "done" (or "progress" when cancelled).
    const char* stage;
    s64 fail_offset;        // partition offset of the failing read/write, -1 otherwise
    int part_index;         // part being written at failure, -1 otherwise
    int parts;              // part files written (when Ok)
    bool multi;
    s64 size;               // bytes dumped (when Ok)
    u64 elapsed_ns;
    u8 sha256[32];          // SHA-256 of the whole partition image
    u8 part_sha256[BIS_MAX_PART_FILES][32]; // SHA-256 of each part file
    int plaintext;          // 1: first bytes look like plain data, 0: not, -1: not checked
    bool used_bounce;       // FS could not map the chunk buffer; the slow path was used
    char stem[80];          // the file name without extension: the partition name, or
                            // that plus "+0x<start>-0x<end>" for a partial dump
    s64 start;              // first byte of the partition that was dumped
    s64 source_size;        // size of the partition the bytes came from
    char message[192];
} BisDumpResult;

// Dump one partition to "<out_dir>/<name>.bin" or, when split is on and the size exceeds
// BIS_PART_SIZE, to "<out_dir>/<name>.bin.NN" parts of at most BIS_PART_SIZE bytes.
//
// An earlier dump of the same name (parts, .sha256, .info) is replaced: it is removed once
// the partition is open and nothing else can be refused, and a dump that then fails or is
// cancelled leaves no file of that name behind. Callers that want to ask first use
// bis_outputs_exist(). On Ok the part files exist plus "<name>.sha256" (sha256sum -c format,
// one line per part) and "<name>.info" (key=value text).
BisDumpStatus bis_dump(const BisPartition* part, FsFileSystem* sd, const char* out_dir,
                       bool split, const BisCallbacks* cb, BisDumpResult* out);

// A part of a partition: the bytes [start, start + length). Both are multiples of 512.
// length 0 means "to the end of the partition".
typedef struct {
    s64 start;
    s64 length;
} BisRange;

// As bis_dump, for the bytes in `range`, read against the size the storage reports at the time.
// A range that turns out to be the whole partition is written exactly like bis_dump's. Any
// other is written as "<name>+0x<start>-0x<end>.bin" (plus the .sha256 and .info files), and
// only ever replaces files with exactly that name: a partial dump cannot delete a full dump,
// nor another partial one. Refused, with stage "plan" or "open": a start or length that is not
// a multiple of 512, a negative start, a start at or beyond the end of the partition.
BisDumpStatus bis_dump_range(const BisPartition* part, FsFileSystem* sd, const char* out_dir,
                             bool split, const BisRange* range, const BisCallbacks* cb,
                             BisDumpResult* out);

// The file name (without extension) of a partial dump: "<name>+0x<start>-0x<end>".
void bis_partial_stem(char* out, size_t cap, const char* name, s64 start, s64 end);

// -----------------------------------------------------------------------------------------
// Network reader: a program on the PC (net_pull.py) pulls partition bytes over TCP.
// The console only reads NAND; these are all the requests there are, one per line:
//
//   PING                        -> "PONG"
//   LIST                        -> "PART <id> <openable 0|1> <size> <open result> <name>" for
//                                  every row of the table, then "END" (or "END truncated")
//   READ <id> <start> <length>  -> "OK <start> <length> <size>", <length> bytes,
//                                  "SHA256 <hex of those bytes>"
//   HASH <id> <start> <length>  -> the same without the bytes
//
// length 0 means to the end of the partition. Numbers are decimal. A request that is refused
// gets "ERR <result> <stage>" and the connection stays usable. When a READ stops halfway (a
// failure, or the progress callback asking to stop) the connection is closed instead, so that
// nothing can be taken for the rest of the data; the reader then knows how much it has.
// -----------------------------------------------------------------------------------------

// One connection to a reader. The two functions block.
typedef struct {
    void* user;
    Result (*send_all)(void* user, const void* data, size_t len); // all of it, or an error
    // One line without its '\n'. 1: a line. 0: the other side closed. -1: an error.
    int (*recv_line)(void* user, char* line, size_t cap);
} NbeLink;

typedef struct {
    char request[96];   // the last request line, as it came in ("" when there was none)
    Result rc;          // of that request: the call that failed, or 0
    const char* stage;  // where it stopped: "recv", "parse", "open", "read", "send", "done"
    bool refused;       // it got "ERR ...": it did not parse, or would not open. Nothing was sent
    s64 start;          // of the last READ or HASH: where it starts ...
    s64 length;         // ... how long it is, once clamped to the partition ...
    s64 sent;           // ... and how many bytes of a READ went out
    u64 elapsed_ns;     // of that request
    bool cancelled;     // the progress callback asked to stop
    u8 sha256[32];      // of the bytes of the last READ or HASH that finished
} BisServeResult;

// Answer the requests that arrive on `link` until the reader closes it, a call fails, or the
// progress callback returns false. `cb->progress` is called after every chunk of a READ or a
// HASH, with pg->network set. `r` tells of the last request of the connection, also when the
// reader closes it right after: a request that was refused stays refused.
void bis_serve(NbeLink* link, const BisTable* table, const BisCallbacks* cb, BisServeResult* r);

// On-console experiment: can this SD card hold a file of `size` bytes? Creates
// "<out_dir>/_bigfile_test.bin" with that size, re-opens it to read the stored length back,
// then deletes it. See README ("Files larger than 4 GiB") before running it on FAT32.
typedef struct {
    s64 requested;
    Result create_rc;      // CreateFile result
    Result reopen_rc;     // OpenFile/GetSize result after a successful create
    s64 stored_size;      // length reported by GetSize after re-opening
    Result delete_rc;
    bool accepts;         // create + reopen succeeded and stored_size == requested
} BisBigFileResult;

void bis_bigfile_test(FsFileSystem* sd, const char* out_dir, s64 size, BisBigFileResult* out);

// Monotonic nanoseconds.
u64 bis_now_ns(void);
