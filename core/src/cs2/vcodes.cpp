#include "nyan/cs2/vcodes.hpp"

#include <algorithm>

#include "nyan/cs2/blowfish.hpp"
#include "nyan/cs2/pe_resources.hpp"

namespace nyan::cs2 {
namespace {

std::string decryptCode(const Bytes& code, const Blowfish& bf) {
    Bytes buf = code;
    buf.resize((buf.size() + 7) & ~size_t{7}, 0);
    bf.decrypt(buf.data(), buf.size());
    auto end = std::find(buf.begin(), buf.end(), uint8_t{0});
    return std::string(buf.begin(), end);
}

}  // namespace

uint32_t vcodeSeed(std::string_view code) {
    uint32_t crc = 0xFFFFFFFF;
    for (char ch : code) {
        if (ch == 0) break;
        crc ^= static_cast<uint32_t>(static_cast<uint8_t>(ch)) << 24;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
        crc = ~crc;
    }
    return crc;
}

uint32_t VCodes::tocSeed() const {
    return vcodeSeed(vcode2);
}

std::optional<VCodes> readVCodes(ByteSpan executable) {
    auto vcode1 = findPeResource(executable, "V_CODE", "DATA");
    auto vcode2 = findPeResource(executable, "V_CODE2", "DATA");
    if (!vcode1 && !vcode2) return std::nullopt;

    // KEY_CODE is stored XORed with 0xCD; older games omit it and use "windmill".
    Bytes key;
    if (auto keyCode = findPeResource(executable, "KEY_CODE", "KEY")) {
        key = *keyCode;
        for (auto& b : key) b ^= 0xCD;
    } else {
        const std::string fallback = "windmill";
        key.assign(fallback.begin(), fallback.end());
    }
    Blowfish bf(key);

    VCodes codes;
    if (vcode1) codes.vcode = decryptCode(*vcode1, bf);
    if (vcode2) codes.vcode2 = decryptCode(*vcode2, bf);
    return codes;
}

std::optional<VCodes> findVCodes(const GameFolder& folder) {
    for (const auto& name : folder.list()) {
        std::string lower = toLowerAscii(name);
        bool candidate = lower.size() > 4 && (lower.ends_with(".exe") || lower.ends_with(".bin"));
        if (!candidate) continue;
        auto file = folder.open(name);
        if (!file || file->size() > 256u * 1024 * 1024) continue;
        Bytes image = file->readAll();
        try {
            if (auto codes = readVCodes(image); codes && !codes->vcode2.empty()) {
                codes->sourceFile = name;
                return codes;
            }
        } catch (const Error&) {
            // Not a PE image, or one we can't parse: keep looking.
        }
    }
    return std::nullopt;
}

}  // namespace nyan::cs2
