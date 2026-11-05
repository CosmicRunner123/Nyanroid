#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

// What a Windows executable says about itself: when it was linked and its version resource's
// strings. The engine builds differ (the patch's WA2.exe is a build of 2011-12-08 with a version
// resource, the extended edition's own one of 2011-12-27 without), so this tells them apart.
struct ExeInfo {
    uint32_t timestamp = 0;                      // the PE header's TimeDateStamp, seconds since 1970 (UTC)
    std::map<std::string, std::string> strings;  // StringFileInfo: ProductName, ProductVersion, ... (UTF-8)
};

// nullopt if `image` is not a PE file. A missing or malformed version resource leaves `strings` empty.
std::optional<ExeInfo> readExeInfo(ByteSpan image);

}  // namespace nyan::wa2
