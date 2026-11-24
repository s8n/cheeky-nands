// Stand-in for libnx's <switch.h> in the headless UI build (test/ui_test.sh passes -Itest/switch,
// so "#include <switch.h>" in source/main.c resolves here).
#pragma once
#include "../shim/shim.h"
#include "../ui_shim.h"
