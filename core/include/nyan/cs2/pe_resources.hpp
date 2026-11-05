#pragma once

#include <optional>
#include <string>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// Minimal reader for named resources inside a Windows PE (32- or 64-bit) image.
// Looks up resource TYPE/NAME (both string-named, case-sensitive ASCII) and returns
// the data of the first language entry. Returns nullopt if absent; throws if malformed.
std::optional<Bytes> findPeResource(ByteSpan image, const std::string& type, const std::string& name);

}  // namespace nyan::cs2
