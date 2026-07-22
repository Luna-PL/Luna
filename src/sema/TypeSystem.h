#pragma once

// Semantic analysis owns inference and constraint solving, but the canonical
// type graph itself lives in Core so MoonIR never depends on a Sema header.
#include "../core/TypeSystem.h"
#include "Inference.h"
