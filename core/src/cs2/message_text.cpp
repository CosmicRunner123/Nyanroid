#include "nyan/cs2/message_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "nyan/common/sjis.hpp"

namespace nyan::cs2 {
namespace {

bool isLeadByte(uint8_t b) {
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}

size_t utf8Length(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// cs2.exe 0x694770: a number after an escape, `0x` / `0X` hex up to the first non-hex character or
// decimal digits, and one `;` right after it. Returns the characters read (0: no number).
size_t readNumber(std::string_view raw, size_t i, uint32_t& value) {
    value = 0;
    size_t j = i;
    if (j + 1 < raw.size() && raw[j] == '0' && (raw[j + 1] == 'x' || raw[j + 1] == 'X')) {
        j += 2;
        while (j < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[j]))) {
            const char c = raw[j++];
            value = value * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (c | 0x20) - 'a' + 10);
        }
    } else {
        while (j < raw.size() && std::isdigit(static_cast<unsigned char>(raw[j]))) value = value * 10 + static_cast<uint32_t>(raw[j++] - '0');
        if (j == i) return 0;
    }
    if (j < raw.size() && raw[j] == ';') ++j;
    return j - i;
}

// cs2.exe 0x6a48e0 / amakanoPE.exe 0x5911e0, the engine's string-to-number: a sign, then `0x` and hex
// digits, or decimal digits (a '.' right after them gives 0); anything else is 0.
int32_t engineNumber(std::string_view s) {
    size_t k = 0;
    bool negative = false;
    if (k < s.size() && (s[k] == '-' || s[k] == '+')) negative = s[k++] == '-';
    if (k >= s.size() || s[k] < '0' || s[k] > '9') return 0;
    int32_t v = 0;
    if (s[k] == '0' && k + 1 < s.size() && (s[k + 1] == 'x' || s[k + 1] == 'X')) {
        for (k += 2; k < s.size() && std::isxdigit(static_cast<unsigned char>(s[k])); ++k) {
            const char c = s[k];
            v = v * 16 + (std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (c | 0x20) - 'a' + 10);
        }
    } else {
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') v = v * 10 + (s[k++] - '0');
        if (k < s.size() && s[k] == '.') return 0;
    }
    return negative ? -v : v;
}

}  // namespace

std::string MessageText::plain() const {
    std::string out;
    for (const auto& s : spans) out += s.text;
    return out;
}

size_t MessageText::length() const {
    size_t n = 0;
    for (const auto& s : spans) n += utf8Length(s.text);
    return n;
}

void MessageText::append(const MessageText& other) {
    // The other line's groups go on from this page's: renumbered when it was parsed on its own.
    const int16_t shift = static_cast<int16_t>(groups - other.groupBase);
    const size_t from = spans.size();
    spans.insert(spans.end(), other.spans.begin(), other.spans.end());
    for (size_t k = from; k < spans.size(); ++k) {
        if (spans[k].group >= 0) spans[k].group = static_cast<int16_t>(spans[k].group + shift);
    }
    groups = static_cast<int16_t>(std::max<int>(groups, other.groups + shift));
    continues = other.continues;
    tail = other.tail;
    if (tail.group >= 0) tail.group = static_cast<int16_t>(tail.group + shift);
    defaultColor = other.defaultColor;
}

MessageText parseMessage(std::string_view raw, bool utf8, const MessageText* after, bool styles) {
    MessageText msg;
    const TextSpan start = after ? after->tail : TextSpan{};
    TextSize size = start.size;
    int16_t px = start.px, dpx = start.dpx;
    uint32_t color = start.color, defaultColor = after ? after->defaultColor : 0;
    uint8_t align = start.align;
    bool bold = start.bold, italic = start.italic;
    // Word groups (cs2.exe 0x696010, amakanoPE.exe 0x586970): `[` opens one, `]` closes it; a group
    // goes on into a joined line that opens inside it.
    int16_t group = start.group;
    msg.groupBase = after ? after->groups : 0;
    int16_t nextGroup = msg.groupBase;
    std::string pending;  // Shift-JIS bytes for the current span

    // Adds UTF-8 text in the current style.
    auto push = [&](std::string text) {
        if (text.empty()) return;
        const TextSpan* last = msg.spans.empty() ? nullptr : &msg.spans.back();
        if (last && last->size == size && last->color == color && last->px == px && last->dpx == dpx && last->align == align &&
            last->group == group && last->bold == bold && last->italic == italic) {
            msg.spans.back().text += text;
        } else {
            msg.spans.push_back({std::move(text), size, color, px, dpx, align, group, bold, italic});
        }
    };
    auto flush = [&] {
        if (pending.empty()) return;
        std::string text = utf8 ? std::move(pending) : sjisToUtf8(pending);
        pending.clear();
        push(std::move(text));
    };
    // A size escape sets the size level (and drops pixel sizes); a colour escape only the colour.
    auto setSize = [&](TextSize s) {
        if (s == size && px < 0 && dpx == 0) return;
        flush();
        size = s;
        px = -1;
        dpx = 0;
    };
    auto setColor = [&](uint32_t c) {
        if (c == color) return;
        flush();
        color = c;
    };
    auto setPixels = [&](int16_t abs, int16_t rel) {
        flush();
        px = abs;
        dpx = rel;
    };
    // An escape's colour is the number with red in its low byte (0x..BBGGRR): the message glyphs
    // pass its bytes low first to the font image (cs2.exe 0x697042), which writes them reversed into
    // a BGRA texture (0x691d90); the alpha byte is never used (glyph coverage is the alpha).
    auto escapeColor = [](uint32_t v) { return 0xFF000000u | (v & 0xFFu) << 16 | (v & 0xFF00u) | ((v >> 16) & 0xFFu); };
    size_t i = 0;
    auto next = [&]() -> char { return i < raw.size() ? raw[i] : '\0'; };
    while (i < raw.size()) {
        auto b = static_cast<uint8_t>(raw[i]);
        if (!utf8 && isLeadByte(b) && i + 1 < raw.size()) {
            pending.append(raw.substr(i, 2));
            i += 2;
            msg.continues = false;
            continue;
        }
        // A `[` outside a group opens one, a `]` inside one closes it; the other way round they are
        // drawn like any character.
        if ((b == '[' && group < 0) || (b == ']' && group >= 0)) {
            flush();
            group = b == '[' ? nextGroup++ : -1;
            ++i;
            continue;
        }
        if (b != '\\' || i + 1 >= raw.size()) {
            pending += static_cast<char>(b);
            ++i;
            if (b != ' ') msg.continues = false;
            continue;
        }

        char e = raw[i + 1];
        i += 2;
        uint32_t v = 0;
        switch (e) {
        case 'n':
        case 'N':
            pending += '\n';
            msg.continues = false;
            break;
        case '_':
            // A space. The escape reader would draw `_` itself, but the text reaches it through the
            // string object's text processing, which turns `\_` into a space first: Himawari's names
            // (`Youichi\_`) show no underscore on PC, nor does the save screen's `No.%02d\_:\_`.
            pending += ' ';
            break;
        case '@':
            // `\@` (code 7) at the message's end: the next message goes on on this page. Text after
            // it leaves the page to clear at the click as usual: Amakano 1's and REBELLIONS'
            // translations open their narration with `\fs\@` and show each line on a page of its own
            // on PC. Text, a `\n` or a double-byte character after it cancels it; spaces and the word
            // brackets don't (Hatsumira's `[\fl"Hurk!"\@]` keeps its closing bracket).
            msg.continues = true;
            break;
        case '$': {
            // `\$<n>;` (cs2.exe 0x6937a0 -> 0x6948d0 / 0x6a48e0; amakanoPE.exe 0x583dc0 -> 0x584e90 /
            // 0x5911e0): what runs up to `;` (or the end) read as a number, the glyph marked as a
            // wide character, which the font draws with GetGlyphOutlineW (amakanoPE.exe 0x581b60):
            // a UTF-16 code. Amakano 1's 2025 translation: `\$39;` ', `\$8212;` —, `\$233;` é.
            // Codes below 0x20 (and surrogates) draw nothing here.
            const size_t end = raw.find(';', i);
            const int32_t code = engineNumber(raw.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i));
            i = end == std::string_view::npos ? raw.size() : end + 1;
            const uint32_t u = static_cast<uint32_t>(code) & 0xFFFF;
            if (u >= 0x20 && (u < 0xD800 || u > 0xDFFF)) {
                flush();
                std::string ch;
                if (u < 0x80) {
                    ch += static_cast<char>(u);
                } else if (u < 0x800) {
                    ch += static_cast<char>(0xC0 | (u >> 6));
                    ch += static_cast<char>(0x80 | (u & 0x3F));
                } else {
                    ch += static_cast<char>(0xE0 | (u >> 12));
                    ch += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                    ch += static_cast<char>(0x80 | (u & 0x3F));
                }
                push(std::move(ch));
                msg.continues = false;
            }
            break;
        }
        case 's':
        case 'S':
            // A full-width space, then a line break (glyphs 0x8140 and 0).
            pending += utf8 ? "\xE3\x80\x80" : "\x81\x40";
            pending += '\n';
            break;
        case 'r':
        case 'R': break;
        case 'p':
        case 'P': {
            // \p / \pl left, \pc centre, \pr right (glyph codes 1-3); another character is read again.
            const char k = static_cast<char>(next() | 0x20);
            flush();
            align = k == 'c' ? 1 : k == 'r' ? 2 : 0;
            if (k == 'c' || k == 'l' || k == 'r') ++i;
            break;
        }
        case 'w':
        case 'W': {
            // \w<n>: the reveal clock to glyph n; \wf<n>: n tenths of strframe later; \wr: from 0.
            const char k = next();
            if (k == 'f' || k == 'F') i += 1 + readNumber(raw, i + 1, v);
            else if (k == 'r' || k == 'R') ++i;
            else i += readNumber(raw, i, v);
            break;
        }
        case 'f':
        case 'F': {
            const char k = next();
            if (k == 'n' || k == 'N') {
                ++i;
                setSize(TextSize::Normal);
            } else if (k == 's' || k == 'S' || k == 'l' || k == 'L') {
                ++i;
                const char k2 = next();
                const bool doubled = (k2 | 0x20) == (k | 0x20);
                if (doubled || k2 == ';') ++i;
                if (doubled && next() == ';') ++i;
                if ((k | 0x20) == 's') setSize(doubled ? TextSize::Smaller : TextSize::Small);
                else setSize(doubled ? TextSize::Larger : TextSize::Large);
            } else if (k == '+' || k == '-') {
                const size_t n = readNumber(raw, i + 1, v);
                if (n > 0) {
                    i += 1 + n;
                    const int16_t d = static_cast<int16_t>(k == '+' ? static_cast<int32_t>(v) : -static_cast<int32_t>(v));
                    if (px >= 0) setPixels(static_cast<int16_t>(std::max(1, px + d)), 0);
                    else setPixels(-1, static_cast<int16_t>(dpx + d));
                } else {
                    ++i;  // a sign without a number: the normal size
                    setSize(TextSize::Normal);
                }
            } else if (k == 'r' || k == 'R') {
                ++i;
                i += readNumber(raw, i, v);  // the ruby size
            } else if (const size_t n = readNumber(raw, i, v); n > 0) {
                i += n;
                setPixels(static_cast<int16_t>(std::max<uint32_t>(1, std::min<uint32_t>(v, 1000))), 0);
            } else {
                setSize(TextSize::Normal);  // a bare \f
                if (next() == ';') ++i;
            }
            break;
        }
        case 'c':
        case 'C': {
            // \c<n> the colour, a bare \c (or one without a number) back to the default; \cd<n> the
            // default, \cr<n> / \cs<n> the ruby and shadow colours (not drawn here).
            const char k = next();
            if (k == 'd' || k == 'D') {
                ++i;
                const size_t n = readNumber(raw, i, v);
                i += n;
                defaultColor = n > 0 ? escapeColor(v) : 0;
            } else if (k == 'r' || k == 'R' || k == 's' || k == 'S') {
                ++i;
                i += readNumber(raw, i, v);
            } else if (const size_t n = readNumber(raw, i, v); n > 0) {
                i += n;
                setColor(escapeColor(v));
            } else {
                if (k == ';') ++i;
                setColor(defaultColor);
            }
            break;
        }
        case 't':
        case 'T':
            if (!styles) {
                pending += e;  // Nekopara's (2.6.1.76) and Amakano 1's (2.6.1.73) readers draw it
                break;
            }
            // The text style (Yukikoi 0x5eaac0 / 0x5f0b30; Amakano 2, Cyanotype and Happiness 2 the
            // same): bold and italic cleared, then each `b` / `i` sets one, up to a `;`, which is
            // taken; another character ends the list and is read as text. Yukikoi's translation:
            // `\ti;look\t;`.
            flush();
            bold = italic = false;
            for (;;) {
                const char k = static_cast<char>(next() | 0x20);
                if (k == ';') {
                    ++i;
                    break;
                }
                if (k == 'b') bold = true;
                else if (k == 'i') italic = true;
                else break;
                ++i;
            }
            break;
        default:
            // Any other escape is the character itself (\' \\ \m \]...): cs2.exe draws it.
            pending += e;
            break;
        }
    }
    flush();
    // Drop a trailing empty run left by style changes.
    while (!msg.spans.empty() && msg.spans.back().text.empty()) msg.spans.pop_back();
    msg.tail = {std::string(), size, color, px, dpx, align, group, bold, italic};
    msg.groups = nextGroup;
    msg.defaultColor = defaultColor;
    return msg;
}

std::string cleanLabel(std::string_view raw, bool utf8) {
    return parseMessage(raw, utf8).plain();
}

}  // namespace nyan::cs2
