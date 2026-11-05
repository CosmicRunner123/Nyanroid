#pragma once

#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"

namespace nyan::cs2 {

// Names a TrueType/OpenType font (or one face of a collection) is known by: family, full name and
// "family subfamily", in every language its name table has (UTF-8). Used to find the face that
// startup.xml's <FONT><face> asks for among the fonts a game ships.
std::vector<std::string> fontFaceNames(const Bytes& font, int face);
// Number of faces (1 for a plain font, n for a .ttc collection; 0 if not a font).
int fontFaceCount(const Bytes& font);
// True if the face has outlines our rasteriser draws: TrueType `glyf`, CFF, or CFF2 (a variable font such
// as recent Android system CJK fonts, drawn at its default instance).
bool fontRenderable(const Bytes& font, int face);
// A face as Windows' font enumeration reports it (EnumFontFamiliesEx's LOGFONT, which the engine's
// font list reads): the family name (name ID 1) in Japanese, else US English, else any; the code
// pages it covers (OS/2 ulCodePageRange1: bit 0 Latin 1252, 17 Japanese 932, 18 Simplified Chinese
// 936, 19 Korean 949, 20 Traditional Chinese 950; without the field, 932 when it has a Japanese
// name); fixed pitch (post isFixedPitch, or PANOSE proportion 9 "monospaced").
// Also the face's style, which Windows' font mapper weighs when several faces share a family
// (meiryo.ttc's and meiryob.ttc's メイリオ): OS/2 usWeightClass (400 normal, 700 bold) and
// fsSelection's italic and bold bits, else head macStyle's (`bold` is what Windows calls a bold
// face, NTM_BOLD: one it does not embolden further when bold is asked for).
struct FontFamilyInfo {
    std::string family;
    uint32_t codePages = 0;
    bool fixedPitch = false;
    int weight = 400;
    bool italic = false;
    bool bold = false;
};
FontFamilyInfo fontFamilyInfo(const Bytes& font, int face);
// The family names alone (name IDs 1 and 16, every language), a subset of fontFaceNames.
std::vector<std::string> fontFamilyNames(const Bytes& font, int face);
// Case- and space-insensitive comparison of font names ("MS Gothic" == "msgothic").
bool fontNameEquals(const std::string& a, const std::string& b);

}  // namespace nyan::cs2
