#pragma once

#include <iosfwd>

namespace luna::driver {

int runRepl(
    std::istream& input, std::ostream& output, std::ostream& errors);

} // namespace luna::driver
