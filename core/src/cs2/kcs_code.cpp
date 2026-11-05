#include "nyan/cs2/kcs_code.hpp"

#include <array>
#include <cstring>

#include "nyan/common/zlib_util.hpp"

namespace nyan::cs2 {
namespace {

// Operands per opcode, as the kcs interpreter reads them; -1 for a
// byte that is no opcode (stepped over alone, as the disassembler does).
constexpr std::array<int8_t, 256> kOperands = [] {
    std::array<int8_t, 256> t{};
    t.fill(-1);
    for (int op = 0x01; op <= 0x24; ++op) t[op] = 2;  // arithmetic, comparisons, assignments
    t[0x00] = 0;
    t[0x08] = t[0x0c] = 1;                                          // not, lnot
    for (int op = 0x16; op <= 0x1a; ++op) t[op] = 1;                // inc, dec, inc2, dec2, neg
    for (int op = 0x25; op <= 0x29; ++op) t[op] = 1;                // push, pop, jz, jnz, jmp
    t[0x2a] = t[0x2b] = t[0x2c] = 2;                                // call, callidx, ret
    t[0x2d] = 0;                                                    // drop
    t[0x2e] = t[0x2f] = 1;                                          // pushaddr, pushref
    t[0x30] = 2;
    t[0x31] = t[0x32] = 1;                                          // alloc, free
    t[0x33] = 0;                                                    // switch
    t[0x34] = 2;                                                    // case
    t[0x35] = 3;                                                    // caserng
    for (int op = 0x36; op <= 0x3a; ++op) t[op] = 1;                // conversions
    t[0x3b] = t[0x3c] = 0;                                          // halt, clr
    t[0x3d] = 1;                                                    // local
    t[0x3e] = 2;                                                    // index
    t[0x41] = 2;                                                    // line
    t[0x42] = 1;
    t[0x43] = 2;                                                    // send
    t[0x44] = 1;                                                    // unsend
    return t;
}();

}  // namespace

size_t kcsNativeCallSites(ByteSpan kcs, int32_t native) {
    if (kcs.size() < 0x34 || std::memcmp(kcs.data(), "KCS\0", 4) != 0) return 0;
    const uint32_t codeAt = readLE32(kcs.data() + 4 + 2 * 4), codeSize = readLE32(kcs.data() + 4 + 3 * 4);
    if (codeAt < 0x34 || codeAt > kcs.size()) return 0;
    Bytes code;
    try {
        code = zlibInflatePrefix(kcs.subspan(codeAt), codeSize);
    } catch (...) {
        return 0;
    }
    size_t n = 0;
    for (size_t ip = 0; ip < code.size();) {
        const uint8_t op = code[ip];
        const int8_t args = kOperands[op];
        if (args < 0) {
            ++ip;
            continue;
        }
        if (op == 0x2b && ip + 6 <= code.size() && static_cast<int32_t>(readLE32(&code[ip + 2])) == native) ++n;
        ip += 1 + 5 * static_cast<size_t>(args);
    }
    return n;
}

}  // namespace nyan::cs2
