#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nyan {

// Minimal XML reader for engine config files: elements and text only
// (attributes, comments, processing instructions and CDATA are skipped).
struct XmlNode {
    std::string name;
    std::string text;  // concatenated, trimmed character data
    std::vector<std::unique_ptr<XmlNode>> children;

    const XmlNode* child(std::string_view name) const;
    // Slash-separated path, e.g. "SCREEN/v_width".
    const XmlNode* find(std::string_view path) const;
    std::optional<std::string> value(std::string_view path) const;
    int intValue(std::string_view path, int fallback) const;
};

// Parses a document and returns a synthetic root whose children are the top-level elements.
std::unique_ptr<XmlNode> parseXml(std::string_view text);

}  // namespace nyan
