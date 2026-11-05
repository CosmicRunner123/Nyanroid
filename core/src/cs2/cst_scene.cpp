#include "nyan/cs2/cst_scene.hpp"

#include "nyan/common/zlib_util.hpp"

namespace nyan::cs2 {

const char* toString(CstLineType type) {
    switch (type) {
    case CstLineType::Input: return "input";
    case CstLineType::Page: return "page";
    case CstLineType::Message: return "message";
    case CstLineType::Name: return "name";
    case CstLineType::Command: return "command";
    case CstLineType::ScriptName: return "scriptname";
    case CstLineType::LineNumber: return "linenumber";
    }
    return "unknown";
}

CstScene CstScene::parse(ByteSpan file) {
    ByteReader header(file, "CST header");
    if (header.fixedString(8) != "CatScene") fail("Not a CatScene (.cst) file");
    uint32_t compressed = header.u32();
    uint32_t decompressed = header.u32();

    Bytes script;
    if (compressed == 0) {
        auto raw = header.span(decompressed);
        script.assign(raw.begin(), raw.end());
    } else {
        script = zlibInflate(header.span(compressed), decompressed);
    }

    ByteReader r(script, "CST script");
    r.skip(4);  // script length
    uint32_t inputCount = r.u32();
    uint32_t offsetTable = r.u32();
    uint32_t stringTable = r.u32();
    constexpr size_t kHeader = 16;
    if (stringTable < offsetTable) fail("CST string table precedes offset table");

    CstScene scene;
    for (uint32_t i = 0; i < inputCount; ++i) {
        r.skip(4);  // offset to next
        scene.inputIndices_.push_back(r.u32());
    }

    uint32_t count = (stringTable - offsetTable) / 4;
    scene.lines_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        r.seek(kHeader + offsetTable + i * 4);
        uint32_t offset = r.u32();
        r.seek(kHeader + stringTable + offset);
        if (r.u8() != 0x01) fail("CST line " + std::to_string(i) + " has a bad prefix byte");
        auto type = static_cast<CstLineType>(r.u8());
        std::string text;
        for (uint8_t c; (c = r.u8()) != 0;) text += static_cast<char>(c);
        scene.lines_.push_back({type, std::move(text)});
    }
    return scene;
}

}  // namespace nyan::cs2
