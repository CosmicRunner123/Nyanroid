#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

enum class CstLineType : uint8_t {
    Input = 0x02,       // wait for input after the message
    Page = 0x03,        // novel-mode page break + wait for input
    Message = 0x20,     // message text
    Name = 0x21,        // speaker name for the next message
    Command = 0x30,     // any other command
    ScriptName = 0xF0,  // debug info: source file name
    LineNumber = 0xF1,  // debug info: source line number
};

struct CstLine {
    CstLineType type;
    std::string text;  // raw script encoding (Shift-JIS, or UTF-8 in some translations)
    bool utf8 = false;  // replaced from the scene's translation table (.cstl): UTF-8
    // A name line replaced from the table: the scene's own text (UTF-8, markup kept). sscript looks
    // the name table up with it (native 1037: the line with no language, 0x72b920).
    std::string own;
    // A message's first text line: the message in the sub language (UTF-8 markup), when one is set.
    std::string sub;
};

// A compiled CatScene (.cst) script: a flat list of typed lines.
class CstScene {
public:
    static CstScene parse(ByteSpan file);

    const std::vector<CstLine>& lines() const { return lines_; }
    std::vector<CstLine>& lines() { return lines_; }
    // Line indices directly after each input/page wait (used for skipping/backlog).
    const std::vector<uint32_t>& inputIndices() const { return inputIndices_; }

private:
    std::vector<CstLine> lines_;
    std::vector<uint32_t> inputIndices_;
};

const char* toString(CstLineType type);

}  // namespace nyan::cs2
