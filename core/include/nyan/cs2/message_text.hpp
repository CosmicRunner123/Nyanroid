#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nyan::cs2 {

enum class TextSize : int8_t { Smaller = -2, Small = -1, Normal = 0, Large = 1, Larger = 2 };

// A run of message text sharing one style. `text` is UTF-8 and may contain '\n'.
struct TextSpan {
    std::string text;
    TextSize size = TextSize::Normal;
    uint32_t color = 0;  // 0xAARRGGBB; 0 = the window's default color
    // `\f<n>` sets the size outright in pixels (px >= 0); `\f+<n>` / `\f-<n>` move it from the
    // size level by dpx pixels.
    int16_t px = -1;
    int16_t dpx = 0;
    // `\p` / `\pl` left (0), `\pc` centred (1), `\pr` right (2): how the text from here on sits in
    // each line (cs2.exe 0x695640, glyph codes 1-3).
    uint8_t align = 0;
    // The `[...]` word group the text belongs to (-1: none). The layout keeps a group on one line
    // (cs2.exe 0x694ec0); groups are numbered through a page.
    int16_t group = -1;
    // `\t<b|i...>;` (the 2.6.1.90 engines): the glyphs are asked of the font in bold (weight 700,
    // else 400) and / or italic (Yukikoi 0x5e82b0: LOGFONT lfWeight / lfItalic, then GDI's glyph).
    bool bold = false, italic = false;
};

// A message line with CatSystem2 markup resolved into styled spans.
struct MessageText {
    std::vector<TextSpan> spans;
    // `\@` at the end: the next message is appended instead of replacing this one.
    bool continues = false;
    // The style the text ends in (`tail.text` is empty) and the `\cd` default colour: a line joined
    // to this one in the same string goes on from them.
    TextSpan tail;
    uint32_t defaultColor = 0;
    // Word group ids used: groupBase (the first, the page's count before this line) up to groups.
    int16_t groupBase = 0;
    int16_t groups = 0;

    std::string plain() const;
    // Number of displayable characters (UTF-8 code points, newlines included).
    size_t length() const;
    bool empty() const { return spans.empty(); }
    // Appends another message's spans (used for `\@` continuations).
    void append(const MessageText& other);
};

// Parses message markup as cs2.exe's escape reader does (0x6937a0; numbers 0x694770: decimal or
// 0x hex, one `;` after them taken with them):
//   \n newline            \fn/\fs/\fss/\fl/\fll, \f<n> pixels, \f+<n> / \f-<n>    \c<n> colour
//   \@ continue message   \w<n> / \wf<n> / \wr reveal timing (read, not applied)   \r (ignored)
//   \pl \pc \pr alignment \s full-width space and newline   \cd<n> default colour
//   \_ a space                 \' \\ and other escapes: the character   \$<n>; character code n
//   \t<b|i...>; bold / italic (`styles`: the 2.6.1.90 engines; older ones draw the `t`)
//   [word groups] (brackets removed; a `[` inside a group and a `]` outside one are drawn); ` kept
// `raw` is Shift-JIS (escapes are only recognised outside double-byte characters), or UTF-8 if
// `utf8` (translated lines). `after`: the text this line is joined to in one string (the lines up to
// a click): parsing goes on in its style. `styles`: the engine's escape reader has `\t`.
MessageText parseMessage(std::string_view raw, bool utf8 = false, const MessageText* after = nullptr, bool styles = false);

// Cleans a speaker name or other short label (same escapes, styling dropped).
std::string cleanLabel(std::string_view raw, bool utf8 = false);

}  // namespace nyan::cs2
