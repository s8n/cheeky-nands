// Platform selection for code shared between the console build and the host test.
// The console build uses libnx (switch1) or libounce (switch2), which expose the same API.
// The host test build (NBE_HOST_TEST) gets a small POSIX stand-in in test/shim/shim.h.
#pragma once

#ifdef NBE_HOST_TEST
#include "shim.h"
#else
#include <switch.h>
#endif
