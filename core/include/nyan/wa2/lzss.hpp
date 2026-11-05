#pragma once

#include <cstddef>

#include "nyan/common/bytes.hpp"

namespace nyan::wa2 {

struct LzssResult {
    Bytes data;           // the size asked for; bytes the stream did not produce stay 0
    size_t produced = 0;  // bytes the stream produced (the engine reports any count but the size)
    // A reference read ring bytes 0xfee..0xfff before the stream wrote them. The engine fills only
    // 0..0xfed with spaces for each file and keeps the rest from the file before (its ring is one
    // global buffer), so such a stream's output depends on what was unpacked before it.
    bool readStaleTail = false;
};

// The engine's LZSS (WA2.exe 0x436df0), used by KCAP archives: a 4 KB ring written from 0xfee, one
// flag byte per eight items read from its lowest bit (1: a literal byte, 0: a two-byte reference to
// ring offset `b0 | (b1 & 0xf0) << 4`, `(b1 & 0x0f) + 3` bytes long). Unpacks `packed` to `size`
// bytes.
LzssResult lzssDecompress(ByteSpan packed, size_t size);

}  // namespace nyan::wa2
