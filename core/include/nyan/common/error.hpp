#pragma once

#include <stdexcept>
#include <string>

namespace nyan {

// Base exception for all recoverable core errors (bad data, missing files, ...).
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message) : std::runtime_error(message) {}
};

[[noreturn]] void fail(const std::string& message);

}  // namespace nyan
