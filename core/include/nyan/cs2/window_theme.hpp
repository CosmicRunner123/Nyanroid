#pragma once

#include <map>
#include <string>
#include <vector>

#include "nyan/common/bytes.hpp"
#include "nyan/cs2/asset_index.hpp"

namespace nyan::cs2 {

struct IRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

// Screen layout of one message-window style (`view 1 <style>`), derived from the
// game's window theme XML, its FE script (`#OBJECT` tables, `str layout ...`) and
// the window image's frame positions, so text and faces sit where the original puts them.
struct WindowLayout {
    std::string image;            // window image, e.g. "sys_mwnd"
    std::string script;           // window FE script without extension, e.g. "meswnd"
    std::string images[3];        // img00..img02 without extension ($str900..$str902 in the script)
    std::string logScript;        // message log screen script (<log>), images in $str910..$str912
    std::string logImages[3];
    std::string selectScript;     // choice screen script (<fselect>), images in $str920..$str923
    std::string selectImages[4];
    // The window's extra buttons (<btn><exN><script>, Amakano 2's graph.fes): the window sends
    // request 100 + N and sscript opens that script over the paused story ("ex btn").
    std::string extraScripts[4];
    std::string extraImages[4][4];  // <btn><exN><img00..03>: $str910..913 for its screen
    // Scripts that run beside the window (<exN><script> under the style itself, Nekopara's
    // touch.fes): loaded and shown with it, told its MES_* messages.
    std::string sideScripts[4];
    std::string windowPlane;      // the script's top-level window plane object
    // Window-image frames drawn as the window background, back to front (e.g. band + vines).
    // The first one takes the user's window opacity.
    std::vector<int32_t> backgroundFrames;
    int32_t originX = 0, originY = 0;  // top-left of the window plane on screen
    std::string textObject = "str";    // the object MES_SETSTRING appends the message to
    IRect text;                   // message text box (screen coordinates)
    IRect name;                   // speaker name box
    // Text styling from `str layout`: font sizes for the five \fs levels, margins, colours.
    int32_t fontSizes[5] = {18, 20, 20, 25, 28};
    int32_t nameSize = 22;
    int32_t marginTop = 8, marginRight = 48;
    int32_t lineSpacing = 8;
    int32_t rubySize = 0;  // `str layout rsize`: ruby space reserved above each line
    int32_t moveY = -1;    // `str layout move <x> <y>`: gap between lines (-1: not given)
    int32_t indent = 0;    // `str layout indent 1`: lines hang under a message's opening bracket
    // Face window: `fw` image at (x, y) is anchored at (faceX + x, faceY + y), clipped to faceClip.
    int32_t faceX = 0, faceY = 0;
    IRect faceClip;
    // MES_SETFACE's `face[\0] attr <id> a b` + `base $a $b`: the face image is anchored at its frame's
    // attribute point <id> (the face's centre) rather than its base point. -1: the base point.
    int32_t faceAttr = -1;
    bool nameImage = false;  // the window shows the speaker's name as an image (`IMAGE name`), not text
    // The window has a name string (`STRING name`) for the speaker's name. The engine never draws the
    // name itself: windows without one show it their own way or not at all (Nekopara's meswnd shows
    // an image per speaker, `nameimg settbl`, nametable.csv's ids; its `str_name` lines name no
    // object and do nothing).
    bool nameText = false;
    // The face plane's mask image frame (its alpha shapes the face window; faceClip is its rect).
    std::string faceMask;
    int32_t faceMaskId = -1;
    // Novel (NVL) windows: the text object is an NVSTRING, which keeps the page's messages one under
    // another, each speaker's name right-aligned in a column of `DIV` pixels at the left of the text
    // box and the text after it, `marginLeft` further on.
    bool novel = false;
    int32_t nameColumn = 0;
    int32_t marginLeft = 0;
};

struct WindowTheme {
    std::map<std::string, WindowLayout> styles;  // by style name ("meswnd", "meswnd02", ...)
    const WindowLayout* find(const std::string& style) const;
};

// Decompresses an FE script (.fes) to its Shift-JIS source text.
std::string decodeFes(ByteSpan file);

// Loads the message window theme (meswnd*.xml) for a game with the given virtual screen size.
WindowTheme loadWindowTheme(const AssetIndex& assets, int32_t screenWidth, int32_t screenHeight);

}  // namespace nyan::cs2
