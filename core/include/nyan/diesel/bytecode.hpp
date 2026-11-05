#pragma once

#include <cstddef>

#include "nyan/common/bytes.hpp"

namespace nyan::diesel {

// A closure stream written by a 64-bit build of the exes' Squirrel 2.2.4 (Phantom of Inferno's
// phantom.exe: SQInteger, SQUnsignedInteger and the stream's tags are 8 bytes; object types, string
// characters, floats, instructions and the two flags are as in a 32-bit build) rewritten in the
// 32-bit layout Nyanroid's VM reads (SQClosure::Load / SQFunctionProto::Load, field by field).
// An outer variable's type is the low half of its 8 bytes (the 4-byte enum written with its padding).
// Integer literals outside 32 bits keep their low 32 bits; `clipped` counts them. Throws nyan::Error
// on a stream that does not follow the layout or does not end where `in` does.
Bytes narrowClosure64(ByteSpan in, size_t* clipped = nullptr);

}  // namespace nyan::diesel
