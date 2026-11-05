#pragma once

#include <cstddef>
#include <cstdint>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// A compiled CatSystem2 script (.kcs: sscript, main, movie, ...): "KCS\0", twelve header words (the
// third the code's offset, 0x34, the fourth its size), then zlib data: the code, addressed from 0,
// and after it the script's data. Each instruction is an opcode byte and a fixed number of operands,
// each a type byte and a 32-bit value.
//
// How many `callidx <native>` instructions (opcode 0x2b) the code holds; 0 if it is not a .kcs.
// Nyanroid runs the scene scripts itself, so where the games' sscripts differ it asks the game's own:
// Fruit's sscript calls native 737 (an image's colour conversion) only from its image load, the later
// ones from their per-frame step too (see ScenePlayer::setModeEachFrame).
size_t kcsNativeCallSites(ByteSpan kcs, int32_t native);

}  // namespace nyan::cs2
