#include "nyan/diesel/text_tokenizer.hpp"

#include <cctype>
#include <cstdint>

namespace nyan::diesel {

namespace {

// The parser's states (the values on its stack).
enum State : int {
    Text = 0,
    Quoted = 1,     // inside '...'
    Comment = 2,    // after "//", to the end of the line
    InTag = 3,      // after a tag's name
    TagName = 4,    // after '<'
    TagValue = 5,   // after "<NAME="
    AttrName = 7,
    AttrValue = 8,  // after "name="
};

// The length of the UTF-8 sequence a lead byte starts as the exe reckons it (0: not a lead byte).
int leadLength(uint8_t c) {
    uint8_t n = static_cast<uint8_t>(~c);
    if ((n & 0xc0) != 0) return 0;
    if ((n & 0x20) != 0) return 2;
    if ((n & 0x10) != 0) return 3;
    if ((n & 0x08) != 0) return 4;
    if ((n & 0x04) != 0) return 5;
    if ((n & 0x02) != 0) return 6;
    return 0;  // 0xfe, 0xff
}

// 0x4e5a60: ASCII letters upper-cased, UTF-8 sequences skipped.
void upperCase(std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (int n = leadLength(c)) {
            i += static_cast<size_t>(n - 1);
            continue;
        }
        if (c >= 'a' && c <= 'z') s[i] = static_cast<char>(c - 'a' + 'A');
    }
}

struct Parser {
    std::string in;
    bool dropAmpersands = false;
    std::vector<int> stack{Text};
    std::vector<TextLine> lines;
    TextLine line;
    TextToken tok;
    std::string work, attrName, attrValue;
    bool lastLineEmpty = true;  // local_60: leading empty lines are not kept
    bool tagError = false;      // local_14: a '<' inside a tag; the rest of the line is skipped

    // The stack's top; an empty stack (a stray '>' pops the last state, where the exe reads before
    // its buffer) reads as text.
    int top() const { return stack.empty() ? Text : stack.back(); }
    void pop() {
        if (!stack.empty()) stack.pop_back();
    }
    // 0x4e6aa0 with the state it is given: the work buffer becomes the token's text in the text
    // state; the token joins the line if it has anything.
    void flush(int state) {
        if (!work.empty() && state == Text) tok.text = work;
        upperCase(tok.name);
        if (!tok.name.empty() || !tok.attributes.empty() || !tok.text.empty() || (!work.empty() && state == Text))
            line.push_back(tok);
        tok = TextToken{};
        work.clear();
    }
    void commitAttribute() {
        upperCase(attrName);
        attrValue = work;
        tok.attributes.emplace_back(attrName, attrValue);
        attrName.clear();
        attrValue.clear();
    }
    void endLine(char c) {
        int prev = top();
        if (prev == Comment) pop();
        if (!tagError) {
            if (prev == Comment) {
                if (!line.empty()) {
                    lines.push_back(line);
                    lastLineEmpty = false;
                }
            } else {
                work += c;
                flush(top());
                if (line.empty()) {
                    if (!lastLineEmpty) lines.push_back(line);
                    lastLineEmpty = true;
                } else {
                    lines.push_back(line);
                    lastLineEmpty = false;
                }
            }
        } else {
            stack.assign(1, Text);
            tagError = false;
        }
        line.clear();
        tok = TextToken{};
        work.clear();
    }

    void run() {
        const size_t len = in.size();
        for (size_t i = 0; i < len; ++i) {
            char c = in[i];
            if (!(stack.empty() || (!tagError && top() != Comment) || c == '\n')) continue;
            uint8_t u = static_cast<uint8_t>(c);
            if (int n = leadLength(u)) {
                for (int k = 0; k < n && i + static_cast<size_t>(k) < len; ++k) work += in[i + static_cast<size_t>(k)];
                i += static_cast<size_t>(n - 1);
                continue;
            }
            if (c == '/' && !work.empty() && work.back() == '/') {
                work.pop_back();
                flush(top());
                stack.push_back(Comment);
                continue;
            }
            if (c == '&' && i + 1 < len) {
                if (!dropAmpersands) work += c;
                work += in[i + 1];
                ++i;
                continue;
            }
            if (top() == Quoted) {
                if (c == '\'') {
                    pop();
                    if (top() != AttrValue && top() != TagValue) work.clear();
                    continue;
                }
                if (c != '\0' && i != len - 1 && c != '\n') {
                    work += c;
                    continue;
                }
                pop();  // unterminated: the byte is read in the state below
            }
            if (c == '<') {
                if (top() != Text) tagError = true;
                flush(top());
                stack.push_back(TagName);
                continue;
            }
            if (c == '>') {
                if (top() == AttrValue) {
                    commitAttribute();
                    pop();
                    work.clear();
                }
                if (top() == TagName) {
                    tok.name = work;
                    work.clear();
                } else if (top() == TagValue) {
                    tok.text = work;
                    work.clear();
                }
                int popped = top();
                pop();
                flush(popped);  // the exe passes the slot just popped
                continue;
            }
            if (c == '\r') continue;
            if (c == '\n' || c == '\0' || i == len - 1) {
                endLine(c);
                continue;
            }
            if (c == '\t' || c == ' ') {
                int s = top();
                if (s == TagName) {
                    tok.name = work;
                    work.clear();
                    stack.back() = InTag;
                    stack.push_back(AttrName);
                } else if (s == InTag) {
                    stack.push_back(AttrName);
                } else if (c == ' ' && s == AttrValue) {
                    commitAttribute();
                    pop();
                    work.clear();
                    if (i + 1 < len && std::isalpha(static_cast<unsigned char>(in[i + 1]))) stack.push_back(AttrName);
                } else if (c == ' ' && s == TagValue) {
                    tok.text = work;
                    pop();
                    work.clear();
                    if (i + 1 < len && std::isalpha(static_cast<unsigned char>(in[i + 1]))) stack.push_back(AttrName);
                } else if (c == ' ' && s == Text) {
                    work += c;
                }
                continue;
            }
            if (c == '\'' && (top() == AttrValue || top() == TagValue)) {
                stack.push_back(Quoted);
                continue;
            }
            if (c == '=') {
                if (top() == AttrName) {
                    attrName = work;
                    stack.back() = AttrValue;
                    work.clear();
                    continue;
                }
                if (top() == TagName) {
                    tok.name = work;
                    work.clear();
                    stack.back() = TagValue;
                    continue;
                }
            }
            work += c;
        }
        if (!work.empty() || !line.empty()) {
            flush(top());
            if (!line.empty() || !lastLineEmpty) lines.push_back(line);
        }
    }
};

}  // namespace

std::optional<std::vector<TextLine>> tokenizeText(std::string_view text, bool dropAmpersands) {
    Parser p;
    p.in.assign(text.begin(), text.end());
    p.dropAmpersands = dropAmpersands;
    // "#{ruby}base#" -> "<RUBY text='ruby'>base</RUBY>".
    size_t at = 0;
    while (at < p.in.size()) {
        size_t open = p.in.find("#{", at);
        if (open == std::string::npos) break;
        size_t close = p.in.find('}', open + 2);
        if (close == std::string::npos) return std::nullopt;
        size_t end = p.in.find('#', close + 1);
        if (end == std::string::npos) return std::nullopt;
        std::string base = p.in.substr(close + 1, end - close - 1);
        std::string ruby = p.in.substr(open + 2, close - open - 2);
        p.in.erase(open, end - open + 1);
        std::string repl = "<RUBY text='" + ruby + "'>" + base + "</RUBY>";
        p.in.insert(open, repl);
        at = open + repl.size();
    }
    p.run();
    return std::move(p.lines);
}

int countUtf16Units(std::string_view s) {
    int units = 0;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            if (c != '\t' && c != '\r' && c != '\n') ++units;
            ++i;
            continue;
        }
        int n = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
        bool ok = n > 0 && i + static_cast<size_t>(n) <= s.size();
        uint32_t cp = n == 2 ? (c & 0x1fu) : n == 3 ? (c & 0x0fu) : (c & 0x07u);
        for (int k = 1; ok && k < n; ++k) {
            uint8_t t = static_cast<uint8_t>(s[i + static_cast<size_t>(k)]);
            if ((t & 0xc0) != 0x80) ok = false;
            cp = (cp << 6) | (t & 0x3fu);
        }
        if (ok && ((n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp < 0xe000))) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff))))
            ok = false;
        if (!ok) {
            ++units;  // U+FFFD for the byte
            ++i;
            continue;
        }
        units += cp >= 0x10000 ? 2 : 1;
        i += static_cast<size_t>(n);
    }
    return units;
}

}  // namespace nyan::diesel
