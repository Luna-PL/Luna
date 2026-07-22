#pragma once

#include "MoonIR.h"

#include <iosfwd>
#include <string>

namespace moon {

class Printer {
public:
    void print(const Module& module, std::ostream& out) const;
    std::string str(const Module& module) const;
    void printCostReport(const Module& module, std::ostream& out) const;
};

} // namespace moon
