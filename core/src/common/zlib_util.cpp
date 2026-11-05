#include "nyan/common/zlib_util.hpp"

#include <zlib.h>

namespace nyan {

Bytes zlibInflate(ByteSpan compressed, size_t expectedSize) {
    Bytes out(expectedSize);
    uLongf outLen = static_cast<uLongf>(expectedSize);
    int rc = uncompress(out.data(), &outLen, compressed.data(), static_cast<uLong>(compressed.size()));
    if (rc != Z_OK) fail("zlib inflate failed (code " + std::to_string(rc) + ")");
    if (outLen != expectedSize) {
        fail("zlib inflate size mismatch: expected " + std::to_string(expectedSize) + ", got " +
             std::to_string(outLen));
    }
    return out;
}

Bytes zlibInflatePrefix(ByteSpan compressed, size_t count) {
    Bytes out(count);
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) fail("zlib init failed");
    zs.next_in = const_cast<Bytef*>(compressed.data());
    zs.avail_in = static_cast<uInt>(compressed.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(count);
    int rc = inflate(&zs, Z_SYNC_FLUSH);
    size_t got = count - zs.avail_out;
    inflateEnd(&zs);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) fail("zlib inflate failed (code " + std::to_string(rc) + ")");
    out.resize(got);
    return out;
}

}  // namespace nyan
