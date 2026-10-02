#pragma once

#include <stdexcept>

namespace opl {

// Every failure in openplugin is an opl::Error (or a subclass) with a message that can be shown
// to the user as-is.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace opl
