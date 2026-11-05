#pragma once

#include <optional>
#include <string>

#include "nyan/common/bytes.hpp"
#include "nyan/common/file_source.hpp"

namespace nyan::cs2 {

// Decrypted codes embedded in a CatSystem2 executable's resources.
struct VCodes {
    std::string vcode;   // V_CODE: game validation
    std::string vcode2;  // V_CODE2: KIF archive table-of-contents key
    std::string sourceFile;  // executable the codes were read from

    // Seed used to unobfuscate KIF entry file names.
    uint32_t tocSeed() const;
};

// Extracts V_CODE/V_CODE2 from an executable image. Returns nullopt if the
// image has no V_CODE resources (e.g. a launcher rather than the engine).
std::optional<VCodes> readVCodes(ByteSpan executable);

// Scans a game folder's .exe/.bin files for the CatSystem2 executable's codes.
std::optional<VCodes> findVCodes(const GameFolder& folder);

// CRC-32 variant CatSystem2 uses to turn a V_CODE string into a 32-bit seed.
uint32_t vcodeSeed(std::string_view code);

}  // namespace nyan::cs2
