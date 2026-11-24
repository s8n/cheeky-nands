// What source/main.c uses from libnx beyond test/shim/shim.h, as a host stand-in, so that the
// console UI can be run scripted: printf() is laid out on a text screen the way libnx's console
// does it, and the pad answers from a script. Values mirror libnx 4.12.0 where it has one.
#pragma once
#include "shim.h"

typedef struct { int consoleWidth, consoleHeight; } PrintConsole;
typedef struct { u64 down, held; } PadState;

enum {
    HidNpadButton_A        = 1u << 0,  HidNpadButton_B      = 1u << 1,
    HidNpadButton_X        = 1u << 2,  HidNpadButton_Y      = 1u << 3,
    HidNpadButton_L        = 1u << 6,  HidNpadButton_R      = 1u << 7,
    HidNpadButton_ZL       = 1u << 8,  HidNpadButton_ZR     = 1u << 9,
    HidNpadButton_Plus     = 1u << 10, HidNpadButton_Minus  = 1u << 11,
    HidNpadButton_Left     = 1u << 12, HidNpadButton_Up     = 1u << 13,
    HidNpadButton_Right    = 1u << 14, HidNpadButton_Down   = 1u << 15,
    HidNpadButton_StickLUp = 1u << 17, HidNpadButton_StickLDown = 1u << 19,
    HidNpadButton_StickRUp = 1u << 23, HidNpadButton_StickRDown = 1u << 25,
};
#define HidNpadButton_AnyUp   (HidNpadButton_Up | HidNpadButton_StickLUp | HidNpadButton_StickRUp)
#define HidNpadButton_AnyDown (HidNpadButton_Down | HidNpadButton_StickLDown | HidNpadButton_StickRDown)
#define HidNpadStyleSet_NpadStandard 1

#define MAKEHOSVERSION(major, minor, micro) \
    ((((major) & 0xFF) << 24) | (((minor) & 0xFF) << 16) | ((micro) & 0xFFFF))
#define HOSVER_MAJOR(version) (((version) >> 24) & 0xFF)
#define HOSVER_MINOR(version) (((version) >> 16) & 0xFF)
#define HOSVER_MICRO(version) ((version) & 0xFFFF)

PrintConsole* consoleInit(PrintConsole* console);
void consoleExit(PrintConsole* console);
void consoleClear(void);
void consoleUpdate(PrintConsole* console);

void padConfigureInput(u32 max_players, u32 style_set);
void padInitializeDefault(PadState* pad);
void padUpdate(PadState* pad);
u64  padGetButtons(const PadState* pad);
u64  padGetButtonsDown(const PadState* pad);

bool appletMainLoop(void);
Result appletSetAutoSleepDisabled(bool flag);
void svcSleepThread(s64 ns);
u32  hosversionGet(void);
FsFileSystem* fsdevGetDeviceFileSystem(const char* name);
Result fsFsCreateDirectory(FsFileSystem* fs, const char* path); // mkdir, "exists" is fine

// ---- the script ------------------------------------------------------------------------------
// Every padUpdate() takes the next entry and then reports the button as released again.
void ui_reset_script(void);
void ui_press(u64 button);          // one poll with this button newly pressed
void ui_repeat(u64 button, int n);  // n such polls back to back
void ui_idle(int polls);            // polls with nothing pressed
void ui_hold(u64 button, int polls); // polls with the button still held, not newly pressed
int  ui_script_left(void);          // entries not consumed
int  ui_polls_after_script(void);   // appletMainLoop() calls since the script ran out

// ---- the screen ------------------------------------------------------------------------------
// Every screen shown by consoleUpdate(), in order, without repeated frames.
int         ui_frame_count(void);
const char* ui_frame(int index);                              // rows joined with '\n', or ""
int         ui_frame_with(const char* text, int from);        // first frame >= from containing text, else -1

// ---- the network reader's transport, without sockets (test/net_stub.c) ---------------------
// A request a reader will send once the app listens; answered in order. "" ends the reader's
// connection; what follows is asked over a new one.
void ui_net_request(const char* line);
// The reader lets `seconds` pass before the `before_call`-th thing that is done on its next
// connection (1: before its request, 2: before the app's first answer). The app's idle hook is
// told of it a second at a time, and every second is one pad poll of the script.
void ui_net_silent(int before_call, int seconds);
// Whether the sockets of this console can be made non-blocking (they can, unless said otherwise).
void ui_net_nonblocking(bool yes);
const char* ui_net_sent(void); // everything the app sent to readers, in order
int ui_net_listen_calls(void);

// ---- sleep -----------------------------------------------------------------------------------
// From then on appletSetAutoSleepDisabled() fails; the app goes on without it.
void ui_sleep_refused(void);
bool ui_sleep_disabled_asked(void); // whether the app asked for the console not to sleep

// Whether fsdevGetDeviceFileSystem("sdmc") answers, and with which directory.
void ui_sd_card(int available, const char* root);
