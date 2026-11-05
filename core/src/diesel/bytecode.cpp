#include "nyan/diesel/bytecode.hpp"

#include <cstdint>
#include <string>

namespace nyan::diesel {

namespace {

constexpr uint32_t kHead = 0x53514952;  // 'SQIR'
constexpr uint32_t kPart = 0x50415254;  // 'PART'
constexpr uint32_t kTail = 0x5441494c;  // 'TAIL'
constexpr uint32_t kOtNull = 0x01000001, kOtInteger = 0x05000002, kOtFloat = 0x05000004,
                   kOtString = 0x08000010;

struct Narrower {
    ByteReader r;
    Bytes out;
    size_t clipped = 0;

    void put32(uint32_t v) {
        uint8_t b[4];
        writeLE32(b, v);
        out.insert(out.end(), b, b + 4);
    }
    void copy(size_t n) {
        auto s = r.span(n);
        out.insert(out.end(), s.begin(), s.end());
    }
    int64_t get64() {
        uint64_t lo = r.u32(), hi = r.u32();
        return static_cast<int64_t>(lo | (hi << 32));
    }
    // An SQInteger (8 bytes) as 4.
    int64_t integer() {
        int64_t v = get64();
        if (v < INT32_MIN || v > INT32_MAX) ++clipped;
        put32(static_cast<uint32_t>(v));
        return v;
    }
    // A count: it must fit.
    size_t count() {
        int64_t v = get64();
        if (v < 0 || v > 0x7fffffff) fail("closure: bad count " + std::to_string(v) + " at " + std::to_string(r.pos() - 8));
        put32(static_cast<uint32_t>(v));
        return static_cast<size_t>(v);
    }
    void tag(uint32_t want) {
        int64_t t = get64();
        if (t != want) fail("closure: tag " + std::to_string(t) + " where " + std::to_string(want) + " belongs");
        put32(want);
    }
    void object() {
        uint32_t type = r.u32();
        put32(type);
        switch (type) {
            case kOtString: copy(count()); break;
            case kOtInteger: integer(); break;
            case kOtFloat: copy(4); break;
            case kOtNull: break;
            default: fail("closure: object type " + std::to_string(type));
        }
    }
    void proto() {
        tag(kPart);
        object();  // source name
        object();  // name
        tag(kPart);
        size_t literals = count(), parameters = count(), outers = count(), locals = count(),
               lines = count(), defaults = count(), instructions = count(), functions = count();
        tag(kPart);
        for (size_t i = 0; i < literals; ++i) object();
        tag(kPart);
        for (size_t i = 0; i < parameters; ++i) object();
        tag(kPart);
        for (size_t i = 0; i < outers; ++i) {
            // The outer's type: SQFunctionProto::Save writes the 4-byte SQOuterType as an
            // SQUnsignedInteger, so the high half is whatever followed it (padding).
            put32(static_cast<uint32_t>(get64()));
            object();  // where it comes from
            object();  // its name
        }
        tag(kPart);
        for (size_t i = 0; i < locals; ++i) {
            object();                     // name
            count(), count(), count();    // pos, start op, end op
        }
        tag(kPart);
        for (size_t i = 0; i < lines; ++i) integer(), integer();  // SQLineInfo: line, op
        tag(kPart);
        for (size_t i = 0; i < defaults; ++i) integer();
        tag(kPart);
        copy(instructions * 8);  // SQInstruction: arg1 (SQInt32), op, arg0, arg2, arg3
        tag(kPart);
        for (size_t i = 0; i < functions; ++i) proto();
        integer();  // stack size
        copy(1);    // generator (bool)
        copy(1);    // varparams (bool)
    }
};

}  // namespace

Bytes narrowClosure64(ByteSpan in, size_t* clipped) {
    Narrower n{ByteReader(in, "closure"), {}, 0};
    n.out.reserve(in.size());
    n.copy(2);  // SQ_BYTECODE_STREAM_TAG (unsigned short)
    n.tag(kHead);
    n.tag(1);  // sizeof(SQChar)
    n.proto();
    n.tag(kTail);
    if (n.r.remaining() != 0) fail("closure: " + std::to_string(n.r.remaining()) + " bytes after the tail");
    if (clipped) *clipped = n.clipped;
    return std::move(n.out);
}

}  // namespace nyan::diesel
