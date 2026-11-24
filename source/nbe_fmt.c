#include "nbe_fmt.h"
#include <stdio.h>
#include "bis.h"

void nbe_fmt_size(char* buf, size_t cap, s64 size) {
    const u64 s = size > 0 ? (u64)size : 0;

    if (s < BIS_KIB) {
        snprintf(buf, cap, "%llu B", (unsigned long long)s);
        return;
    }
    if (s < BIS_MIB) {
        snprintf(buf, cap, "%llu%s KiB", (unsigned long long)(s / BIS_KIB),
                 (s % BIS_KIB) ? "+" : "");
        return;
    }

    const bool gib = s >= BIS_GIB;
    const u64 unit = gib ? BIS_GIB : BIS_MIB;
    snprintf(buf, cap, "%llu.%02llu %s", (unsigned long long)(s / unit),
             (unsigned long long)((s % unit) * 100 / unit), gib ? "GiB" : "MiB");
}

void nbe_fmt_hex32(const u8* hash, char* out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", hash[i]);
    out[64] = '\0';
}

const char* nbe_result_hint(Result rc) {
    if (R_SUCCEEDED(rc)) return "ok";

    if (R_MODULE(rc) == NBE_MODULE) {
        switch (R_DESCRIPTION(rc)) {
        case 1: return "part file has the wrong length";
        case 2: return "negative size";
        case 10: return "connection failed";
        case 11: return "request does not parse";
        case 12: return "range outside the partition";
        }
        return NULL;
    }
    if (R_VALUE(rc) == 0xD401) return "buffer not mappable for FS IPC";
    if (R_MODULE(rc) != 2) return NULL; // not the FS sysmodule

    switch (R_DESCRIPTION(rc)) {
    case 1:    return "PathNotFound";
    case 2:    return "PathAlreadyExists";
    case 7:    return "TargetLocked";
    case 39:   return "UsableSpaceNotEnoughSdCard";
    case 6001: return "InvalidArgument"; // for OpenBisStorage: an id the system does not have?
    case 6002: return "InvalidPath";
    case 6003: return "TooLongPath";
    case 6201: return "FileExtensionWithoutOpenModeAllowAppend";
    case 6455: return "FileNotClosed";
    case 6457: return "WriteModeFileNotClosed";
    }
    if (R_DESCRIPTION(rc) >= 6400 && R_DESCRIPTION(rc) <= 6449) return "PermissionDenied";
    return NULL;
}

void nbe_fmt_result(char* buf, size_t cap, Result rc) {
    const char* hint = nbe_result_hint(rc);
    if (hint) snprintf(buf, cap, "0x%08X %s", (unsigned)rc, hint);
    else snprintf(buf, cap, "0x%08X", (unsigned)rc);
}

void nbe_fmt_hms(char* buf, size_t cap, u64 seconds) {
    snprintf(buf, cap, "%02llu:%02llu:%02llu", (unsigned long long)(seconds / 3600),
             (unsigned long long)(seconds / 60 % 60), (unsigned long long)(seconds % 60));
}

const char* nbe_platform(void) {
#ifdef NBE_HOST_TEST
    return "host-test";
#elif defined(__SWITCH2__)
    return "switch2";
#else
    return "switch1";
#endif
}

bool nbe_is_switch2(void) {
#if !defined(NBE_HOST_TEST) && defined(__SWITCH2__)
    return true;
#else
    return false;
#endif
}
