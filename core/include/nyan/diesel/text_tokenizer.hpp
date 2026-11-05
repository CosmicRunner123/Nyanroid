#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nyan::diesel {

// A piece of a line of message text as the engine's tokenizer gives it: plain text, or a tag
// (<RUBY text='...'>, </RUBY>, <TAG=value>) with its attributes.
struct TextToken {
    std::string name;  // the tag's name, ASCII letters upper-cased ("RUBY", "/RUBY"); empty for text
    std::vector<std::pair<std::string, std::string>> attributes;  // names upper-cased, in order
    std::string text;  // the text (a line's last text keeps its '\n'), or a tag's "=value"
};
using TextLine = std::vector<TextToken>;

// Diesel::CTextTokenizer's parse (muramasa_en.exe 0x4e5b30): first "#{ruby}base#" becomes
// "<RUBY text='ruby'>base</RUBY>" (nullopt if a '}' or the closing '#' is missing); then a state
// machine reads text and tags into lines of tokens. '&' takes the next byte literally (kept as "&x"
// unless `dropAmpersands`); "//" comments out the rest of the line; a '<' inside a tag drops the line
// (what was read of it too); runs of empty lines collapse to one and leading ones go. The input's last byte is
// taken as text, as the exe takes it. UTF-8 sequences pass through whole.
std::optional<std::vector<TextLine>> tokenizeText(std::string_view text, bool dropAmpersands);

// The UTF-16 length of UTF-8 text as MultiByteToWideChar gives it (2 units beyond the BMP; a unit
// for each byte of a broken sequence), not counting tabs, CRs and LFs: what CountTextLength counts.
int countUtf16Units(std::string_view utf8);

}  // namespace nyan::diesel
