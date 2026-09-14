#pragma once

#include "RuntimeABI.h"

// Process-lifetime service tables used by compiler-generated application
// entry points. The minimal Runtime default deliberately does not expose
// input or filesystem access; installation is an explicit application action.
const LunaConsoleV1* lunaApplicationConsoleV1();
const LunaFileSystemV1* lunaApplicationFileSystemV1();

// Internal Runtime state transition used by the public application-profile
// installer. Kept separate so output-only links do not extract this file.
const LunaAllocatorV1* lunaDefaultAllocatorV1();
int lunaInstallApplicationHostServicesV1(const LunaHostServicesV1* services);
