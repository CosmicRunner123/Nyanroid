#include "nyan/common/error.hpp"

namespace nyan {

void fail(const std::string& message) {
    throw Error(message);
}

}  // namespace nyan
