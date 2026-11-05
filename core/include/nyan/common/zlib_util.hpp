#pragma once

#include "nyan/common/bytes.hpp"

namespace nyan {

// Inflates a zlib stream whose decompressed size is known up front.
// Throws if the stream is corrupt or does not produce exactly `expectedSize` bytes.
Bytes zlibInflate(ByteSpan compressed, size_t expectedSize);

// The first `count` bytes of a zlib stream (fewer if it is shorter), without inflating the rest.
Bytes zlibInflatePrefix(ByteSpan compressed, size_t count);

}  // namespace nyan
