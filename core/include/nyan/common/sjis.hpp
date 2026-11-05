#pragma once

#include <string>
#include <string_view>

namespace nyan {

// Converts Shift-JIS (CP932) text to UTF-8. ASCII bytes pass through unchanged
// (so '\\' stays a backslash rather than becoming a yen sign). Unmappable
// characters become U+FFFD.
std::string sjisToUtf8(std::string_view sjis);

// Converts UTF-8 to Shift-JIS (CP932), for strings handed to game scripts. Characters with no
// Shift-JIS form become '?'.
std::string utf8ToSjis(std::string_view utf8);

// Whether the bytes are well-formed UTF-8.
bool isValidUtf8(std::string_view s);

// Decodes a game text file to UTF-8: an XML encoding declaration decides (utf-8 or Shift-JIS);
// otherwise a UTF-8 BOM or valid multi-byte UTF-8 means UTF-8, else Shift-JIS.
std::string decodeGameText(std::string_view bytes);

}  // namespace nyan
