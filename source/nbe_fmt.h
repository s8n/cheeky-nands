// Formatting helpers shared by the dump engine and the UI.
#pragma once
#include "nbe_platform.h"

// "511 B", "4.00 MiB", "2.50 GiB" into buf.
void nbe_fmt_size(char* buf, size_t cap, s64 size);

// Lower-case hex of a 32-byte hash into out (65 bytes).
void nbe_fmt_hex32(const u8* hash, char* out);

// Name of a documented FS result code (switchbrew "Error codes"), or NULL when unknown.
const char* nbe_result_hint(Result rc);

// "0x0000320002 PermissionDenied", or just the code when there is no known name.
void nbe_fmt_result(char* buf, size_t cap, Result rc);

// "hh:mm:ss" from a second count.
void nbe_fmt_hms(char* buf, size_t cap, u64 seconds);

const char* nbe_platform(void);

// True in a build for a Switch 2 (libounce). The host test build answers false.
bool nbe_is_switch2(void);

