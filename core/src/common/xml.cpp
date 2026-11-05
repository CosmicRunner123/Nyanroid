#include "nyan/common/xml.hpp"

#include <cstdlib>

namespace nyan {
namespace {

std::string trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

}  // namespace

const XmlNode* XmlNode::child(std::string_view n) const {
    for (const auto& c : children) {
        if (c->name == n) return c.get();
    }
    return nullptr;
}

const XmlNode* XmlNode::find(std::string_view path) const {
    const XmlNode* node = this;
    while (node && !path.empty()) {
        size_t slash = path.find('/');
        node = node->child(path.substr(0, slash));
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
    }
    return node;
}

std::optional<std::string> XmlNode::value(std::string_view path) const {
    const XmlNode* n = find(path);
    if (!n) return std::nullopt;
    return n->text;
}

int XmlNode::intValue(std::string_view path, int fallback) const {
    auto v = value(path);
    if (!v || v->empty()) return fallback;
    return static_cast<int>(std::strtol(v->c_str(), nullptr, 0));
}

std::unique_ptr<XmlNode> parseXml(std::string_view s) {
    auto root = std::make_unique<XmlNode>();
    std::vector<XmlNode*> stack{root.get()};
    std::string pendingText;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            size_t next = s.find('<', i);
            if (next == std::string_view::npos) next = s.size();
            pendingText.append(s.substr(i, next - i));
            i = next;
            continue;
        }
        if (s.substr(i, 4) == "<!--") {
            size_t end = s.find("-->", i);
            i = end == std::string_view::npos ? s.size() : end + 3;
            continue;
        }
        if (s.substr(i, 2) == "<?" || s.substr(i, 2) == "<!") {
            size_t end = s.find('>', i);
            i = end == std::string_view::npos ? s.size() : end + 1;
            continue;
        }
        size_t end = s.find('>', i);
        if (end == std::string_view::npos) break;
        std::string_view tag = s.substr(i + 1, end - i - 1);
        i = end + 1;
        if (!tag.empty() && tag[0] == '/') {
            if (stack.size() > 1) {
                stack.back()->text += trim(pendingText);
                stack.pop_back();
            }
            pendingText.clear();
            continue;
        }
        bool selfClosing = !tag.empty() && tag.back() == '/';
        if (selfClosing) tag.remove_suffix(1);
        size_t nameEnd = tag.find_first_of(" \t\r\n");
        auto node = std::make_unique<XmlNode>();
        node->name = std::string(tag.substr(0, nameEnd));
        XmlNode* raw = node.get();
        stack.back()->text += trim(pendingText);
        pendingText.clear();
        stack.back()->children.push_back(std::move(node));
        if (!selfClosing) stack.push_back(raw);
    }
    return root;
}

}  // namespace nyan
