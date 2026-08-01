#pragma once

#include "RuntimeABI.h"

// Process-lifetime service tables used by compiler-generated application
// entry points. The minimal Runtime default deliberately does not expose
// input or filesystem access; installation is an explicit application action.
const LunaConsoleV1* lunaApplicationConsoleV1();
const LunaFileSystemV1* lunaApplicationFileSystemV1();
