#include "nyan/cs2/window_theme.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "nyan/common/sjis.hpp"
#include "nyan/common/xml.hpp"
#include "nyan/common/zlib_util.hpp"
#include "nyan/cs2/fes_script.hpp"
#include "nyan/cs2/hg_image.hpp"

namespace nyan::cs2 {
namespace {

// `#OBJECT` tables of an FE script: object name -> column name -> value text.
using ObjectTable = std::map<std::string, std::map<std::string, std::string>>;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string t; in >> t;) out.push_back(t);
    return out;
}

ObjectTable parseObjects(const std::string& text, std::vector<std::string>& lines) {
    ObjectTable table;
    std::istringstream in(text);
    std::vector<std::string> columns;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (line.starts_with("#OBJECT")) {
            columns = split(line);
            columns.erase(columns.begin());
            continue;
        }
        if (line.starts_with("#")) {
            columns.clear();
            continue;
        }
        if (columns.empty()) continue;
        auto tok = split(line);
        if (tok.empty()) continue;
        auto& row = table[tok[0]];
        for (size_t i = 0; i < columns.size() && i + 1 < tok.size(); ++i) row[columns[i]] = tok[i + 1];
    }
    return table;
}

bool pair(const std::string& v, int32_t& a, int32_t& b) {
    size_t comma = v.find(',');
    if (comma == std::string::npos) return false;
    a = std::atoi(v.c_str());
    b = std::atoi(v.c_str() + comma + 1);
    return true;
}

// Frame offset/size by id; image frames are looked up by `$str900:<id>` references.
struct FrameInfo {
    int32_t x = 0, y = 0, w = 0, h = 0;
};
std::map<int32_t, FrameInfo> frameMap(const AssetIndex& assets, const std::string& image) {
    std::map<int32_t, FrameInfo> out;
    auto ref = assets.find(image + ".hg3");
    if (!ref) return out;
    try {
        HgImage img = HgImage::parse(ref->archive->read(*ref->entry));
        for (const auto& f : img.frames()) {
            out.emplace(static_cast<int32_t>(f.id), FrameInfo{f.offsetX, f.offsetY, static_cast<int32_t>(f.width),
                                                              static_cast<int32_t>(f.height)});
        }
        // Frame 0 of the window image is the full canvas; record its total size.
        const auto& f0 = img.frames().front();
        out[-1] = FrameInfo{0, 0, static_cast<int32_t>(f0.totalWidth), static_cast<int32_t>(f0.totalHeight)};
    } catch (const Error&) {
    }
    return out;
}

std::string stripExt(std::string s) {
    auto dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

// Object types from the `#DEFINE` section ("PLANE pl", "IMAGE bg[2]", "STRING str", ...),
// keyed by base name without the array suffix.
std::map<std::string, std::string> defineTypes(const std::vector<std::string>& lines) {
    std::map<std::string, std::string> types;
    bool inDefine = false;
    for (const auto& line : lines) {
        if (line.starts_with("#")) {
            inDefine = line.starts_with("#DEFINE");
            continue;
        }
        if (!inDefine) continue;
        auto tok = split(line);
        if (tok.size() >= 2) types[tok[1].substr(0, tok[1].find('['))] = FesScript::baseType(tok[0]);
    }
    return types;
}

std::string baseName(const std::string& obj) {
    return obj.substr(0, obj.find('['));
}

std::optional<WindowLayout> buildLayout(const AssetIndex& assets, const std::string& script,
                                        const std::array<std::string, 4>& images, int32_t screenW, int32_t screenH) {
    const std::string& image = images[0];
    auto fesRef = assets.find(script);
    if (!fesRef || image.empty()) return std::nullopt;
    std::string text = decodeFes(fesRef->archive->read(*fesRef->entry));
    std::vector<std::string> lines;
    ObjectTable obj = parseObjects(text, lines);
    auto types = defineTypes(lines);
    auto typeOf = [&](const std::string& name) {
        auto it = types.find(baseName(name));
        return it == types.end() ? std::string() : it->second;
    };

    WindowLayout L;
    L.image = stripExt(image);
    L.script = stripExt(script);
    auto frames = frameMap(assets, L.image);
    // Newer scripts name the window image themselves rather than through
    // $str900 (img00), whose file then needn't exist: take the image the window plane is sized by.
    // `$str90N` names the style's image N (img0N); `$str900` is the window image.
    auto fileOf = [&](const std::string& v) {
        std::string f = v.substr(0, v.find(':'));
        if (f.size() >= 7 && f.starts_with("$str90") && f[6] >= '0' && f[6] <= '3') {
            return f[6] == '0' ? L.image : stripExt(images[static_cast<size_t>(f[6] - '0')]);
        }
        return assets.expand(f);
    };
    std::map<std::string, std::map<int32_t, FrameInfo>> otherFrames;
    auto framesOf = [&](const std::string& file) -> const std::map<int32_t, FrameInfo>& {
        if (file == L.image) return frames;
        auto it = otherFrames.find(file);
        if (it == otherFrames.end()) it = otherFrames.emplace(file, frameMap(assets, file)).first;
        return it->second;
    };
    if (frames.empty()) {
        for (const auto& [name, row] : obj) {
            auto sz = row.find("SIZE2");
            if (typeOf(name) == "PLANE" && row.find("PARENT") == row.end() && sz != row.end() && !sz->second.starts_with("$")) {
                L.image = fileOf(sz->second);
                frames = frameMap(assets, L.image);
                break;
            }
        }
    }
    if (frames.empty()) return std::nullopt;
    const FrameInfo canvas = frames[-1];
    L.originX = (screenW - canvas.w) / 2;
    L.originY = screenH - canvas.h;

    // "$str900:3" -> frame id 3 of the window image ($str900 = img00).
    auto frameRef = [&](const std::string& v, FrameInfo& out) {
        size_t colon = v.find(':');
        if (colon == std::string::npos) return false;
        const auto& fm = framesOf(fileOf(v));
        auto it = fm.find(std::atoi(v.c_str() + colon + 1));
        if (it == fm.end()) return false;
        out = it->second;
        return true;
    };
    // Position/size of an object: literal "x,y" (POS/SIZE) or a frame reference (POS2/SIZE2).
    auto rect = [&](const std::map<std::string, std::string>& row, IRect& out) {
        int32_t x = 0, y = 0, w = 0, h = 0;
        FrameInfo f;
        bool havePos = false, haveSize = false;
        if (auto it = row.find("POS"); it != row.end() && pair(it->second, x, y)) havePos = true;
        else if (auto it2 = row.find("POS2"); it2 != row.end() && frameRef(it2->second, f)) x = f.x, y = f.y, havePos = true;
        if (auto it = row.find("SIZE"); it != row.end() && pair(it->second, w, h)) haveSize = true;
        else if (auto it2 = row.find("SIZE2"); it2 != row.end() && frameRef(it2->second, f)) w = f.w, h = f.h, haveSize = true;
        if (!havePos || !haveSize) return false;
        out = {L.originX + x, L.originY + y, w, h};
        return true;
    };

    // Window plane: the parentless PLANE; face plane: a PLANE whose parent is the window plane.
    std::string& windowPlane = L.windowPlane;
    std::string facePlane;
    for (const auto& [name, row] : obj) {
        if (typeOf(name) != "PLANE") continue;
        if (row.find("PARENT") == row.end()) {
            if (windowPlane.empty()) windowPlane = name;
        } else if (facePlane.empty()) {
            facePlane = name;
        }
    }
    bool faceBase = false;  // the face base point came from a window-image object outside any plane
    std::vector<std::pair<int32_t, int32_t>> backgrounds;  // (priority, frame id)
    for (const auto& [name, row] : obj) {
        auto file = row.find("FILE");
        if (file == row.end() || fileOf(file->second) != L.image) continue;
        auto id = row.find("ID.0");
        auto pl = row.find("PL");
        auto pri = row.find("PRI");
        if (id == row.end()) continue;
        if (pl == row.end()) {
            // Window-image object outside any plane: the face base point (fbase / img_facebg).
            auto it = frames.find(std::atoi(id->second.c_str()));
            if (it != frames.end()) L.faceX = it->second.x, L.faceY = it->second.y, faceBase = true;
        } else if (pl->second == windowPlane && typeOf(name) == "IMAGE" && pri != row.end()) {
            // Initially visible window-image layers on the window plane form the background.
            auto disp = row.find("DISP");
            if (disp == row.end() || std::atoi(disp->second.c_str()) != 0) {
                backgrounds.emplace_back(std::atoi(pri->second.c_str()), std::atoi(id->second.c_str()));
            }
        }
    }
    std::sort(backgrounds.begin(), backgrounds.end());
    for (const auto& [pri, id] : backgrounds) L.backgroundFrames.push_back(id);
    L.nameImage = typeOf("name") == "IMAGE";
    L.nameText = typeOf("name") == "STRING" || typeOf("name") == "NVSTRING";
    // The engine does not look the text box up: sscript hands the message to the script's
    // MES_SETSTRING in $str1000 (and the sub language's in $str1002), and the script appends it to
    // its text object (`str apend $str1000`; Fruit's novel windows `nvstr`). That object is the box.
    std::string textObj;
    {
        bool inSet = false;
        for (const auto& line : lines) {
            if (line.starts_with("#")) {
                inSet = line.starts_with("#MES_SETSTRING");
                continue;
            }
            auto tok = split(line);
            if (inSet && tok.size() >= 3 && tok[1] == "apend" && line.find("$str1000") != std::string::npos) {
                textObj = tok[0];
                break;
            }
        }
    }
    std::string fallback;  // a script without that line: the STRING other than `name`
    for (const auto& [name, row] : obj) {
        const std::string type = typeOf(name);
        if (type != "STRING" && type != "NVSTRING") continue;
        IRect r;
        if (!rect(row, r)) continue;
        if (name == "name") {
            L.name = r;
            continue;
        }
        if (!textObj.empty() && name != textObj) continue;
        L.text = r;
        fallback = name;
        if (type == "NVSTRING") {
            L.novel = true;
            if (auto d = row.find("DIV"); d != row.end()) L.nameColumn = std::atoi(d->second.c_str());
        }
    }
    if (textObj.empty()) textObj = fallback.empty() ? "str" : fallback;
    L.textObject = textObj;

    // Face plane: positioned at a window-image frame, sized like the face mask image's first frame.
    if (!facePlane.empty()) {
        FrameInfo fpl;
        if (frameRef(obj[facePlane]["POS2"], fpl)) {
            L.faceClip = {L.originX + fpl.x, L.originY + fpl.y, fpl.w, fpl.h};
            // The mask: the MASK object of the face plane (`fmask $str902 ...`), else img01.
            std::string maskFile = stripExt(images[1]);
            for (const auto& [name, row] : obj) {
                auto pl = row.find("PL"), file = row.find("FILE");
                if (row.count("MASK") && pl != row.end() && pl->second == facePlane && file != row.end()) maskFile = fileOf(file->second);
            }
            const auto& mask = framesOf(maskFile);
            auto m0 = mask.lower_bound(0);
            // A mask frame that lies wholly outside the plane leaves the face unmasked within the
            // plane: Amakano 1's sys_meswnd_fmask1 frame sits at (640,74) of its canvas, past the
            // 220x240 face plane, and its faces show whole at the window's left on PC (fitted to
            // the PC screenshots; the MASK object's own drawing is not decoded).
            if (m0 != mask.end()) {
                const FrameInfo& f = m0->second;
                if (f.x >= fpl.w || f.y >= fpl.h || f.x + f.w <= 0 || f.y + f.h <= 0) m0 = mask.end();
            }
            if (m0 != mask.end()) {
                L.faceMask = maskFile;
                L.faceMaskId = m0->first;
                // The mask frame's own offset within its canvas shifts the visible area.
                L.faceClip.x += m0->second.x;
                L.faceClip.y += m0->second.y;
                L.faceClip.w = m0->second.w, L.faceClip.h = m0->second.h;
            }
        }
    }
    // Otherwise the face objects' own position in the face plane (`face[0] getpos` -> \120 \121):
    // a frame of one of the style's images (Cyanotype: `face $str902:1`, the face mask's centre mark).
    if (!faceBase && !facePlane.empty()) {
        auto fr = obj.find("face");
        if (fr != obj.end()) {
            auto v = fr->second.find("POS2");
            FrameInfo ff, fpl;
            bool have = false;
            if (v != fr->second.end()) have = frameRef(v->second, ff);
            // A frame the image lacks leaves the object at its plane's origin: Amakano 1's
            // `face $str902:1` (its mask image has only frame 0).
            if (frameRef(obj[facePlane]["POS2"], fpl)) L.faceX = fpl.x + (have ? ff.x : 0), L.faceY = fpl.y + (have ? ff.y : 0);
        }
    }
    // Face anchor: the face base point, shifted by MES_SETFACE's y offset ("face[\0] pos ... -160"),
    // and the attribute point the face image is re-based on (`face[\0] attr 100 850 851`).
    int32_t faceShiftY = -160;
    for (const auto& line : lines) {
        auto tok = split(line);
        if (tok.size() >= 4 && tok[0].starts_with("face[") && tok[1] == "pos") {
            // The y expression's literal terms: `\121+\3-160` (Grisaia), `\121+\3` (Cyanotype),
            // `\121+\3+60` (Amakano 2).
            faceShiftY = 0;
            const std::string& y = tok[3];
            for (size_t i = 0; i < y.size();) {
                size_t j = y.find_first_of("+-", i + 1);
                if (j == std::string::npos) j = y.size();
                std::string term = y.substr(i, j - i);
                size_t d = term.find_first_not_of("+-");
                if (d != std::string::npos && std::isdigit(static_cast<unsigned char>(term[d]))) {
                    faceShiftY += (term[0] == '-' ? -1 : 1) * std::atoi(term.c_str() + d);
                }
                i = j;
            }
        }
        if (tok.size() >= 3 && tok[0].starts_with("face[") && tok[1] == "attr") L.faceAttr = std::atoi(tok[2].c_str());
    }
    L.faceX += L.originX;
    L.faceY += L.originY + faceShiftY;

    // Runtime layout statements the window script executes: `str layout ...`, `name layout ...`,
    // and `name pos x y` / `name pos2 $str900 <frame>` which move the name box.
    for (const auto& line : lines) {
        auto tok = split(line);
        if (tok.size() >= 4 && tok[0] == "name" && tok[1] == "pos") {
            L.name.x = L.originX + std::atoi(tok[2].c_str());
            L.name.y = L.originY + std::atoi(tok[3].c_str());
            continue;
        }
        if (tok.size() >= 4 && tok[0] == "name" && tok[1] == "pos2") {
            auto it = frames.find(std::atoi(tok[3].c_str()));
            if (it != frames.end()) L.name.x = L.originX + it->second.x, L.name.y = L.originY + it->second.y;
            continue;
        }
        if (tok.size() >= 5 && tok[0] == textObj && tok[1] == "layout" && tok[2] == "move") {
            L.moveX = std::atoi(tok[3].c_str());
            L.moveY = std::atoi(tok[4].c_str());
            continue;
        }
        if (tok.size() >= 4 && tok[0] == textObj && tok[1] == "layout" && tok[2] == "indent") {
            L.indent = std::atoi(tok[3].c_str());
            continue;
        }
        if (tok.size() >= 4 && tok[0] == textObj && tok[1] == "layout" && tok[2] == "rsize") {
            L.rubySize = std::atoi(tok[3].c_str());
            continue;
        }
        if (tok.size() >= 8 && tok[0] == textObj && tok[1] == "layout" && tok[2] == "size") {
            for (int i = 0; i < 5; ++i) L.fontSizes[i] = std::atoi(tok[3 + i].c_str());
        } else if (tok.size() >= 7 && tok[0] == textObj && tok[1] == "layout" && tok[2] == "margin") {
            L.marginLeft = std::atoi(tok[3].c_str());
            L.marginTop = std::atoi(tok[4].c_str());
            L.marginRight = std::atoi(tok[5].c_str());
            L.lineSpacing = std::max(L.marginTop, 6);
        } else if (tok.size() >= 4 && tok[0] == "name" && tok[1] == "layout" && tok[2] == "size") {
            // The normal (third) size; a single size (`name layout size 26`) is every size.
            L.nameSize = std::atoi(tok[std::min<size_t>(5, tok.size() - 1)].c_str());
        }
    }
    if (L.text.w <= 0) return std::nullopt;  // unrecognised script layout: use the generic window
    return L;
}

}  // namespace

const WindowLayout* WindowTheme::find(const std::string& style) const {
    auto it = styles.find(style);
    if (it != styles.end()) return &it->second;
    it = styles.find("default");
    return it != styles.end() ? &it->second : nullptr;
}

std::string decodeFes(ByteSpan file) {
    ByteReader r(file, "FES header");
    if (r.fixedString(4) != "FES") fail("Not an FE script");
    uint32_t compressed = r.u32();
    uint32_t decompressed = r.u32();
    r.skip(4);
    Bytes text = zlibInflate(r.span(compressed), decompressed);
    std::string s(text.begin(), text.end());
    // Scripts are Shift-JIS, but some were saved as UTF-8 (Amakano 2's translated name entry,
    // Yukikoi's font names and `【$str1001】`); Shift-JIS text with non-ASCII bytes is almost
    // never valid UTF-8. Such a script's lines become Shift-JIS where they can; a line Shift-JIS
    // cannot hold (LSG's Chinese samples) stays UTF-8.
    if (std::none_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; }) || !isValidUtf8(s)) return s;
    std::string out;
    for (size_t pos = 0; pos <= s.size();) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) nl = s.size();
        std::string line = s.substr(pos, nl - pos);
        std::string sjis = utf8ToSjis(line);
        out += sjisToUtf8(sjis) == line ? sjis : line;
        if (nl < s.size()) out += '\n';
        pos = nl + 1;
    }
    return out;
}

WindowTheme loadWindowTheme(const AssetIndex& assets, int32_t screenWidth, int32_t screenHeight) {
    WindowTheme theme;
    // The theme file is selected by the engine flow (sysstr/wndtheme). Rather than depend on a
    // per-game name, use the XML asset that has theme structure: <style><mes><script>...</script>.
    std::vector<std::string> candidates;
    for (const auto& archive : assets.archives()) {
        for (const auto& entry : archive->entries()) {
            std::string lower = AssetIndex::normalizeName(entry.name);
            if (lower.ends_with(".xml")) candidates.push_back(entry.name);
        }
    }
    for (const auto& name : candidates) {
        auto ref = assets.find(name);
        if (!ref) continue;
        Bytes xml = ref->archive->read(*ref->entry);
        std::string text = decodeGameText(std::string_view(reinterpret_cast<const char*>(xml.data()), xml.size()));
        if (text.find("<mes>") == std::string::npos || text.find("<script>") == std::string::npos) continue;
        auto root = parseXml(text);
        const XmlNode* doc = root->child("document");
        if (!doc) continue;
        for (const auto& style : doc->children) {
            const XmlNode* mes = style->child("mes");
            if (!mes) continue;
            try {
                std::array<std::string, 4> images;
                for (size_t i = 0; i < images.size(); ++i) images[i] = mes->value("img0" + std::to_string(i)).value_or("");
                if (auto L = buildLayout(assets, mes->value("script").value_or(""), images, screenWidth, screenHeight)) {
                    for (int i = 0; i < 3; ++i) {
                        L->images[i] = stripExt(mes->value("img0" + std::to_string(i)).value_or(""));
                    }
                    // The style's message log screen: its script and images ($str910.. in the script).
                    if (const XmlNode* log = style->child("log")) {
                        L->logScript = stripExt(log->value("script").value_or(""));
                        for (int i = 0; i < 3; ++i) {
                            L->logImages[i] = stripExt(log->value("img0" + std::to_string(i)).value_or(""));
                        }
                    }
                    // The style's choice screen (`fselect`): script and images ($str920..).
                    if (const XmlNode* sel = style->child("fselect")) {
                        L->selectScript = stripExt(sel->value("script").value_or(""));
                        for (int i = 0; i < 4; ++i) {
                            L->selectImages[i] = stripExt(sel->value("img0" + std::to_string(i)).value_or(""));
                        }
                    }
                    for (int i = 0; i < 4; ++i) {
                        if (const XmlNode* ex = style->child("ex" + std::to_string(i))) {
                            L->sideScripts[i] = stripExt(ex->value("script").value_or(""));
                        }
                    }
                    if (const XmlNode* btn = style->child("btn")) {
                        for (int i = 0; i < 4; ++i) {
                            if (const XmlNode* ex = btn->child("ex" + std::to_string(i))) {
                                L->extraScripts[i] = stripExt(ex->value("script").value_or(""));
                                for (int k = 0; k < 4; ++k) {
                                    L->extraImages[i][k] = stripExt(ex->value("img0" + std::to_string(k)).value_or(""));
                                }
                            }
                        }
                    }
                    theme.styles[style->name] = *L;
                }
            } catch (const Error&) {
            }
        }
        if (!theme.styles.empty()) break;
    }
    return theme;
}

}  // namespace nyan::cs2
