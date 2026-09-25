#pragma once

#include "MoonIR.h"

#include <string>
#include <unordered_map>

namespace moon {

// Keyed by the complete declaration identity.  This is a derived compiler
// fact: source metadata cannot opt a function into or out of the effect.
using FragmentContextEffectMap = std::unordered_map<std::string, bool>;

std::string fragmentContextEffectKey(const DeclarationRef& reference);

// Computes the least fixed point seeded by RuntimeSlot terminators and closed
// over exact direct-call DeclarationRefs.
FragmentContextEffectMap computeFragmentContextEffects(const Module& module);

// Stores the derived summary on every executable function after CFG sealing.
void inferFragmentContextEffects(Module& module);

} // namespace moon
